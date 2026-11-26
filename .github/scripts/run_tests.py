#!/usr/bin/env python3
"""Runs the extension's sqllogictest files against a built extension, in the duckdb Python
package of the extension's DuckDB version, so a test needs no build of its own:

    python .github/scripts/run_tests.py <dax_sql.duckdb_extension> test/sql/*.test

It reads the part of the format the tests use: require, load, restart, statement ok, statement
error (the expected text after ---- is a substring of the error), query (the expected rows after
----, tab-separated; a row <REGEX>:x matches a one-value row against x). A file stops at its first
failure; the exit code is 1 when any file failed.
"""
import re
import sys
import tempfile
from pathlib import Path

import duckdb


def records(path):
    """The test's records: (line, kind, argument, sql, expected lines)."""
    lines = Path(path).read_text(encoding="utf-8").splitlines()
    k = 0
    while k < len(lines):
        line = lines[k].strip()
        start = k + 1
        k += 1
        if not line or line.startswith("#"):
            continue
        kind, _, argument = line.partition(" ")
        if kind not in ("statement", "query"):
            yield start, kind, argument.strip(), None, None
            continue
        sql = []
        while k < len(lines) and lines[k].strip() and lines[k].strip() != "----":
            sql.append(lines[k])
            k += 1
        expected = None
        if k < len(lines) and lines[k].strip() == "----":
            k += 1
            expected = []
            while k < len(lines) and lines[k].strip():
                expected.append(lines[k])
                k += 1
        yield start, kind, argument.strip(), "\n".join(sql), expected


def render(value):
    if value is None:
        return "NULL"
    if isinstance(value, bool):
        return "true" if value else "false"
    return str(value)


def matches(expected, row):
    if expected.startswith("<REGEX>:"):
        return len(row) == 1 and re.fullmatch(expected[len("<REGEX>:") :], row[0]) is not None
    return expected.split("\t") == row


def run(extension, path, workdir):
    def connect(database):
        con = duckdb.connect(database, config={"allow_unsigned_extensions": "true"})
        con.execute(f"LOAD '{extension}'")
        return con

    database = ":memory:"
    con = connect(database)
    for line, kind, argument, sql, expected in records(path):
        where = f"{path}:{line}"
        if sql:
            sql = sql.replace("__TEST_DIR__", workdir)
        try:
            if kind == "require":
                continue
            if kind == "load":
                con.close()
                database = argument.replace("__TEST_DIR__", workdir)
                con = connect(database)
                continue
            if kind == "restart":
                con.close()
                con = connect(database)
                continue
            if kind == "statement" and argument == "ok":
                con.execute(sql)
                continue
            if kind == "statement" and argument == "error":
                try:
                    con.execute(sql)
                except duckdb.Error as e:
                    text = "\n".join(expected or [])
                    if text and text not in str(e):
                        raise AssertionError(f"expected an error with:\n{text}\ngot:\n{e}")
                    continue
                raise AssertionError("expected an error, the statement succeeded")
            if kind == "query":
                rows = [[render(v) for v in r] for r in con.execute(sql).fetchall()]
                expected = expected or []
                if len(rows) != len(expected) or not all(matches(e, r) for e, r in zip(expected, rows)):
                    got = "\n".join("\t".join(r) for r in rows)
                    raise AssertionError(f"expected:\n" + "\n".join(expected) + f"\ngot:\n{got}")
                continue
            raise AssertionError(f"unknown record: {kind} {argument}")
        except Exception as e:
            print(f"FAIL {where}\n{sql or ''}\n{e}\n", flush=True)
            return False
    print(f"ok   {path}", flush=True)
    return True


def main():
    extension, files = sys.argv[1], sys.argv[2:]
    with tempfile.TemporaryDirectory() as workdir:
        results = [run(extension, f, workdir) for f in files]
    sys.exit(0 if all(results) else 1)


if __name__ == "__main__":
    main()
