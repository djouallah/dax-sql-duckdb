#!/usr/bin/env python3
"""The extension through the duckdb Python client, as a notebook uses it: con.sql(...) and .show()
for every statement, the way test/integrations/ossie.ipynb reads, over dax-sql's contoso model and
its data (no OneLake). Run by the Test workflow:

    python test/integrations/python_client.py <dax_sql.duckdb_extension>

The Python client binds a SELECT before it runs it (ClientContext::BindStatement, which takes no
rebind), and reads its text again as a subquery when it is shown: a model query must be compiled
as it is parsed (#1), and the extension's statements must be ones a client runs at once (#2).
Each check fails with the client's own error.
"""
import sys
import traceback

import duckdb
import duckdb._relation as _relation
import duckdb._seam as _seam

EXT = sys.argv[1]

# What the client sends, printed when a check fails
SENT = []
_submit, _bind = _seam.submit, _seam.bind


def _spy_submit(con, sql, values, *args, **kwargs):
    SENT.append(("submit", sql))
    return _submit(con, sql, values, *args, **kwargs)


def _spy_bind(con, sql, *args, **kwargs):
    SENT.append(("bind", sql if isinstance(sql, str) else f"<parsed> {sql.query}"))
    return _bind(con, sql, *args, **kwargs)


_seam.submit = _relation._seam.submit = _spy_submit
_seam.bind = _relation._seam.bind = _spy_bind


def _report(kind, error, trace):
    traceback.print_exception(kind, error, trace)
    for kind, sql in SENT[-4:]:
        print(f"===== {kind}\n{sql}")
        if kind == "submit" or not sql.startswith("<parsed>"):
            try:
                print("----- dax_sql_rewrite\n" + con.execute("SELECT dax_sql_rewrite(?)", [sql]).fetchone()[0])
            except Exception as error:  # noqa: BLE001
                print(f"----- dax_sql_rewrite failed: {error}")


sys.excepthook = _report
DATA = "third_party/dax-sql/conformance/models/contoso/data"
MODEL = "third_party/dax-sql/conformance/models/contoso/model.bim"
TABLES = ["Sales", "Product", "Category", "Customer", "Date", "Returns", "CityTarget", "Staff"]

con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
con.sql(f"LOAD '{EXT}'")
for table in TABLES:
    types = ", types = {'Amount': 'DECIMAL(18,4)'}" if table == "Sales" else ""
    con.sql(f"""CREATE TABLE "{table}" AS SELECT * FROM read_csv('{DATA}/{table}.csv', header = true{types})""")


def check(name, got, expected):
    if got != expected:
        sys.exit(f"FAIL {name}: got {got!r}, expected {expected!r}")
    print(f"ok   {name}")


# The extension's statements: run at once, their rows kept and shown (#2)
con.sql(f"CREATE OR REPLACE SEMANTIC VIEW contoso FROM '{MODEL}'").show()
con.sql(f"CREATE SEMANTIC VIEW IF NOT EXISTS contoso FROM '{MODEL}'").show()
views = con.sql("SHOW SEMANTIC VIEWS")
views.show()
check("SHOW SEMANTIC VIEWS", [r[2] for r in views.fetchall()], ["contoso"])
con.sql("SHOW SEMANTIC METRICS IN contoso").show()
con.sql("REFRESH SEMANTIC VIEW contoso").show()

# Model queries through con.sql: bound before they run (#1)
query = """SELECT Product.Color, AGG(Orders) AS orders, AGG("Sales Amount") AS amount
           FROM contoso GROUP BY ALL ORDER BY Color"""
relation = con.sql(query)
relation.show()
check("con.sql == con.execute", relation.fetchall(), con.execute(query).fetchall())
check("columns", relation.columns, ["Color", "orders", "amount"])

with_cte = """WITH m AS (SELECT Category.Category, AGG(Quantity) AS q FROM contoso GROUP BY ALL)
              SELECT m.*, c."CategoryKey" FROM m JOIN "Category" c USING ("Category") ORDER BY ALL"""
con.sql(with_cte).show()
check("a model query in a CTE", con.sql(with_cte).fetchall(), con.execute(with_cte).fetchall())

described = con.sql("DESCRIBE contoso")
described.show()
check("DESCRIBE has the measure", "Orders" in [r[0] for r in described.fetchall()], True)

# A relation over a model query, built on further in Python
con.sql(query).filter("orders > 0").order("amount DESC").limit(3).show()

con.sql("DROP SEMANTIC VIEW contoso").show()
check("dropped", con.sql("SHOW SEMANTIC VIEWS").fetchall(), [])
print("all ok")
