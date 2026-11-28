#!/usr/bin/env python3
"""The README's notebook, in CI: the duckdb Python client, the latest build of the extension (loaded
unsigned), a credential_chain secret signed in through GitHub's OIDC token (workload identity, no
token pasted, no Azure CLI), and the nem model's Ossie YAML from the lakehouse's Context folder,
which names its lakehouse, so the view attaches OneLake itself. Every statement is con.sql(...)
and .show(), as the notebook has them. Run by the Test workflow:

    AZURE_FEDERATED_TOKEN_FILE=... python test/integrations/onelake_notebook.py <dax_sql.duckdb_extension>

WS_ID and LH_ID name the workspace and the lakehouse. The YAML deployed there names no lakehouse
yet (its DirectLake expression is filled in at deploy), so its OneLake URL is set to WS_ID/LH_ID
in a local copy, as the deployed model has it in Fabric.
"""
import os
import re
import sys
import tempfile

import duckdb

EXT = sys.argv[1]
WS, LH = os.environ["WS_ID"], os.environ["LH_ID"]
FOLDER = f"abfss://{WS}@onelake.dfs.fabric.microsoft.com/{LH}/Context/Semantic Views/nem"


def check(name, ok, detail=""):
    if not ok:
        sys.exit(f"FAIL {name} {detail}")
    print(f"ok   {name}")


con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
con.sql(f"LOAD '{EXT}'")
con.sql("CREATE OR REPLACE SECRET onelake (TYPE azure, PROVIDER credential_chain, CHAIN 'workload_identity')")

# The YAML from the lakehouse, its OneLake URL naming the lakehouse
text = con.sql(f"SELECT content FROM read_text('{FOLDER}/nem.yaml')").fetchone()[0]
url = re.compile(r"(https://onelake\.dfs\.fabric\.microsoft\.com/)[^/\"'\s]+/[^/\"'\s)]+")
check("the YAML has a OneLake URL", url.search(text) is not None,
      "\n" + "\n".join(line for line in text.splitlines() if "onelake" in line.lower() or "expression" in line.lower())[:2000])
text = url.sub(lambda m: f"{m.group(1)}{WS}/{LH}", text)
path = os.path.join(tempfile.mkdtemp(), "nem.yaml")
with open(path, "w", encoding="utf-8") as f:
    f.write(text)

con.sql(f"CREATE OR REPLACE SEMANTIC VIEW nem FROM '{path}'").show()
views = con.sql("SELECT view_name, tables_from, data_as_of FROM (SHOW SEMANTIC VIEWS)")
views.show(max_width=200)
tables_from = views.fetchall()[0][1]
check("the view attached OneLake itself", tables_from.startswith("OneLake "), tables_from)

con.sql("DESCRIBE nem").show()
con.sql("DESCRIBE SEMANTIC VIEW nem").show()

by_region = """SELECT dim_region.Region, AGG("Registered units") AS units, AGG("Registered MW") AS mw
               FROM nem GROUP BY ALL ORDER BY ALL"""
con.sql(by_region).show()
check("con.sql == con.execute", con.sql(by_region).fetchall() == con.execute(by_region).fetchall())

# The README's query: MWh by state and year, a window over the model query
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

# The folder form, as Fabric keeps a semantic view (every .yaml merged), read straight from OneLake
con.sql(f"CREATE OR REPLACE SEMANTIC VIEW nem_folder FROM '{FOLDER}'").show()
con.sql("SHOW SEMANTIC METRICS IN nem_folder").show()
print("all ok")
