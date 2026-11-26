// What the extension does in DuckDB-Wasm (the base bundle, platform wasm_eh): each item runs on
// its own and is reported, works or not, with its output or error. The page serves DuckDB-Wasm's
// dist at /duckdb-wasm/dist, the extension at /ext, and the models and data at /data (CI's wasm
// job lays them out). test/wasm/run.mjs reads window.__probe.
//
// Items marked `finding` are what the browser can or cannot do (Direct Lake, other extensions);
// the others are what the extension must do there.
import * as duckdb from '/duckdb-wasm/dist/duckdb-browser.mjs';

const base = location.origin;
const results = [];
window.__probe = results;

function show(r) {
  const row = document.createElement('tr');
  for (const v of [r.name, r.ok ? 'yes' : 'no', r.ok ? r.out : r.error]) {
    const td = document.createElement('td');
    td.textContent = v ?? '';
    row.append(td);
  }
  document.getElementById('results').append(row);
}

const json = rows => JSON.stringify(rows, (k, v) => (typeof v === 'bigint' ? Number(v) : v));

// With a OneLake token, the service worker (/sw.js) signs DuckDB-Wasm's requests to OneLake's files:
// the token goes in Cache Storage, and the worker controls this page and DuckDB's worker before
// DuckDB starts.
async function signOneLake(token) {
  await (await caches.open('onelake-token')).put('/__onelake_token', new Response(token));
  await navigator.serviceWorker.register('/sw.js', { scope: '/' });
  await navigator.serviceWorker.ready;
  if (!navigator.serviceWorker.controller) {
    await new Promise(resolve => navigator.serviceWorker.addEventListener('controllerchange', resolve, { once: true }));
  }
}

