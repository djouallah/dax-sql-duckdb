#!/usr/bin/env python3
"""The odd things people do with a semantic view from Python, over dax-sql's contoso model and its
data (no OneLake): keywords in any case and comments, several statements in one string, names that
need quotes, model queries inside other SQL and inside DuckDB's own statements, data changing
under a view, threads, transactions, a database file closed, reopened and read-only, pandas
DataFrames as the model's tables, and errors that must not break the connection. Run by the Test
workflow:

    python test/integrations/python_odd.py <dax_sql.duckdb_extension>

Each scenario runs whatever failed before it; the run fails at the end, listing what failed.
"""
import concurrent.futures
import os
import sys
import tempfile
import traceback

import duckdb

EXT = sys.argv[1]
DATA = "third_party/dax-sql/conformance/models/contoso/data"
MODEL = "third_party/dax-sql/conformance/models/contoso/model.bim"
TABLES = ["Sales", "Product", "Category", "Customer", "Date", "Returns", "CityTarget", "Staff"]
UNSIGNED = {"allow_unsigned_extensions": "true"}
TMP = tempfile.mkdtemp()
FAILED = []
# What fails today for a reason filed: reported, not failing the run, and said when it works
KNOWN = {}
NOW_WORKS = []


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
        failed = len(FAILED)
        try:
            f()
        except Exception as error:  # noqa: BLE001
            traceback.print_exc()
            check(title, False, f"{type(error).__name__}: {str(error).splitlines()[0]}")
        if title in KNOWN:
            if len(FAILED) > failed:
                del FAILED[failed:]
                print(f"known: fails until {KNOWN[title]}")
            else:
                NOW_WORKS.append(f"{title} ({KNOWN[title]})")
        return f

    return run


def tables(con):
    for table in TABLES:
        types = ", types = {'Amount': 'DECIMAL(18,4)'}" if table == "Sales" else ""
        con.sql(f"""CREATE TABLE "{table}" AS SELECT * FROM read_csv('{DATA}/{table}.csv', header = true{types})""")


def connect(path=":memory:", view=True, data=True, **kwargs):
    con = duckdb.connect(path, config=UNSIGNED, **kwargs)
    con.sql(f"LOAD '{EXT}'")
    if data:
        tables(con)
    if view:
        con.sql(f"CREATE OR REPLACE SEMANTIC VIEW contoso FROM '{MODEL}'")
    return con


def one(con, sql):
    return con.sql(sql).fetchall()[0][0]


def names(con):
    return [r[0] for r in con.sql("SELECT view_name FROM (SHOW SEMANTIC VIEWS) ORDER BY ALL").fetchall()]


def fails(name, run, *words):
    """run() must raise a duckdb error whose text names each of words."""
    try:
        run()
        check(name, False, "it answered")
    except duckdb.Error as error:
        text = str(error)
        print(f"     ({text.splitlines()[0][:200]})")
        missing = [w for w in words if w.lower() not in text.lower()]
        check(name, not missing and "INTERNAL" not in text, f"the error does not say {missing}" if missing else text)


Q = """SELECT Product.Color, AGG(Orders) AS orders, AGG("Sales Amount") AS amount
       FROM contoso GROUP BY ALL ORDER BY Color"""
ORDERS = "SELECT AGG(Orders) FROM contoso"
base = connect()
BASE = base.sql(Q).fetchall()
ROWS = one(base, 'SELECT count(*) FROM "Sales"')
print(f"the base answer: {BASE}")


