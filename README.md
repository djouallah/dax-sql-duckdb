# dax_sql, a DuckDB extension

A DuckDB extension that makes a semantic model (a TMSL `.bim`, a TMDL `.tmdl` or folder, or an Apache Ossie `.yaml`) a relation in
DuckDB, compiled by [dax-sql](https://github.com/djouallah/dax-sql). The design is
[EXTENSION.md](EXTENSION.md).

It targets DuckDB 2.0 and later, and was made from DuckDB's
[extension template](https://github.com/duckdb/extension-template). The `duckdb` and
`extension-ci-tools` submodules are pinned to DuckDB v2.0.0.

## Stage one: the CLI

The first stage is a native build loaded into the DuckDB 2.0 CLI. Wasm comes later. So far the
extension registers one function, `dax_sql_version()`.

Everything is verified in CI; nothing is built or run on a laptop.

- **Build:** `linux_amd64` and `windows_amd64`, each running `test/sql/` against its build.
- **DuckDB CLI** (`cli` job): loads the `linux_amd64` build into the official DuckDB v2.0.0 CLI
  (`-unsigned`, since the build is not signed). The same CLI then reads OneLake's Iceberg
  REST catalog with the token from CI's identity: it lists the lakehouse's tables and counts
  one.

The `cli` job signs in to Azure with OIDC, as dax-sql's CI does. Its identity is the Entra
app whose federated credential `dax-sql-duckdb-main` trusts this repo's `main`. The repository
variables are:

| Variable | What |
|---|---|
| `AZURE_CLIENT_ID` | The Entra app CI signs in as |
| `AZURE_TENANT_ID` | Its tenant |
| `WS_ID` | The Fabric workspace |
| `LH_ID` | The lakehouse in it whose tables the job reads |
