#!/usr/bin/env bash
# Runs one of test/onelake/'s SQL files in the DuckDB CLI (./cli/duckdb), its ${...} filled in
# from the environment: EXT, STORAGE_TOKEN, WS_ID and LH_ID only, so
# nothing else in the SQL is touched. The first statement that fails stops it (-bail). The filled-in
# copy holds the token, so it is private to the runner's user and removed afterwards.
#
#   onelake_sql.sh <file.sql> [database file]
set -euo pipefail

sql=$(mktemp --suffix=.sql "${RUNNER_TEMP:-/tmp}/onelake.XXXXXX")
trap 'rm -f "$sql"' EXIT
envsubst '${EXT} ${STORAGE_TOKEN} ${WS_ID} ${LH_ID}' < "$1" > "$sql"
./cli/duckdb -unsigned -bail ${2:+"$2"} -c ".read $sql"
