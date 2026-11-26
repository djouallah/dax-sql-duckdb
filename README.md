# dax_sql, a DuckDB extension

`dax_sql` makes a Power BI semantic model a semantic view in DuckDB, queried in plain SQL. The
model's measures are DAX; they are compiled to DuckDB SQL by
[dax-sql](https://github.com/djouallah/dax-sql), embedded in the extension.

```sql
LOAD dax_sql;
CREATE SECRET onelake (TYPE azure, PROVIDER credential_chain);

CREATE SEMANTIC VIEW sales
    FROM 'abfss://<workspace>@onelake.dfs.fabric.microsoft.com/<lakehouse>/Files/model.bim';

SELECT Product.Color, AGG(Orders) AS orders, AGG("Sales Amount") AS amount
FROM sales
WHERE Customer.Country = 'France'
GROUP BY ALL
ORDER BY amount DESC;
```

- **The model:**
  - a TMSL `.bim`;
  - a TMDL `.tmdl`, or a folder of them;
  - an Apache Ossie `.yaml`, or a folder of them as a Fabric semantic view keeps them
    (`Context/Semantic Views/<view>/`).

  The file is read from wherever DuckDB reads: a local path, S3, Azure or OneLake.
- **The data:** a Direct Lake model names its lakehouse, so the view attaches it through OneLake's
  Iceberg REST catalog and reads its tables there, pinned to their current snapshots. Any other
  model reads its tables from the search path.
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
