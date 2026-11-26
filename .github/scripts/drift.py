#!/usr/bin/env python3
"""Checks that the extension rewrites each query to the SQL dax-sql gives for it in Node
(drift.mjs): dax_sql_rewrite() in Python duckdb with the built extension, compared byte for byte.

    drift.py <dax_sql.duckdb_extension> <view>=<model file> ... -- <queries.sql> ...

The views are created here first, so each is framed on the data (a Direct Lake one: its tables
pinned, its relationships' keys checked). Their frames go to drift.mjs, which the script runs, so
the glue in Node writes its SQL from the same frame.

DRIFT_SETUP, when set, is SQL run first (the Azure secret a Direct Lake view needs).
"""
import json
import os
import subprocess
import sys

import duckdb


def main():
    extension, args = sys.argv[1], sys.argv[2:]
    split = args.index("--")
    views, files = args[:split], args[split + 1:]
    con = duckdb.connect(":memory:", config={"allow_unsigned_extensions": "true"})
    con.execute(f"LOAD '{extension}'")
    if os.environ.get("DRIFT_SETUP"):
        con.execute(os.environ["DRIFT_SETUP"])
    for v in views:
        name, path = v.split("=", 1)
        con.execute(f"CREATE SEMANTIC VIEW {name} FROM '{path}'")
    facts = {}
    for v in views:
        name = v.split("=", 1)[0]
        facts[name] = json.loads(con.execute("SELECT dax_sql_facts(?)", [name]).fetchone()[0])
        print(f"{name}: {con.execute('SELECT data_as_of FROM (SHOW SEMANTIC VIEWS) WHERE view_name = ?', [name]).fetchone()[0]}")
    with open("drift_facts.json", "w", encoding="utf-8") as f:
        json.dump(facts, f)
    subprocess.run(["node", os.path.join(os.path.dirname(__file__), "drift.mjs"), "drift.json",
                    "--facts", "drift_facts.json", *views, "--", *files], check=True)
    expected = json.loads(open("drift.json", encoding="utf-8").read())
    drifted = 0
    for item in expected:
        got = con.execute("SELECT dax_sql_rewrite(?)", [item["query"]]).fetchone()[0]
        if got != item["sql"]:
            drifted += 1
            print(f"DRIFT\n{item['query']}\n--- dax-sql in Node:\n{item['sql']}\n--- the extension:\n{got}\n", flush=True)
    print(f"{len(expected) - drifted} of {len(expected)} queries rewritten as dax-sql rewrites them")
    sys.exit(1 if drifted else 0)


if __name__ == "__main__":
    main()