@scenario("keywords in any case, comments, whitespace and semicolons")
def _():
    con = connect(view=False)
    con.sql(f"  create or replace semantic view contoso from '{MODEL}' ;  ")
    con.sql(f"Create Semantic View If Not Exists contoso From '{MODEL}'")
    con.sql(f"-- a comment first\n/* and a block */ CREATE OR REPLACE SEMANTIC VIEW contoso\n\tFROM\n'{MODEL}'")
    check("show semantic views, lower case", names(con) == ["contoso"], names(con))
    commented = """-- the orders by colour
    /* a block comment */ SELECT Product.Color, AGG(Orders) AS orders, AGG("Sales Amount") AS amount -- trailing
    FROM contoso /* here */ GROUP BY ALL ORDER BY Color;"""
    check("comments around and inside a model query", con.sql(commented).fetchall() == BASE)
    check("agg( in lower case", con.sql(Q.replace("AGG(", "agg(")).fetchall() == BASE)
    check("FROM CONTOSO in upper case", con.sql(Q.replace("FROM contoso", "FROM CONTOSO")).fetchall() == BASE)
    check("a measure named in another case", one(con, "SELECT AGG(orders) FROM contoso") == ROWS)
    con.sql("drop semantic view if exists contoso")
    con.sql("DROP SEMANTIC VIEW IF EXISTS contoso")
    check("dropped, lower case, and again IF EXISTS", names(con) == [])


@scenario("several statements in one string")
def _():
    con = connect(view=False)
    con.execute(
        f"CREATE SEMANTIC VIEW a FROM '{MODEL}'; CREATE SEMANTIC VIEW b FROM '{MODEL}'; DROP SEMANTIC VIEW a;"
    ).fetchall()
    check("create, create, drop in one execute", names(con) == ["b"], names(con))
    rows = con.sql(f"CREATE OR REPLACE SEMANTIC VIEW contoso FROM '{MODEL}'; {Q}").fetchall()
    check("create then query in one con.sql: the query's rows", rows == BASE)
    rows = con.sql(f'{ORDERS}; SELECT AGG("Sales Amount") FROM b').fetchall()
    check(
        "two model queries in one string: the last one's rows",
        rows[0][0] == one(con, 'SELECT AGG("Sales Amount") FROM b'),
    )


@scenario("names that need quotes: spaces, a keyword, accents, a schema")
def _():
    con = connect(view=False)
    for name in ['"my view"', '"select"', '"Ventes été"', '"MixedCase"']:
        con.sql(f"CREATE SEMANTIC VIEW {name} FROM '{MODEL}'")
        check(f"a view named {name}", con.sql(Q.replace("FROM contoso", f"FROM {name}")).fetchall() == BASE)
        con.sql(f"DROP SEMANTIC VIEW {name}")
    check("all dropped", names(con) == [], names(con))
    con.sql("CREATE SCHEMA analytics")
    con.sql(f"CREATE SEMANTIC VIEW analytics.contoso FROM '{MODEL}'")
    check(
        "a view in a schema, by its full name",
        con.sql(Q.replace("FROM contoso", "FROM analytics.contoso")).fetchall() == BASE,
    )
    unicode = con.sql('SELECT AGG(Orders) AS "Commandes €", AGG("Sales Amount") AS "金額" FROM analytics.contoso')
    check("aliases in unicode", unicode.columns == ["Commandes €", "金額"], unicode.columns)
    con.sql("DROP SEMANTIC VIEW analytics.contoso")


@scenario("the user moves to another schema (USE) after creating a view over main's tables")
def _():
    con = connect()
    con.sql("CREATE SCHEMA scratch")
    con.sql("USE scratch")
    check("the view by its name from another schema", con.sql(Q).fetchall() == BASE)
    con.sql("USE main")


