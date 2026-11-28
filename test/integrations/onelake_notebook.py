#!/usr/bin/env python3
"""The README's notebook, in CI, and what people do around it: the duckdb Python client, the latest
build of the extension (loaded unsigned), a credential_chain secret signed in through GitHub's OIDC
token (workload identity, no token pasted, no Azure CLI), and the nem model's Ossie YAML from the
lakehouse's Context folder, which names its lakehouse, so the view attaches OneLake itself. Every
statement is con.sql(...) and .show(), as a notebook has them. Run by the Test workflow:

    AZURE_FEDERATED_TOKEN_FILE=... STORAGE_TOKEN=... python test/integrations/onelake_notebook.py <dax_sql.duckdb_extension>

WS_ID and LH_ID name the workspace and the lakehouse. The YAML deployed there names no lakehouse
yet (its DirectLake expression is filled in at deploy), so its OneLake URL is set to WS_ID/LH_ID
in a local copy, as the deployed model has it in Fabric. STORAGE_TOKEN is for the catalog a user
attaches on their own, beside the extension's.

Each scenario runs whatever failed before it; the run fails at the end, listing what failed.
"""
import os
import re
import sys
import tempfile
import traceback

import duckdb

EXT = sys.argv[1]
WS, LH = os.environ["WS_ID"], os.environ["LH_ID"]
TOKEN = os.environ["STORAGE_TOKEN"]
FOLDER = f"abfss://{WS}@onelake.dfs.fabric.microsoft.com/{LH}/Context/Semantic Views/nem"
ICEBERG = "https://onelake.table.fabric.microsoft.com/iceberg"
FAILED = []


def check(name, ok, detail=""):
    if ok:
        print(f"ok   {name}")
    else:
        print(f"FAIL {name} {detail}")
        FAILED.append(name)


def scenario(title):
    """A scenario: its exceptions are a failure of its own, and the next one runs."""

    def run(f):
        print(f"\n=== {title}")
        try:
            f()
        except Exception as error:  # noqa: BLE001
            traceback.print_exc()
            check(title, False, f"{type(error).__name__}: {str(error).splitlines()[0]}")
        return f

    return run


def one(sql, params=None):
    return con.execute(sql, params).fetchall()[0][0] if params else con.sql(sql).fetchall()[0][0]


con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
con.sql(f"LOAD '{EXT}'")
con.sql("CREATE OR REPLACE SECRET onelake (TYPE azure, PROVIDER credential_chain, CHAIN 'workload_identity')")

# The YAML from the lakehouse, its OneLake URL naming the lakehouse
text = con.sql(f"SELECT content FROM read_text('{FOLDER}/nem.yaml')").fetchall()[0][0]
# a path segment ends at a slash, a quote, a backslash (a YAML string's escaped quote) or a space
url = re.compile(r"(https://onelake\.dfs\.fabric\.microsoft\.com/)[^/\\\"'\s]+/[^/\\\"'\s)]+")
if not url.search(text):
    sys.exit("FAIL the YAML has no OneLake URL")
text = url.sub(lambda m: f"{m.group(1)}{WS}/{LH}", text)
path = os.path.join(tempfile.mkdtemp(), "nem.yaml")
with open(path, "w", encoding="utf-8") as f:
    f.write(text)

BY_REGION = """SELECT dim_region.Region, AGG("Registered units") AS units, AGG("Registered MW") AS mw
               FROM nem GROUP BY ALL ORDER BY ALL"""
UNITS = 'SELECT AGG("Registered units") FROM nem'