async function main() {
  if (window.__onelakeToken) await signOneLake(window.__onelakeToken);
  const bundle = await duckdb.selectBundle({
    base: { mainModule: `${base}/duckdb-wasm/dist/duckdb-base.wasm`, mainWorker: `${base}/duckdb-wasm/dist/duckdb-browser-base.worker.js` },
  });
  const db = new duckdb.AsyncDuckDB(new duckdb.VoidLogger(), new Worker(bundle.mainWorker));
  await db.instantiate(bundle.mainModule, bundle.pthreadWorker);
  // Extensions autoload from the page (/ext/<build name>/wasm_eh/), where CI puts DuckDB's own.
  await db.open({ allowUnsignedExtensions: true, extensionRepository: `${base}/ext` });
  const con = await db.connect();

  async function item(name, sqls, finding = false) {
    const r = { name, finding, ok: true, out: null, error: null };
    try {
      let table = null;
      for (const sql of sqls) table = await con.query(sql);
      if (table) r.out = json(table.toArray().slice(0, 6).map(row => row.toJSON())).slice(0, 500);
    } catch (e) {
      r.ok = false;
      r.error = String(e?.message ?? e).slice(0, 800);
    }
    results.push(r);
    show(r);
  }

  const csv = t => `read_csv('${base}/data/contoso/data/${t}.csv', header = true${t === 'Sales' ? ", types = {'Amount': 'DECIMAL(18,4)'}" : ''})`;
  const tables = ['Sales', 'Product', 'Category', 'Customer', 'Date', 'Returns', 'CityTarget', 'Staff'];

  await item('DuckDB-Wasm version and platform', ['SELECT version() AS version, (SELECT platform FROM pragma_platform()) AS platform']);
  await item('LOAD the extension', [`LOAD '${base}/ext/dax_sql.duckdb_extension.wasm'`, 'SELECT dax_sql_version() AS version']);
  await item('the parser override is on', ["SELECT current_setting('allow_parser_override_extension') AS setting"]);
  await item('contoso tables over HTTP', [...tables.map(t => `CREATE TABLE "${t}" AS SELECT * FROM ${csv(t)}`), 'SELECT count(*) AS sales FROM "Sales"']);
  await item('CREATE SEMANTIC VIEW from a .bim over HTTP', [`CREATE SEMANTIC VIEW contoso FROM '${base}/data/contoso/model.bim'`, 'SHOW SEMANTIC VIEWS']);
  await item('a model query', ['SELECT Product.Color, AGG(Orders) AS orders, AGG("Sales Amount") AS amount FROM contoso GROUP BY ALL ORDER BY Color']);
  await item('a model query in a CTE, joined',
    ['WITH m AS (SELECT Category.Category, AGG(Quantity) AS q FROM contoso GROUP BY ALL) SELECT m.*, c."CategoryKey" FROM m JOIN "Category" c USING ("Category") ORDER BY 1']);
  await item('DESCRIBE a view', ["SELECT column_name, column_type, extra FROM (DESCRIBE contoso) WHERE extra = 'measure' LIMIT 4"]);
  await item('SHOW SEMANTIC METRICS', ['SELECT name, data_type FROM (SHOW SEMANTIC METRICS IN contoso) LIMIT 4']);
  await item('dax_sql_rewrite', ["SELECT length(dax_sql_rewrite('SELECT AGG(Orders) FROM contoso')) > 0 AS rewritten"]);
  await item('CREATE SEMANTIC VIEW from an Apache Ossie .yaml over HTTP (the embedded yaml parser)', [
    `CREATE SEMANTIC VIEW aac FROM '${base}/data/aac/ossie.yaml'`,
    "SELECT count(*) AS metrics, length(dax_sql_rewrite('SELECT dim_duid.Region, AGG(\"Registered MW\") FROM aac GROUP BY ALL')) > 0 AS rewritten FROM (SHOW SEMANTIC METRICS IN aac)",
  ]);
  await item('deep measures (time intelligence, ALLSELECTED)',
    ['SELECT "Date".Year, "Date".Month, AGG("Sales YTD") AS ytd, AGG("Sales PY") AS py, AGG("Share of Category") AS share FROM contoso GROUP BY ALL ORDER BY Year, Month'], true);
  // Without OneLake (no token yet), a Direct Lake view reads the same schema.entity tables from the
  // search path: here nem.duckdb, a copy of the lakehouse CI made, attached over HTTP (httpfs,
  // autoloaded from the page's extension repository).
  await item('ATTACH a DuckDB file over HTTP', [
    `ATTACH '${base}/data/nem.duckdb' AS lakehouse_copy (READ_ONLY)`,
    'SELECT count(*) AS duids FROM lakehouse_copy.mart.dim_duid',
  ]);
  await item('a Direct Lake .bim over a DuckDB copy of its lakehouse', [
    `CREATE SEMANTIC VIEW nem FROM '${base}/data/nem.bim'`,
    'USE lakehouse_copy',
    'SELECT dim_region.Region, AGG("Registered units") AS units, AGG("Registered MW") AS mw FROM nem GROUP BY ALL ORDER BY Region',
  ]);
  await item('where the Direct Lake view reads from', ["SELECT tables_from FROM (SHOW SEMANTIC VIEWS) WHERE view_name = 'nem'"]);
  // Live from OneLake in the browser. With no azure extension in wasm, the token is an http
  // secret's bearer token (a page gets it from its host, as VS Code's sign-in; run.mjs hands CI's
  // in as window.__onelakeToken). The extension attaches the Iceberg catalog with it, and reads the
  // abfss:// files over https through httpfs (src/onelake_fs.cpp).
  const token = window.__onelakeToken;
  if (token) {
    await item('a Direct Lake view live from OneLake', [
      'USE memory',
      `CREATE SECRET onelake_http (TYPE http, BEARER_TOKEN '${token}')`,
      // OneLake's file reads in the browser fail httpfs's ETag check ("changed after it was opened").
      'SET unsafe_disable_etag_checks = true',
      `CREATE SEMANTIC VIEW nem_live FROM '${base}/data/nem.bim'`,
      "SELECT tables_from FROM (SHOW SEMANTIC VIEWS) WHERE view_name = 'nem_live'",
    ]);
    await item('a query over OneLake in the browser',
      ['SELECT dim_region.Region, AGG("Registered units") AS units, AGG("Registered MW") AS mw FROM nem_live GROUP BY ALL ORDER BY Region']);
  }
}

main().then(() => { window.__done = true; }, e => { window.__fatal = String(e?.stack ?? e); });