@scenario("a model query inside other SQL: IN, UNION ALL, scalar subqueries, PIVOT, a window, ORDER BY a measure")
def _():
    con = connect()
    with_orders = [c for c, o, _ in BASE if o]
    in_sub = con.sql(
        """SELECT count(*) FROM "Product" WHERE "Color" IN
           (SELECT Color FROM (SELECT Product.Color, AGG(Orders) AS o FROM contoso GROUP BY ALL) WHERE o > 0)"""
    ).fetchall()[0][0]
    plain = con.execute('SELECT count(*) FROM "Product" WHERE list_contains(?, "Color")', [with_orders]).fetchall()[0][
        0
    ]
    check("a model query in IN (...)", in_sub == plain, f"{in_sub} vs {plain}")
    union = con.sql(
        f"""SELECT 'orders' AS k, AGG(Orders) AS v FROM contoso
                        UNION ALL SELECT 'quantity', AGG(Quantity) FROM contoso ORDER BY k"""
    ).fetchall()
    qty = one(con, 'SELECT sum("Qty") FROM "Sales"')
    check("UNION ALL of two model queries", union == [("orders", ROWS), ("quantity", qty)], union)
    scalar = con.sql(f'SELECT ({ORDERS}) AS o, (SELECT count(*) FROM "Sales") AS s').fetchall()[0]
    check("a model query as a scalar subquery", scalar == (ROWS, ROWS), scalar)
    pivot = con.sql(f"PIVOT ({Q}) ON Color USING sum(orders)").fetchall()
    check("PIVOT over a model query", len(pivot) == 1)
    ranked = con.sql(f"SELECT Color, rank() OVER (ORDER BY orders DESC) AS r FROM ({Q}) QUALIFY r = 1").fetchall()
    check("a window and QUALIFY over a model query", len(ranked) >= 1)
    top = con.sql(
        "SELECT Product.Color, AGG(Orders) AS orders FROM contoso GROUP BY ALL "
        "ORDER BY orders DESC NULLS LAST, Color LIMIT 2 OFFSET 1"
    ).fetchall()
    expected = sorted([(c, o) for c, o, _ in BASE], key=lambda r: (r[1] is None, -(r[1] or 0), r[0] or ""))[1:3]
    check("ORDER BY a measure, LIMIT, OFFSET", top == expected, f"{top} vs {expected}")
    ratio = con.sql('SELECT AGG("Sales Amount") / AGG(Orders) AS per_order FROM contoso').fetchall()[0][0]
    total = one(con, 'SELECT AGG("Sales Amount") FROM contoso')
    check("an expression of two measures", abs(float(ratio) - float(total) / ROWS) < 1e-9, ratio)


@scenario("DuckDB's own statements around a model query: DESCRIBE, SUMMARIZE, EXPLAIN ANALYZE, COPY, PREPARE, a macro")
def _():
    con = connect()
    described = con.sql(f"DESCRIBE {Q}").fetchall()
    check("DESCRIBE a model query", [r[0] for r in described] == ["Color", "orders", "amount"], described)
    con.sql(f"SUMMARIZE {Q}").show()
    con.sql(f"EXPLAIN ANALYZE {Q}").fetchall()
    out = os.path.join(TMP, "orders.parquet").replace("\\", "/")
    con.sql(f"COPY ({Q}) TO '{out}' (FORMAT parquet)")
    check("COPY a model query to parquet", con.sql(f"SELECT * FROM '{out}' ORDER BY Color").fetchall() == BASE)
    csv = os.path.join(TMP, "orders.csv").replace("\\", "/")
    con.sql(Q).write_csv(csv)
    check("relation.write_csv", len(con.sql(f"SELECT * FROM read_csv('{csv}')").fetchall()) == len(BASE))
    con.sql(f"PREPARE by_colour AS {Q}")
    check("PREPARE and EXECUTE", con.sql("EXECUTE by_colour").fetchall() == BASE)
    con.sql(f"CREATE MACRO by_colour() AS TABLE {Q}")
    check("a table macro over a model query", con.sql("SELECT * FROM by_colour()").fetchall() == BASE)
    con.sql(Q).create("orders_t")
    check("relation.create", con.sql("SELECT * FROM orders_t ORDER BY Color").fetchall() == BASE)
    con.sql(Q).to_view("orders_v")
    check("relation.to_view", con.sql("SELECT * FROM orders_v").fetchall() == BASE)
    check("relation.sql_query() runs again", con.sql(con.sql(Q).sql_query()).fetchall() == BASE)


@scenario("the data changes under the view: rows added, a table dropped and made again")
def _():
    con = connect()
    con.sql('INSERT INTO "Sales" SELECT * FROM "Sales" LIMIT 3')
    check("rows added are counted", one(con, ORDERS) == ROWS + 3, one(con, ORDERS))
    con.sql('DELETE FROM "Sales"')
    check("no rows: no orders", one(con, ORDERS) in (0, None), one(con, ORDERS))
    con.sql('DROP TABLE "Category"')
    fails(
        "a query over a dropped table fails, naming it",
        lambda: con.sql("SELECT Category.Category, AGG(Orders) FROM contoso GROUP BY ALL").fetchall(),
        "Category",
    )
    con.sql(f"""CREATE TABLE "Category" AS SELECT * FROM read_csv('{DATA}/Category.csv', header = true)""")
    con.sql('DROP TABLE "Sales"')
    con.sql(
        f"""CREATE TABLE "Sales" AS SELECT * FROM read_csv('{DATA}/Sales.csv', header = true, types = {{'Amount': 'DECIMAL(18,4)'}})"""
    )
    check("the tables made again: the same answers", con.sql(Q).fetchall() == BASE)


