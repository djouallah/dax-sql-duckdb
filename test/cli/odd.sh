#!/usr/bin/env bash
# The odd things people do with the extension in the DuckDB CLI, over dax-sql's contoso model and
# its data (no OneLake): every output mode, a script piped in or with Windows line endings, dot
# commands around semantic views, .bail off past an error, PREPARE, a database file reopened,
# read-only and without the extension. Run by the Test workflow:
#
#     test/cli/odd.sh <duckdb CLI> <dax_sql.duckdb_extension>
#
# Each case runs whatever failed before it; the run fails at the end, listing what failed.
set -uo pipefail
DUCKDB=$1
EXT=$2
W=$(mktemp -d)
FAILED=()
MODEL=third_party/dax-sql/conformance/models/contoso/model.bim
Q='SELECT Product.Color, AGG(Orders) AS orders, AGG("Sales Amount") AS amount FROM contoso GROUP BY ALL ORDER BY Color'

# The tables of test/cli/contoso.sql, and the view over them
grep '^CREATE TABLE' test/cli/contoso.sql > "$W/tables.sql"
# CREATE's row not shown: what a case prints is its own commands' rows
{ cat "$W/tables.sql"; echo ".output /dev/null"; echo "CREATE SEMANTIC VIEW contoso FROM '$MODEL';"; echo ".output"; } > "$W/setup.sql"
duck() { "$DUCKDB" -unsigned "$@"; }
# setup [database] [flags] -c ...: the database and the flags first, then LOAD and the view, then the commands
setup() {
  local first=()
  while [ $# -gt 0 ] && [ "$1" != -c ]; do first+=("$1"); shift; done
  duck "${first[@]}" -bail -c "LOAD '$EXT'" -c ".read $W/setup.sql" "$@"
}

# case <name> <command...>: the command must succeed; its output is shown
case_() {
  local name=$1
  shift
  echo "=== $name"
  if "$@"; then echo "ok   $name"; else echo "FAIL $name"; FAILED+=("$name"); fi
}

# The expected rows, as CSV
EXPECTED=$(setup -csv -c "$Q")
echo "$EXPECTED"
test -n "$EXPECTED" || { echo "FAIL no base answer"; exit 1; }

same_csv() { [ "$(setup -csv "$@" -c "$Q")" = "$EXPECTED" ]; }
case_ "-csv" same_csv
json() { setup -json -c "$Q" | jq -e 'length > 0 and (.[0] | has("Color") and has("orders"))'; }
case_ "-json, read back by jq" json
mode() { setup -c ".mode $1" -c "$Q" -c "SHOW SEMANTIC VIEWS" -c "DESCRIBE contoso" >/dev/null; }
for m in box duckbox markdown line list json jsonlines html latex insert table trash; do
  case_ ".mode $m" mode "$m"
done
case_ ".headers off, .nullvalue, .maxrows 2, .timer on, .changes on, .echo on" \
  setup -c ".headers off" -c ".nullvalue NULL" -c ".maxrows 2" -c ".timer on" -c ".changes on" -c ".echo on" \
  -c "$Q" -c "SHOW SEMANTIC METRICS IN contoso"

piped() { { echo "LOAD '$EXT';"; cat "$W/setup.sql"; echo "$Q;"; } | duck -bail -csv | grep -q Red; }
case_ "a script piped into the CLI" piped

crlf() {
  { echo "LOAD '$EXT';"; cat "$W/setup.sql"; echo "$Q;"; echo "SHOW SEMANTIC VIEWS;"; } | sed 's/$/\r/' > "$W/crlf.sql"
  duck -bail -csv -c ".read $W/crlf.sql" | grep -q Red
}
case_ "a script with Windows line endings (.read)" crlf

oneline() {
  duck -bail -c "LOAD '$EXT'; $(tr '\n' ' ' < "$W/tables.sql") CREATE SEMANTIC VIEW contoso FROM '$MODEL'; $Q; SHOW SEMANTIC VIEWS; DROP SEMANTIC VIEW contoso;"
}
case_ "everything on one line" oneline

multiline() {
  duck -bail -c "LOAD '$EXT'" -c ".read $W/tables.sql" -c "CREATE
    OR REPLACE
    SEMANTIC VIEW
    contoso
    FROM
    '$MODEL'" -c "SELECT
      Product.Color,
      AGG(Orders) AS orders
    FROM contoso
    GROUP BY ALL"
}
case_ "a statement over many lines" multiline

dots() { setup -c ".tables" -c ".schema" -c ".databases" -c "SHOW ALL TABLES" -c "SUMMARIZE $Q"; }
case_ ".tables, .schema, .databases, SHOW ALL TABLES, SUMMARIZE" dots

output() { setup -csv -c ".output $W/out.csv" -c "$Q" -c ".output" && [ "$(cat "$W/out.csv")" = "$EXPECTED" ]; }
case_ ".output to a file" output

bail_off() {
  setup -csv -c ".bail off" -c "SELECT AGG(NoSuchMeasure) FROM contoso" -c "SELECT 1 FROM nosuch_view" \
    -c "$Q" 2>"$W/err.txt" | grep -q Red && grep -qi NoSuchMeasure "$W/err.txt"
}
case_ ".bail off: an error, then the next query answers" bail_off

prepared() { [ "$(setup -csv -c "PREPARE p AS $Q" -c "EXECUTE p")" = "$EXPECTED" ]; }
case_ "PREPARE and EXECUTE" prepared

# A database file
file() {
  rm -f "$W/shop.duckdb"
  setup "$W/shop.duckdb" -c "CREATE VIEW orders_by_colour AS $Q" || return 1
  echo "--- opened again"
  [ "$(duck -bail -csv "$W/shop.duckdb" -c "LOAD '$EXT'" -c "$Q")" = "$EXPECTED" ] || return 1
  echo "--- read-only"
  [ "$(duck -bail -csv -readonly "$W/shop.duckdb" -c "LOAD '$EXT'" -c "$Q")" = "$EXPECTED" ] || return 1
  echo "--- without the extension: its tables, and a DuckDB view over a model query"
  [ "$(duck -bail -csv "$W/shop.duckdb" -c "SELECT * FROM orders_by_colour")" = "$EXPECTED" ] || return 1
  "$DUCKDB" -bail -csv "$W/shop.duckdb" -c 'SELECT count(*) FROM "Sales"' || return 1
  echo "--- without the extension, a model query fails as an error, not a crash"
  "$DUCKDB" -bail "$W/shop.duckdb" -c "$Q"
  local code=$?
  [ "$code" -eq 1 ] || { echo "exit $code"; return 1; }
}
case_ "a database file: again, read-only, without the extension" file

readonly_create() {
  local out
  out=$(duck -csv -readonly "$W/shop.duckdb" -c ".bail off" -c "LOAD '$EXT'" -c "CREATE SEMANTIC VIEW x FROM '$MODEL'" -c "$Q" 2>&1)
  echo "$out"
  echo "$out" | grep -qi "read" && echo "$out" | grep -q Red && ! echo "$out" | grep -q INTERNAL
}
case_ "read-only: CREATE fails, saying so, and queries still answer" readonly_create

echo
if [ ${#FAILED[@]} -gt 0 ]; then
  printf 'FAILED: %s\n' "${FAILED[@]}"
  exit 1
fi
echo "all ok"