@scenario("the README's notebook")
def _():
    con.sql(f"CREATE OR REPLACE SEMANTIC VIEW nem FROM '{path}'").show()
    views = con.sql("SELECT view_name, tables_from, data_as_of FROM (SHOW SEMANTIC VIEWS)")
    views.show(max_width=200)
    row = views.fetchall()[0]
    check("the view attached OneLake itself", row[1].startswith("OneLake "), row[1])
    check("the view is pinned to its snapshots", row[2].startswith("snapshots of "), row[2])
    con.sql("DESCRIBE nem").show()
    con.sql("DESCRIBE SEMANTIC VIEW nem").show()
    con.sql(BY_REGION).show()
    check("con.sql == con.execute", con.sql(BY_REGION).fetchall() == con.execute(BY_REGION).fetchall())
    by_state_year = """
    WITH g AS (
        SELECT dim_region.State AS state, dim_calendar.year AS year, AGG("Generation MWh") AS mwh
        FROM nem
        GROUP BY ALL
    )
    SELECT state, year, round(mwh) AS mwh,
           round(100 * mwh / sum(mwh) OVER (PARTITION BY year), 1) AS pct_of_year
    FROM g
    ORDER BY year, state
    """
    rows = con.sql(by_state_year)
    rows.show()
    check("MWh by state and year has rows", len(rows.fetchall()) > 0)


@scenario("the user attaches the same lakehouse on their own, beside the extension's catalog")
def _():
    con.sql(
        f"ATTACH '{WS}/{LH}' AS lake (TYPE iceberg, URI '{ICEBERG}', TOKEN '{TOKEN}', ACCESS_DELEGATION_MODE 'none')"
    )
    con.sql("SELECT database_name, type, readonly FROM duckdb_databases() ORDER BY ALL").show()
    check(
        "the model's answer is plain SQL's over the user's catalog",
        one(UNITS) == one("SELECT count(*) FROM lake.mart.dim_duid"),
    )
    by_region_model = con.sql(
        'SELECT dim_duid.Region AS region, round(AGG("Registered MW"), 6) AS mw FROM nem GROUP BY ALL ORDER BY ALL'
    ).fetchall()
    by_region_sql = con.sql(
        'SELECT "Region", round(sum("RegCapMW")::DOUBLE, 6) FROM lake.mart.dim_duid GROUP BY ALL '
        'HAVING sum("RegCapMW") IS NOT NULL ORDER BY ALL'
    ).fetchall()
    check("Registered MW by region is plain SQL's", [r for r in by_region_model if r[1] is not None] == by_region_sql)


@scenario("a model query joined to the user's own lakehouse table")
def _():
    joined = con.sql(
        """WITH g AS (SELECT dim_region.Region, round(AGG("Generation MWh"), 6) AS generation FROM nem GROUP BY ALL)
           SELECT g.Region, generation, count(*) AS units
           FROM g JOIN lake.mart.dim_duid d ON d."Region" = g.Region GROUP BY ALL ORDER BY 1"""
    )
    joined.show()
    check("the join has a row per region", len(joined.fetchall()) > 0)


@scenario("the user's current catalog is their lakehouse (USE lake.mart)")
def _():
    con.sql("USE lake.mart")
    check("the view by its name, from another catalog", one(UNITS) == one("SELECT count(*) FROM lake.mart.dim_duid"))
    check("the view by its full name", one('SELECT AGG("Registered units") FROM memory.main.nem') == one(UNITS))
    con.sql(f"CREATE OR REPLACE SEMANTIC VIEW memory.main.nem_in_use FROM '{path}'").show()
    check("a view created there", one('SELECT AGG("Registered units") FROM memory.main.nem_in_use') == one(UNITS))
    con.sql("SHOW SEMANTIC VIEWS").show(max_width=200)
    con.sql("DROP SEMANTIC VIEW memory.main.nem_in_use").show()
    con.sql("USE memory.main")


@scenario("a local table named like the model's (a stale copy) does not shadow OneLake")
def _():
    before = one(UNITS)
    con.sql("CREATE SCHEMA IF NOT EXISTS memory.mart")
    con.sql("CREATE OR REPLACE TABLE memory.mart.dim_duid AS SELECT * FROM lake.mart.dim_duid LIMIT 1")
    con.sql("USE memory.mart")
    check("the model still reads its lakehouse", one(UNITS) == before, f"{one(UNITS)} vs {before}")
    con.sql("USE memory.main")
    con.sql("DROP TABLE memory.mart.dim_duid")


