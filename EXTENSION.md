# A DuckDB extension

The design of the extension, and what of it is built. The compiler it builds on is
[dax-sql](https://github.com/djouallah/dax-sql); paths such as `src/` and `formats/` are that
repository's.

The extension targets DuckDB 2.0 and later. Earlier versions are not supported: the hooks it
needs (below) are DuckDB 2.0's, and nothing is kept for 1.x.

## The goal

Today a model can be queried only from JavaScript, through dax-sql's `createCompiler`. The
extension makes a model a semantic view in DuckDB, so that any DuckDB client can query it: the
CLI, Python, R, and BI tools that speak DuckDB SQL. A model's queries can then be mixed with
the rest of the user's SQL.

```sql
LOAD dax_sql;
CREATE SEMANTIC VIEW sales FROM 'contoso/model.bim';

SELECT Product.Color, AGG(Orders) AS orders FROM sales GROUP BY ALL;
```

## Background

Two earlier blog posts describe the idea behind this design:

- ["Modern" BI tools are missing something like PowerBI Composable SQL Generator](https://datamonkeysite.com/2021/10/03/modern-bi-tools-are-missing-something-like-powerbi-composable-sql-generator/)
  (2021): Power BI's DAX engine sends several SQL statements and assembles their results, and
  other BI tools have no SQL generator like it.
- [Why Datamart is a Big Deal](https://datamonkeysite.com/2022/06/03/why-datamart-is-a-big-deal/)
  (2022): Power BI is the DAX engine, and the engine that computes the answers can be chosen.

## What a user writes

A model is a semantic view, queried with `AGG()` in plain SQL, in the forms DuckDB uses for
its own statements:

```sql
CREATE [OR REPLACE] SEMANTIC VIEW [IF NOT EXISTS] [db.][schema.]sales FROM 'contoso/model.bim';
DROP SEMANTIC VIEW [IF EXISTS] sales;
REFRESH SEMANTIC VIEW sales;

SELECT Color, AGG(Orders) AS orders FROM sales WHERE Year = 2024 GROUP BY ALL HAVING orders > 2;
WITH m AS (SELECT Color, AGG(Orders) AS orders FROM sales GROUP BY ALL) SELECT * FROM m JOIN stock USING (Color);

SHOW SEMANTIC VIEWS;
DESCRIBE sales;
SHOW SEMANTIC DIMENSIONS IN sales;
SHOW SEMANTIC METRICS IN sales;
SHOW SEMANTIC DIMENSIONS IN sales FOR METRIC Orders;
```

### Creating

`CREATE SEMANTIC VIEW <name> FROM '<path>'` reads a model file and makes it a semantic view
named `<name>`, in the current schema or the one named.

- **Many views:** the name is the statement's, not the file's. One database holds as many
  views as it likes, each from its own `.bim`.
- **The file:** its path says its format, and dax-sql's reader of that format reads it:

  | Path | Format | Read by |
  |---|---|---|
  | `….bim` | TMSL | `formats/tmsl.js` |
  | `….tmdl` | TMDL in one file (Fabric's TMDL of a semantic view, or a `createOrReplace` script) | `formats/tmdl.js` |
  | a folder (a PBIP's `definition/`, a `.SemanticModel` folder) | TMDL: every `.tmdl` in it, at any depth | `formats/tmdl.js` |
  | `….yaml`, `….yml` | Apache Ossie | `formats/ossie.js`, after the embedded `yaml` parser |

  Each is read as TMSL first, so everything after (the location of the data, the compiler) is
  the same whatever the format. An Ossie metric dax-sql cannot read (one written only in another
  engine's dialect) is left out, as dax-sql leaves it out; a model left with no metric is an
  error. The path is any one DuckDB reads: a local file, S3, Azure or OneLake; a folder needs a
  file system that lists files (not plain HTTP).
- **No options:** everything the view needs is in the `.bim`. That covers where each table's
  data is ([Where the data comes from](#where-the-data-comes-from)), the relationships, their
  integrity flags and the roles.
- **Stored in the database:** the file's text is kept with the view, not just its path, so a
  database file carries its semantic views (a TMDL folder's files, as one JSON object).
  `CREATE OR REPLACE` reads the file again after it changes; `DROP SEMANTIC VIEW` removes the view.

### Querying

A **model query** is a `SELECT` whose only `FROM` is a semantic view, with or without an
alias. It is the SQL the compiler takes today (DESIGN.md, Reference, SQL):

- `AGG(m)` is measure `m`, and the only way to name one: `AGG(Orders)`, or
  `AGG("Sales Amount")` for a name SQL quotes. A measure's name is unique in its model (as in
  any Tabular model, whatever its case), so it needs no table. `SUM`, `AVG`, `MIN`, `MAX` and
  `COUNT` aggregate a column.
- The columns in the select list are the groups. They appear in `GROUP BY`, or `GROUP BY ALL`
  takes them from the select list. `ROLLUP` adds subtotals.
- `WHERE` filters on columns only, and `HAVING` filters on aggregates.
- `ORDER BY`, `LIMIT` and `FETCH FIRST n ROWS WITH TIES` work as usual.

The whole query is compiled together, because a measure's value depends on the groups and the
filters around it. That is why the view is not a table: it has no rows until a query chooses
the groups.

These are errors, each with a message that says what to write instead:

- **`SELECT *`:** name the columns and `AGG(measures)`; `DESCRIBE sales` lists them.
- **`JOIN sales`, or `FROM sales` outside a model query:** put the model query in a CTE or a
  subquery, and join that.
- **Window functions and `QUALIFY` in a model query:** apply them to the result, outside it.
- **Grouping by a measure, or a bare measure without `AGG`.**

Names are SQL's. A bare column name is enough when one table has it; otherwise write
`table.column`. A name with spaces takes double quotes. A model query is plain DuckDB SQL:
the only DAX is in the model's file, its measures' expressions.

### Composing

Everything around a model query is DuckDB's own SQL. Inside a larger statement, a model query
is in parentheses, wherever DuckDB takes a subquery:

- a CTE's body, or a subquery in `FROM`;
- an arm of a `UNION`, `INTERSECT` or `EXCEPT`;
- `IN (SELECT … FROM sales …)`;
- `CREATE TABLE AS` and `COPY (…) TO`.

```sql
WITH m AS (SELECT Color, AGG(Orders) AS orders FROM sales GROUP BY Color)
SELECT m.*, s.stock FROM m JOIN warehouse.stock s USING (Color)
UNION ALL
(SELECT 'total', AGG(Orders), NULL FROM sales);
```

A model query reads no column of the query around it: it is not correlated. A prepared
statement's parameters (`$1`, `?`) inside it become typed values, never SQL text.

### Inspecting

| Statement | Lists |
|---|---|
| `SHOW SEMANTIC VIEWS` | The semantic views, with their database, schema and file, where their tables are read from (`tables_from`), and the data they read (`data_as_of`: the snapshots they are pinned to, and what the check of their relationships' keys found) |
| `DESCRIBE sales` | One row per column and measure a query can name, in DuckDB's `DESCRIBE` columns: `column_name, column_type, null, key, default, extra`, with `extra` `'column'` or `'measure'` |
| `SHOW SEMANTIC DIMENSIONS IN sales` | The model's columns |
| `SHOW SEMANTIC METRICS IN sales` | The model's measures |
| `SHOW SEMANTIC DIMENSIONS IN sales FOR METRIC m` | The columns that filter measure `m`: those its table reaches along the relationships (`Model.reaches()`, `src/model.js`) |

- **Vocabulary:** a dimension is a model column and a metric is a measure.
- **Output columns:** the `SHOW SEMANTIC` statements return `database_name, schema_name,
  view_name, table_name, name, data_type, expression, format_string, hidden, comment`.
- **Hidden items:** hidden columns and measures are listed, with `hidden` true.
- **As a subquery:** each of them can be read like a table:
  `SELECT * FROM (SHOW SEMANTIC METRICS IN sales) WHERE …`.

## How it works

The extension is written in C++ and has three parts.

1. **The definitions.** `CREATE SEMANTIC VIEW` reads the file through DuckDB's `FileSystem`,
   checks it by building a compiler (`createCompiler` over the reader's model), and stores
   the text in a table of its schema. An in-memory registry holds each view's compiler, filled
   on `CREATE` and, for views stored earlier, when a connection opens.
2. **The statements.**
   - **`CREATE`, `DROP`, `REFRESH` and `SHOW SEMANTIC …`** are rules of a grammar extension of
     DuckDB's PEG parser (`src/dax_sql_grammar.cpp`), active on every connection: the
     connections opened after `LOAD` at once, the one that ran `LOAD` once that statement ends
     (so `LOAD` and a `CREATE SEMANTIC VIEW` are two statements, not one string). Each is
     lowered to a call of the extension's table functions, `CALL dax_sql_create(…)`,
     `dax_sql_drop`, `dax_sql_refresh`, `dax_sql_show`, which do the work or return the rows.
     A `CALL`, not a `SELECT`: a client runs it at once and keeps its rows, where the Python
     client would read a `SELECT`'s text again as a subquery when it is shown.
     `(SHOW SEMANTIC …)` is a table wherever DuckDB takes one. The grammar's plumbing follows
     the `semantic_layer` community extension's (`THIRD_PARTY_NOTICES.md`).
   - **Model queries** are plain SQL, compiled as they are parsed: a parser override
     (`DaxSqlParserOverride`; `LOAD` sets `allow_parser_override_extension` to `fallback`
     unless it was set already) parses a text that names a view, and sends each statement that
     names one to dax-sql's `compileStatement(text, name)`, once per view it names. A client
     that binds a statement before it runs it (the Python client's `con.sql`, through
     `ClientContext::BindStatement`) binds plain SQL. A text that holds one of the extension's
     own statements is parsed as usual, since the views it creates change what the statements
     after it compile to: those compile when binding fails on the view's name
     (`OnPlanningError`, a hook on every connection), as does a statement that did not compile
     at parse time, whose compiler error is reported then. That finds each
     model query (its scanner skips strings, quoted names, `$$` strings and comments), compiles
     it, and puts the SQL in its place. The extension parses what comes back, puts its body in
     the statement's place, and DuckDB binds, optimises and runs the whole statement as one
     query. Filters and column choices push down into the compiled SQL, and the result
     streams. `DESCRIBE sales` goes the same way, as its rows.
   - **Errors:** the compiler's errors (`SYNTAX`, `SEMANTIC`, `UNSUPPORTED`) become DuckDB
     errors that keep its message.
3. **The compiler, unchanged.** `src/` and the readers in `formats/` (with `yaml` 2.8.1, the
   version dax-sql's tests parse Ossie with, vendored in `third_party/yaml/`) are embedded in
   the extension as their files, and run by quickjs-ng, a small JavaScript engine written in C
   that MSVC builds as well. The compiler has no runtime dependencies, so it needs nothing
   else. Compiling a query takes milliseconds. There is one
   compiler, so the conformance suite's goldens keep covering what the extension runs.

The extension lives in this repository, made from DuckDB's extension template, with dax-sql
as a submodule pinned to a commit. Its CI builds it and runs its tests.

### The compiler does not drift from dax-sql

Every rule of how DAX and a model query become SQL lives in dax-sql, and only there. This
repository holds only what DuckDB needs: the statements, the definitions, the file reading,
the OneLake attach, and the calls into the compiler (`js/glue.js`).

- **No compiler logic here.** The glue script that calls dax-sql adds no translation of its
  own: it does not rewrite queries, fix results, or work around the compiler. A wrong answer
  is fixed in dax-sql, with its test, and reaches the extension by moving the submodule.
- **The embedded compiler is the pinned commit.** The build embeds the submodule's `src/` and
  its readers (`formats/tmsl.js`, `tmdl.js`, `ossie.js`, `ossie-sql.js`) as they are, file by file (`cmake/embed_js.cmake`), and QuickJS loads them
  as ES modules. Nothing is bundled, patched or committed in their place, so there is no copy
  to fall out of step.
- **One answer for one query.** For each SQL test, CI also runs the query through dax-sql in
  Node at the same commit, and the two must give the same SQL and the same rows.
- **Moving the submodule** is an ordinary commit here, made once dax-sql's own CI is green at
  the new commit. The extension follows dax-sql; it never leads it.

### Where the data comes from

The data is read from OneLake through its Iceberg REST catalog. Nothing in the query or in the
`CREATE` says where it is, because a Direct Lake `.bim` says so already:

- **The lakehouse:** the model's shared expression (`model.expressions`, `kind: m`) holds
  `AzureStorage.DataLake("https://onelake.dfs.fabric.microsoft.com/<workspace>/<item>")`. The
  two GUIDs are the workspace and the lakehouse.
- **The table:** each table's partition has `mode: directLake` and a source with
  `expressionSource` (which expression, so which lakehouse), `schemaName` and `entityName`.

The extension reads these, and for each lakehouse the model uses it attaches the catalog
itself, under an internal name:

```sql
ATTACH '<workspace>/<item>' AS <internal> (
    TYPE iceberg,
    URI 'https://onelake.table.fabric.microsoft.com/iceberg',
    ACCESS_DELEGATION_MODE 'none');
```

The compiled SQL then names `<internal>.<schemaName>.<entityName>`, and DuckDB's `iceberg` and
`azure` extensions do the reading. Both autoload, so the user does not `LOAD` them.

- **Authentication:** one token with the scope `https://storage.azure.com/.default` is accepted
  by both the catalog and the storage. The extension takes it, when it attaches, from the
  secrets DuckDB finds for the lakehouse, in this order:
  1. an Azure secret's `access_token` (`PROVIDER access_token`);
  2. an Azure secret's `token`, which a `credential_chain` secret with `REFRESH 'auto'` keeps
     (duckdb-azure#191, not merged yet);
  3. a `credential_chain` secret whose chain has `workload_identity` (or names no chain): the
     extension exchanges the OIDC token in `AZURE_FEDERATED_TOKEN_FILE` with Entra for
     `AZURE_CLIENT_ID` in `AZURE_TENANT_ID`, through httpfs's HTTP client. GitHub Actions and
     Kubernetes give that token; CI runs this way, with no Azure CLI;
  4. a `credential_chain` secret whose chain has `cli` (or names no chain): the extension runs
     `az account get-access-token` itself.

     Steps 3 and 4 are hacks until #191: the azure extension resolves the chain but keeps the
     token to itself. They are native only;
  5. an http secret's `bearer_token`.

  When none gives a token, the attach's error in `tables_from` says why (`no token: …`).

  The token is used once, at attach time; nothing refreshes it yet (duckdb-iceberg#1429 would).
- **Reading the location:** dax-sql's TMSL reader (`formats/tmsl.js`) keeps only `schemaName`
  and `entityName`. The expression and its URL are read by the extension's own code, from the
  same parsed JSON, so dax-sql is not changed.
- **Which data, and when it changes:** a Direct Lake view reads its tables at the snapshots they
  were at when it was framed: on `CREATE`, when a database holding it is opened, and on
  `REFRESH SEMANTIC VIEW`, as Power BI's Direct Lake reads a model's tables as of its last
  framing. A new load in the lakehouse shows after the next of those. Every query of a view reads
  the same data, which is what lets the facts below be found once ([Facts from the
  data](#facts-from-the-data)).
- **A model without a OneLake location:** a partition with no `expressionSource` is read as
  `schema.entity` through the session's search path. That is how the tests run the
  conformance models over local tables.
- **A lakehouse that cannot be attached:** when the attach fails (no `azure` extension, no
  secret, no network), `CREATE` still succeeds. The view reads the same
  `schema.entity` tables from the search path instead, such as a DuckDB file holding a copy of
  the lakehouse: `ATTACH 'copy.duckdb'; USE copy;`. `SHOW SEMANTIC VIEWS` says where each view's
  tables come from (`tables_from`), with the attach's error when it failed. A view restored on
  load whose lakehouse cannot be attached then (no secret yet) reads the search path too, until
  `CREATE OR REPLACE`.

### Facts from the data

dax-sql reads no data, so its SQL asks some questions of the data on every query. The costly one
is DAX's blank row: for each dimension a query groups by, one anti-join per fact related to it,
over the fact's whole key column. In the conformance suite that changes the SQL of 59 of the
827 `aac` queries, 381 of 1,075 on contoso and 337 of 916 on tpcds. Example: the `aac`
dashboard's 3-day demand peak scans 15 tables where 9 would do.

The answer depends only on the data, so the extension finds it once per frame and gives it to the
compiler. The compiler still reads nothing, and every rule stays in dax-sql:

- **The frame** (`FrameView`, the glue's `frame_sql`, `frame`, `orphans_found`, `padded_sql`,
  `padded_found`):
  1. each OneLake table's current snapshot, from `iceberg_snapshots()`;
  2. the view's compiler rebuilt with each table read as
     `(SELECT * FROM <catalog>.<schema>.<table> AT (VERSION => <snapshot>))`, a subquery because
     DuckDB takes no alias after `AT`;
  3. dax-sql's `orphanCheck()` statement run at those snapshots: for each relationship, whether
     its many side holds a key its one side lacks, NULL and empty keys included
     (djouallah/dax-sql#7);
  4. the answers passed to dax-sql's `orphans` option, keyed by `rel.key` (djouallah/dax-sql#6).
     A relationship with a table that is not pinned gets no answer and is tested in the SQL;
  5. dax-sql's `trailingSpacesCheck()` statement run at the same snapshots: for each text column
     of a pinned table, whether a value ends in a space. Power BI trims trailing spaces when it
     loads text, so `"North  "` is `"North"` there; the columns found are passed to dax-sql's
     `trailingSpaces` option (keyed by `key`, `Table|Column`) and read trimmed, the others as
     stored. `data_as_of` lists the columns trimmed. A step of its own: when it fails, the
     orphan answers stay.
- **When a step fails** (no `iceberg_snapshots` for the table, the check fails), the view keeps
  what the steps before it found: unpinned, today's SQL, or pinned with no answers.
  `data_as_of` says why. The answers never outlive the snapshots they were found at.
- **Not for search-path tables.** A view reading DuckDB tables (a copy of the lakehouse, the
  tests' local tables) has nothing to pin, so it gets no answers: those tables can change under
  them.
- **Checked in CI** (`.github/scripts/facts.py`): every query of `test/directlake/queries.sql`,
  plus dashboard-shaped ones over the last three days, runs over the live `nem` view with the
  answers and without them, at the same snapshots. The rows must be the same. Each query's time
  both ways and the `EXISTS` in its SQL go to the job summary. `dax_sql_facts(view)` and
  `dax_sql_set_facts(view, json)` read and set a view's frame for that check and for the drift
  check, which gives the glue in Node the same frame.
- **Approximate distinct counts** (djouallah/dax-sql#8): grouping a fact by its join keys before
  the join pays when the keys collapse the rows (#4: 20× faster). When they are the fact's grain
  it costs about 6× instead. dax-sql now applies a dimension's conditions before that grouping,
  which took most of it. The choice by estimates is declined until a case is measured as very
  slow at scale.
- **Not-null keys** stay the model's (`nullable`, TMSL's `isNullable`): a constraint, true across
  loads. Orphan answers are a fact about one version of the data (#6).

### Not for DuckDB-Wasm

The extension is not built for wasm. What it is for is OneLake, read as `abfss://` through
DuckDB's `azure` extension, and `azure` has no wasm build (duckdb-azure builds no wasm target).
An earlier build ran in DuckDB-Wasm, reading OneLake live only through hacks of its own: a file
system mapping `abfss://` to `https://`, a page's service worker signing each file read, and
httpfs's ETag check turned off. That is not a thing to ship. The descriptor's `excluded_platforms`
leaves the wasm platforms out (`wasm_mvp;wasm_eh;wasm_threads`); a wasm build comes back when
`azure` has one, or DuckDB-Wasm reads `abfss://` itself (duckdb-wasm#2238).

## Choices set aside

- **`ATTACH '<file>' AS x (TYPE model)`**, an earlier form of this design. A semantic view
  lives in a schema of a database and survives it being closed, where an attached model has
  to be attached again in every session. It also needs no custom DuckDB catalog, which is
  most of the C++ ATTACH would have taken.
- **Options on `CREATE`** (roles, a data source, the compiler's flags). The `.bim` already
  holds all of it, and a second place to say it would only disagree with the first.
- **A view with fixed rows.** For `SELECT * FROM sales` to have rows, the model would need one
  grain: every column with every measure (often far too many rows), or one row of grand totals.
  Neither is what a query means by its groups. So `*` is an error, and the query names its groups.
- **A table function that takes the groups and measures as arguments**, as in
  `FROM semantic_view('sales', dimensions := [...], metrics := [...])`. Another semantic-layer
  extension for DuckDB works this way. Here the compiler already speaks `SELECT … GROUP BY`,
  so the query form is SQL's own, and there is one way to write a query.
- **Printing DuckDB's parse tree back to SQL** for the compiler. `compileStatement` already finds
  the model queries in the statement's own text, which DuckDB keeps, so no second printer is
  needed.
- **A parser override** (`allow_parser_override_extension`), which the first build used: it saw
  every statement's text first, and needed the global `FALLBACK` setting. The grammar and the
  rebind hook need neither.
- **DuckDB's C extension API.** It is stable across DuckDB versions, but it has no hook for
  the parser. That exists only in DuckDB's C++ API, so the extension is built for one DuckDB
  2.x release at a time.
- **Porting the compiler to C++ or Rust.** There would be two compilers to keep equal, and the
  goldens would test only one of them.

## Stages

1. **The CLI.** Native builds (`linux_amd64` and `windows_amd64` first), loaded into the DuckDB
   2.0 CLI: create a semantic view from a `.bim`, query it, inspect it, and read its data
   from OneLake.
2. **Every native platform** community-extensions builds, as a community extension. Not wasm
   ([Not for DuckDB-Wasm](#not-for-duckdb-wasm)).

## Open questions

Each one is settled by a first, small build, before the rest is written.

- **Which role applies.** The roles are in the `.bim`, but DuckDB has no signed-in user to
  match against their members. Until that is settled, queries run without row-level security.
- **Long sessions on OneLake.** A token lasts about an hour. Check that a `credential_chain`
  secret refreshes it for the attached catalog, and what a query sees when it has expired.
- **Views.** The rebind puts the compiled SQL in the statement's place, so `CREATE VIEW` over a
  model query stores the compiled SQL, not the user's. Check whether the user's
  text can be kept, so a view follows `CREATE OR REPLACE SEMANTIC VIEW`.
- **Following DuckDB releases.** The floor is DuckDB 2.0. Find out how much the C++ API the
  extension uses changes between 2.x releases, and so how each new release is picked up.

## Verification

- **The extension's tests,** in DuckDB's sqllogictest format, run in CI against DuckDB 2.0 and
  the latest 2.x release, and cover:
  - `CREATE SEMANTIC VIEW` from a `.bim`, a `.tmdl`, a TMDL folder and an Ossie `.yaml`
    (`test/sql/formats.test`: the same model in two formats gives the same rows), with `OR REPLACE`,
    `IF NOT EXISTS` and a schema, several views in one database, and `DROP`;
  - a view still there after the database file is closed and opened again;
  - each inspecting statement, and each one used as a subquery;
  - a model query on its own, in a CTE, in a subquery, in a UNION arm, and in `IN`;
  - the errors for `SELECT *`, `JOIN sales`, and a window function;
  - the OneLake location read from a Direct Lake `.bim`, both the expression's URL and the
    partitions' names, checked without a network.
- **OneLake:** CI's `cli` job signs in with OIDC federation (`azure/login`, repository
  variables `AZURE_CLIENT_ID`, `AZURE_TENANT_ID`, `WS_ID`, `LH_ID`). It already loads the build
  into the DuckDB v2.0.0 CLI and reads the lakehouse through the Iceberg REST catalog. Once
  `CREATE SEMANTIC VIEW` exists, it creates a view from a Direct Lake `.bim` over that
  lakehouse and checks a few answers.
- **Conformance:** the suite's queries are DAX, which this syntax does not take, so they are
  not run through the extension yet. Its SQL tests compare each answer with the same query
  run through dax-sql in Node.
