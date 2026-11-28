#!/usr/bin/env python3
"""The orphan answers change the SQL, never the rows (EXTENSION.md, Facts from the data).

    facts.py <dax_sql.duckdb_extension> <nem .bim path> <queries.sql> ...

Creates the nem view over the live lakehouse, which frames it: its tables pinned to their current
snapshots, its relationships' keys checked there. The view must be framed. Then every SELECT of the
queries files, plus some dashboard-shaped ones over the last three days, runs twice at the same
snapshots: with the orphan answers and without them (dax_sql_set_facts keeps the pins and drops the
answers). The rows must be the same. Each query's time both ways, best of two, and the EXISTS in its
SQL go to the job summary.

SETUP, when set, is SQL run first (the Azure secret); WS_ID and LH_ID name the lakehouse, attached
as ol, with STORAGE_TOKEN, for the queries that read it. A query that fails (the same both ways:
a measure the deployed model lacks) is listed, not counted.
"""
import datetime
import json
import math
import os
import sys
import time

import duckdb


def paragraphs(path):
    text = open(path, encoding="utf-8").read()
    for p in text.replace("\r\n", "\n").split("\n\n"):
        q = "\n".join(l for l in p.split("\n") if not l.lstrip().startswith("--")).strip()
        q = q.split("; --")[0].rstrip().rstrip(";").strip()
        if q:
            yield q


def same(a, b):
    if len(a) != len(b):
        return False
    for ra, rb in zip(a, b):
        if len(ra) != len(rb):
            return False
        for x, y in zip(ra, rb):
            if isinstance(x, float) and isinstance(y, float):
                if math.isnan(x) and math.isnan(y):
                    continue
                if abs(x - y) > 1e-9 * max(1.0, abs(x), abs(y)):
                    return False
            elif x != y:
                return False
    return True


def timed(con, q):
    t = time.perf_counter()
    rows = con.execute(q).fetchall()
    return rows, (time.perf_counter() - t) * 1000


def main():
    extension, bim, files = sys.argv[1], sys.argv[2], sys.argv[3:]
    con = duckdb.connect(":memory:", config={"allow_unsigned_extensions": "true"})
    con.execute(f"LOAD '{extension}'")
    if os.environ.get("SETUP"):
        con.execute(os.environ["SETUP"])
    con.execute(f"CREATE SEMANTIC VIEW nem FROM '{bim}'")
    frame = con.execute("SELECT data_as_of FROM (SHOW SEMANTIC VIEWS) WHERE view_name = 'nem'").fetchone()[0]
    print(f"nem: {frame}")
    facts = json.loads(con.execute("SELECT dax_sql_facts('nem')").fetchone()[0])
    if not facts["pins"]:
        sys.exit(f"nem is not pinned: {frame}")
    if not facts["orphans"]:
        sys.exit(f"nem's relationships were not checked: {frame}")
    with_answers = json.dumps(facts)
    without = json.dumps({"pins": facts["pins"], "orphans": {}, "trailingSpaces": facts.get("trailingSpaces", {})})
    found = sorted(k for k, v in facts["orphans"].items() if v)
    print(f"{len(facts['orphans'])} relationships checked; with keys their one side lacks: {found or 'none'}")

    con.execute(
        f"ATTACH '{os.environ['WS_ID']}/{os.environ['LH_ID']}' AS ol (TYPE iceberg, "
        f"URI 'https://onelake.table.fabric.microsoft.com/iceberg', TOKEN '{os.environ['STORAGE_TOKEN']}', "
        "ACCESS_DELEGATION_MODE 'none')"
    )
    last = con.execute("SELECT max(date)::DATE FROM ol.mart.fct_region_daily").fetchone()[0]
    since = f"DATE '{last - datetime.timedelta(days=2)}'"
    queries = [q for f in files for q in paragraphs(f)
               if q.lstrip("( \n").upper().startswith(("SELECT", "WITH")) and "revenue_by_region" not in q]
    queries += [
        f'SELECT dim_calendar.date, dim_time.time, AGG("Demand with rooftop MW") AS demand FROM nem '
        f"WHERE dim_calendar.date >= {since} GROUP BY ALL ORDER BY ALL",
        f'SELECT dim_calendar.date, dim_time.time, dim_duid.FuelSourceDescriptor, AGG("Generation MW") AS mw FROM nem '
        f"WHERE dim_calendar.date >= {since} GROUP BY ALL ORDER BY ALL",
        f'SELECT dim_duid.FuelSourceDescriptor, AGG("Capacity factor") AS cf, AGG("Generation MWh") AS mwh FROM nem '
        f"WHERE dim_calendar.date >= {since} GROUP BY ALL ORDER BY ALL",
        f'SELECT dim_region.Region, dim_time.time, AGG("Average price") AS price FROM nem '
        f"WHERE dim_calendar.date >= {since} GROUP BY ALL ORDER BY ALL",
    ]

    lines = ["| query | rows | EXISTS with / without | ms with / without |", "|---|---|---|---|"]
    differ, total_on, total_off = 0, 0.0, 0.0
    for q in queries:
        sqls, best, rows = {}, {}, {}
        short = " ".join(q.split())[:90].replace("|", "\\|")
        try:
            for _ in range(2):
                for mode, f in (("on", with_answers), ("off", without)):
                    con.execute("SELECT dax_sql_set_facts('nem', ?)", [f])
                    sqls[mode] = con.execute("SELECT dax_sql_rewrite(?)", [q]).fetchone()[0]
                    r, ms = timed(con, q)
                    rows[mode] = r
                    best[mode] = min(best.get(mode, ms), ms)
        except duckdb.Error as e:
            lines.append(f"| `{short}` | error: {str(e).splitlines()[0][:80]} | | |")
            continue
        ok = same(rows["on"], rows["off"])
        if not ok:
            differ += 1
            print(f"DIFFER\n{q}\n--- with the orphan answers:\n{rows['on'][:20]}\n--- without:\n{rows['off'][:20]}\n", flush=True)
        total_on += best["on"]
        total_off += best["off"]
        lines.append(f"| `{short}` | {len(rows['on'])}{'' if ok else ' **differ**'} | "
                     f"{sqls['on'].count('EXISTS')} / {sqls['off'].count('EXISTS')} | {best['on']:.0f} / {best['off']:.0f} |")
    lines.append(f"| **all {len(queries)}** | | | **{total_on:.0f} / {total_off:.0f}** |")
    report = (f"### Orphan answers on nem\n\n{frame}\n\nWith keys their one side lacks: "
              f"{', '.join(found) or 'none'}\n\n" + "\n".join(lines) + "\n")
    print(report)
    if os.environ.get("GITHUB_STEP_SUMMARY"):
        open(os.environ["GITHUB_STEP_SUMMARY"], "a", encoding="utf-8").write(report)
    print(f"{len(queries) - differ} of {len(queries)} queries give the same rows with and without the orphan answers")
    sys.exit(1 if differ else 0)


if __name__ == "__main__":
    main()