@scenario("REFRESH: the view framed again; the user's own catalog untouched")
def _():
    before = one(UNITS)
    con.sql("REFRESH SEMANTIC VIEW nem").show(max_width=200)
    check("the answer after REFRESH", one(UNITS) == before)
    check("the user's catalog still attached", one("SELECT count(*) FROM lake.mart.dim_duid") == before)
    as_of = one("SELECT data_as_of FROM (SHOW SEMANTIC VIEWS) WHERE view_name = 'nem'")
    check("still pinned after REFRESH", as_of.startswith("snapshots of "), as_of)


@scenario("the user detaches the extension's catalog; REFRESH attaches it again")
def _():
    internal = one("SELECT database_name FROM duckdb_databases() WHERE database_name LIKE 'dax_sql_onelake_%'")
    con.sql(f'DETACH "{internal}"')
    try:
        one(UNITS)
        check("a query without the catalog fails", False, "it answered")
    except duckdb.Error as error:
        check("a query without the catalog fails", True)
        print(f"     ({str(error).splitlines()[0]})")
    con.sql("REFRESH SEMANTIC VIEW nem").show(max_width=200)
    check("answers again after REFRESH", one(UNITS) == one("SELECT count(*) FROM lake.mart.dim_duid"))


@scenario("two views over one lakehouse: dropping one keeps the other")
def _():
    con.sql(f"CREATE OR REPLACE SEMANTIC VIEW nem_folder FROM '{FOLDER}'").show()
    con.sql("SHOW SEMANTIC METRICS IN nem_folder").show()
    con.sql("DROP SEMANTIC VIEW nem_folder").show()
    check("the other still answers", one(UNITS) == one("SELECT count(*) FROM lake.mart.dim_duid"))


@scenario("parameters, as an app binds them")
def _():
    sql = 'SELECT AGG("Registered units") FROM nem WHERE dim_duid.Region = ?'
    expected = one('SELECT count(*) FROM lake.mart.dim_duid WHERE "Region" = ?', ["NSW1"])
    check("con.execute with a parameter", one(sql, ["NSW1"]) == expected)
    check("con.sql with params", con.sql(sql, params=["NSW1"]).fetchall()[0][0] == expected)


@scenario("a second cursor, as threads in a notebook use")
def _():
    cursor = con.cursor()
    check("the view from a cursor", cursor.sql(BY_REGION).fetchall() == con.sql(BY_REGION).fetchall())


@scenario("pandas, Arrow and relations built on a model query")
def _():
    df = con.sql(BY_REGION).df()
    print(df.head())
    check("a DataFrame", len(df) > 0 and list(df.columns) == ["Region", "units", "mw"])
    check("an Arrow table", con.sql(BY_REGION).arrow().num_rows == len(df))
    top = con.sql(BY_REGION).filter("units > 0").order("mw DESC").limit(2)
    top.show()
    check("filter, order, limit on the relation", len(top.fetchall()) == min(2, len(df)))


@scenario("a DuckDB view and a table made from model queries")
def _():
    con.sql(f"CREATE OR REPLACE VIEW units_by_region AS {BY_REGION}")
    check(
        "a DuckDB view over a model query",
        con.sql("SELECT * FROM units_by_region").fetchall() == con.sql(BY_REGION).fetchall(),
    )
    con.sql(f"CREATE OR REPLACE TABLE units_by_region_t AS {BY_REGION}")
    check(
        "CREATE TABLE AS a model query",
        one("SELECT count(*) FROM units_by_region_t") == len(con.sql(BY_REGION).fetchall()),
    )
    con.sql(f"EXPLAIN {BY_REGION}").show(max_width=200)


print()
if FAILED:
    sys.exit("FAILED: " + "; ".join(FAILED))
print("all ok")
