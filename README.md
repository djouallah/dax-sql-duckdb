# dax_sql, a DuckDB extension

`dax_sql` makes a Fabric semantic view, an Apache Ossie YAML, a semantic view in DuckDB, queried
in plain SQL. Its measures are compiled to DuckDB SQL by
[dax-sql](https://github.com/djouallah/dax-sql), embedded in the extension.

In a Fabric Python notebook ([test/integrations/ossie.ipynb](test/integrations/ossie.ipynb)),
first upgrade DuckDB, since the extension needs DuckDB 2.0.1:

```python
%pip install duckdb --upgrade
notebookutils.session.restartPython()
```

Then:

```python
import duckdb

con = duckdb.connect()
con.execute("INSTALL dax_sql FROM community")
con.execute("LOAD dax_sql")

con.execute("""
CREATE OR REPLACE SEMANTIC VIEW nem
    FROM 'abfss://<workspace>@onelake.dfs.fabric.microsoft.com/<lakehouse>.Lakehouse/Context/Semantic Views/nem/ossie.yaml'
""").fetchall()

con.execute("DESCRIBE nem").fetchdf()

con.execute("""
SELECT dim_region.Region, AGG("Registered units") AS units, AGG("Registered MW") AS mw
FROM nem
GROUP BY ALL
ORDER BY ALL
""").fetchdf()
```

Run every statement with `con.execute` and fetch its result: `con.sql` does not work with
semantic views yet.

- **The model:** an Ossie `.yaml`, or a semantic view's folder of them
  (`Context/Semantic Views/<view>/`, every `.yaml` in it merged into one model). It is read from
  wherever DuckDB reads: a local path, S3, Azure or OneLake.
- **The data:** the YAML's DirectLake expression names its lakehouse, so the view attaches it
  through OneLake's Iceberg REST catalog and reads its tables there, pinned to their current
  snapshots. A Fabric notebook needs no secret; elsewhere, create one first:
  `CREATE SECRET onelake (TYPE azure, PROVIDER credential_chain)`. A YAML that names no
  lakehouse reads `schema.table` from the search path: `ATTACH` the catalog and `USE` it before
  the `CREATE`.
- **Queries:** plain DuckDB SQL. A measure is `AGG(name)`, or `AGG("name with spaces")`. Model
  queries go wherever DuckDB takes a subquery: CTEs, joins, `UNION`, `IN`, `CREATE TABLE AS`.
- **Statements:**
  - `CREATE [OR REPLACE] SEMANTIC VIEW [IF NOT EXISTS] v FROM '…'`
  - `DROP SEMANTIC VIEW [IF EXISTS] v`
  - `REFRESH SEMANTIC VIEW v`
  - `SHOW SEMANTIC VIEWS`
  - `SHOW SEMANTIC DIMENSIONS | METRICS IN v`
  - `DESCRIBE v`

The design, and how it works: [EXTENSION.md](EXTENSION.md).

## Platforms

DuckDB 2.0.1, on every native platform. Not DuckDB-Wasm: OneLake is read through the `azure`
extension, which has no wasm build.

## Tests

Everything is verified in CI:

- **sqllogictests:** `test/sql/`, with their fixtures in `test/models/fixtures/`.
- **Live tests:** the DuckDB CLI reads a real lakehouse, with the SQL files in `test/onelake/` and
  the Test workflow's steps. They sign in to Azure with OIDC: the Entra app's federated credential
  trusts this repository's `main` branch, and nothing runs for a fork's pull request.

| Repository variable | What |
|---|---|
| `AZURE_CLIENT_ID` | The Entra app CI signs in as |
| `AZURE_TENANT_ID` | Its tenant |
| `WS_ID` | The Fabric workspace |
| `LH_ID` | The lakehouse whose tables the tests read |
| `SM_ID` | The semantic model whose `.bim` the tests read |
| `FULL_CI` | `true`: build every platform and run the tests in the build, as community-extensions does |

## License

MIT ([LICENSE](LICENSE)). Third-party code: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