@scenario("threads: eight cursors querying at once")
def _():
    con = connect()

    def work(_):
        cursor = con.cursor()
        return all(cursor.sql(Q).fetchall() == BASE for _ in range(10))

    with concurrent.futures.ThreadPoolExecutor(8) as pool:
        results = list(pool.map(work, range(8)))
    check("80 queries from 8 threads, all the same answer", all(results), results)


@scenario("threads: views created and dropped while others query")
def _():
    con = connect()

    def churn(i):
        cursor = con.cursor()
        for n in range(5):
            cursor.sql(f"CREATE OR REPLACE SEMANTIC VIEW churn_{i} FROM '{MODEL}'")
            ok = cursor.sql(Q.replace("FROM contoso", f"FROM churn_{i}")).fetchall() == BASE
            cursor.sql(f"DROP SEMANTIC VIEW churn_{i}")
            if not ok:
                return False
        return True

    def query(_):
        cursor = con.cursor()
        return all(cursor.sql(Q).fetchall() == BASE for _ in range(10))

    with concurrent.futures.ThreadPoolExecutor(8) as pool:
        churned = list(pool.map(churn, range(3)))
        queried = list(pool.map(query, range(5)))
    check("the churning threads' views answered", all(churned), churned)
    check("the querying threads' answers stayed the same", all(queried), queried)
    check("only contoso is left", names(con) == ["contoso"], names(con))


@scenario("a transaction rolled back")
def _():
    con = connect()
    con.sql("BEGIN")
    con.sql(f"CREATE SEMANTIC VIEW t FROM '{MODEL}'")
    check("the view inside its transaction", "t" in names(con))
    con.sql("ROLLBACK")
    check("no view after ROLLBACK", "t" not in names(con), names(con))
    con.sql(f"CREATE OR REPLACE SEMANTIC VIEW t FROM '{MODEL}'")
    con.sql("BEGIN")
    con.sql("DROP SEMANTIC VIEW t")
    con.sql("ROLLBACK")
    check("the view back after a rolled-back DROP", "t" in names(con), names(con))
    con.sql("BEGIN")
    check("a model query inside a transaction", con.sql(Q).fetchall() == BASE)
    con.sql("COMMIT")


@scenario("a database file: closed, opened again, without the extension, read-only, attached")
def _():
    path = os.path.join(TMP, "contoso.duckdb")
    con = connect(path)
    con.sql(f"CREATE VIEW orders_by_colour AS {Q}")
    con.close()
    con = duckdb.connect(path, config=UNSIGNED)
    check("opened without the extension: its tables read", one(con, 'SELECT count(*) FROM "Sales"') == ROWS)
    check("and a DuckDB view made over a model query", con.sql("SELECT * FROM orders_by_colour").fetchall() == BASE)
    con.sql(f"LOAD '{EXT}'")
    check("loaded: the view is back", names(con) == ["contoso"], names(con))
    check("and answers", con.sql(Q).fetchall() == BASE)
    con.close()
    ro = duckdb.connect(path, read_only=True, config=UNSIGNED)
    ro.sql(f"LOAD '{EXT}'")
    check("read-only: the view answers", ro.sql(Q).fetchall() == BASE)
    fails(
        "read-only: CREATE fails, saying so",
        lambda: ro.sql(f"CREATE SEMANTIC VIEW x FROM '{MODEL}'").fetchall(),
        "read",
    )
    check("read-only: still answers after the failed CREATE", ro.sql(Q).fetchall() == BASE)
    ro.close()
    mem = connect(view=False, data=False)
    mem.sql(f"ATTACH '{path}' AS shop (READ_ONLY)")
    check("attached: its tables read", one(mem, 'SELECT count(*) FROM shop."Sales"') == ROWS)
    check(
        "attached: its view answers by its full name",
        mem.sql(Q.replace("FROM contoso", "FROM shop.main.contoso")).fetchall() == BASE,
    )
    mem.close()


@scenario("each database its own views; cursors share them")
def _():
    a, b = connect(), connect(view=False)
    check("another in-memory database has none", names(b) == [], names(b))
    check("a cursor of the first sees the view", a.cursor().sql(Q).fetchall() == BASE)
    b.sql(f"CREATE SEMANTIC VIEW only_b FROM '{MODEL}'")
    check("a view of the second is not the first's", "only_b" not in names(a), names(a))


@scenario("the model's tables are pandas DataFrames")
def _():
    con = duckdb.connect(config=UNSIGNED)
    con.sql(f"LOAD '{EXT}'")
    frames = {t: base.sql(f'SELECT * FROM "{t}"').df() for t in TABLES}
    for table, frame in frames.items():
        con.register(table, frame)
    con.sql(f"CREATE SEMANTIC VIEW contoso FROM '{MODEL}'")
    orders = con.sql("SELECT Product.Color, AGG(Orders) FROM contoso GROUP BY ALL ORDER BY Color").fetchall()
    check("orders by colour over DataFrames", orders == [(c, o) for c, o, _ in BASE], orders)


@scenario("every measure at once, by colour")
def _():
    con = connect()
    metrics = [r[0] for r in con.sql("SELECT name FROM (SHOW SEMANTIC METRICS IN contoso) ORDER BY ALL").fetchall()]
    sql = "SELECT Product.Color, " + ", ".join(f'AGG("{m}") AS "{m}"' for m in metrics) + " FROM contoso GROUP BY ALL"
    rows = con.sql(sql).fetchall()
    check(f"{len(metrics)} measures in one query", len(rows) == len(BASE) and len(rows[0]) == len(metrics) + 1)


@scenario("errors name what is wrong, and the connection goes on")
def _():
    con = connect()
    fails(
        "a measure that is not", lambda: con.sql("SELECT AGG(NoSuchMeasure) FROM contoso").fetchall(), "NoSuchMeasure"
    )
    fails(
        "a table that is not",
        lambda: con.sql("SELECT NoTable.Col, AGG(Orders) FROM contoso GROUP BY ALL").fetchall(),
        "NoTable",
    )
    fails(
        "a model file that is not",
        lambda: con.sql("CREATE SEMANTIC VIEW x FROM 'no/such/model.bim'").fetchall(),
        "no/such/model.bim",
    )
    fails("a view that is not, dropped", lambda: con.sql("DROP SEMANTIC VIEW nosuch").fetchall(), "nosuch")
    fails("a view that is not, refreshed", lambda: con.sql("REFRESH SEMANTIC VIEW nosuch").fetchall(), "nosuch")
    fails("a view that is not, described", lambda: con.sql("DESCRIBE SEMANTIC VIEW nosuch").fetchall(), "nosuch")
    fails("a view created twice", lambda: con.sql(f"CREATE SEMANTIC VIEW contoso FROM '{MODEL}'").fetchall(), "contoso")
    fails(
        "OR REPLACE with IF NOT EXISTS",
        lambda: con.sql(f"CREATE OR REPLACE SEMANTIC VIEW IF NOT EXISTS contoso FROM '{MODEL}'").fetchall(),
        "IF NOT EXISTS",
    )
    check("the connection still answers", con.sql(Q).fetchall() == BASE)
    check("and lists only contoso", names(con) == ["contoso"], names(con))


@scenario("LOAD again, and the view replaced by itself while a relation is open")
def _():
    con = connect()
    relation = con.sql(Q)
    con.sql(f"LOAD '{EXT}'")
    check("LOAD twice keeps the view", names(con) == ["contoso"], names(con))
    con.sql(f"CREATE OR REPLACE SEMANTIC VIEW contoso FROM '{MODEL}'")
    check("a relation made before the replace", relation.fetchall() == BASE)


print()
for title in NOW_WORKS:
    print(f"now works, take it off KNOWN: {title}")
if FAILED:
    sys.exit("FAILED: " + "; ".join(FAILED))
print("all ok")
