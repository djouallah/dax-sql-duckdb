// dax_sql's glue: what DuckDB needs from dax-sql, and nothing of the compiler's own
// (EXTENSION.md, The compiler does not drift from dax-sql). dax-sql's createViews keeps the
// semantic views and reads their statements (CREATE, DROP, REFRESH, SHOW, DESCRIBE, and each
// model query compiled in place); dax-sql's readers read their files. What is the extension's
// is here: where a view's tables are read from (OneLake attached, or the search path), the frame
// (its tables pinned to their snapshots, the data checked there), and the SQL that stores a view
// in its schema's dax_sql_semantic_views.
// The extension (src/js_engine.cpp) calls the functions on globalThis.dax_sql with strings;
// an error carries a `code`: dax-sql's SYNTAX, SEMANTIC, UNSUPPORTED, CATALOG or INPUT.
import { createViews } from 'dax-sql/src/index.js';
import { tmslToModel, directLakeTables } from 'dax-sql/formats/tmsl.js';
import { readModel, modelFiles } from 'dax-sql/formats/model.js';
import { parse as parseYaml } from 'yaml/dist/index.js';

const lc = s => String(s).toLowerCase();
const str = s => (s == null ? 'NULL' : `'${String(s).replace(/'/g, "''")}'`);
const ident = s => `"${String(s).replace(/"/g, '""')}"`;
const fail = (message, code) => Object.assign(new Error(message), { code });

// The views, dax-sql's: SHOW SEMANTIC VIEWS lists the extension's data_as_of after its own columns.
const views = createViews({ viewColumns: [['data_as_of', 'VARCHAR']] });

// What the extension keeps of each view beside dax-sql's, by the view's full name (db, schema,
// name): { db, schema, name, path, bim, lake, offline, frame, frameError }, or { ..., error } for
// one stored in a database whose file no longer reads.
const hosts = new Map();
const keyOf = v => [v.db, v.schema, v.name].map(lc).join('\u0000');

// The extension's record of the view a name means (in full or in part, as dax-sql resolves it).
function host(name) {
  const v = views.get(name);
  if (!v) throw fail(`semantic view ${name} does not exist`, 'CATALOG');
  const h = hosts.get(keyOf(v));
  if (h.error != null) throw fail(`semantic view ${h.name}: ${h.error}`, 'INPUT');
  return h;
}

// The bim's Direct Lake tables, where dax-sql's directLakeTables says they are (a lakehouse named
// by its workspace's and its own GUIDs), each with the catalog its lakehouse is attached as. A
// table named by its lakehouse's name in the model's workspace is not attached yet: it is read
// from the search path.
function oneLake(bim) {
  const tables = new Map();
  for (const [table, l] of directLakeTables(bim)) {
    // A table read through a Fabric SQL endpoint (viaSqlEndpoint) names no lakehouse to attach: it
    // is read from the search path, as a table of no lakehouse is (#9).
    if (l.inWorkspace || !l.workspace || !l.item || l.viaSqlEndpoint) continue;
    tables.set(table, { ...l, catalog: lakeCatalogs.get(lc(l.item)) ?? `dax_sql_onelake_${l.item.replace(/-/g, '')}` });
  }
  return tables;
}

// The catalog each lakehouse is read from (its GUID, lower case → the catalog), as the extension
// chose it: the lakehouse's name in Fabric, or the GUID name when that is not found. Every view
// over the lakehouse reads it there.
const lakeCatalogs = new Map();

function setCatalog(item, catalog) {
  lakeCatalogs.set(lc(item), catalog);
  for (const h of hosts.values()) {
    if (!h.lake?.size) continue;
    let changed = false;
    for (const l of h.lake.values()) {
      if (lc(l.item) === lc(item) && l.catalog !== catalog) {
        l.catalog = catalog;
        changed = true;
      }
    }
    if (changed) build(h);
  }
  return '';
}

// A model's file as TMSL, by dax-sql's readModel (the format its path says, the BOM dropped, a
// model none of whose metrics can be read refused). A folder comes from the extension as one JSON
// object, { "<path in it>": "<text>" }: its .tmdl files (TMDL), or else its .yaml files (a Fabric
// semantic view's folder of Apache Ossie files, merged as Fabric merges them).
function toTmsl(path, text) {
  let input = String(text);
  // A folder is the extension's JSON of its files' texts (an object of strings); a .bim is JSON too,
  // but never all strings. readModel decides the format of either (#11).
  try {
    const files = JSON.parse(input);
    if (files && typeof files === 'object' && !Array.isArray(files) && Object.values(files).every(v => typeof v === 'string')) {
      input = files;
    }
  } catch {}
  return readModel(path, input, { parseYaml }).bim;
}

// The view defined in dax-sql from its model and what is known of where its tables are and what
// they hold: a Direct Lake table is read from its attached lakehouse, at the snapshot the view is
// framed on when it has one (`AT (VERSION => …)`, in a subquery, since DuckDB takes no alias
// after it); any other table, and every table of a view whose lakehouse is not attached, as
// dax-sql places it by default (its schema.entity on the search path). The frame's facts go to
// dax-sql's facts option; SHOW SEMANTIC VIEWS gets where the tables are and the frame.
function build(h) {
  const options = {};
  const pins = h.frame?.pins ?? {};
  // A view kept in a database other than the session's first (a file attached later) reads its
  // other tables there, not on the search path: dax-sql's default relation (`fallback`) qualified
  // with that database, and with the view's schema when the table's source names none, so that
  // DuckDB reads it as catalog.schema.table.
  const own = (t, fallback) =>
    h.own ? `${ident(h.own)}.${t.source?.schema ? '' : `${ident(h.schema)}.`}${fallback}` : undefined;
  if (h.lake.size && !h.offline) {
    options.tableSource = (t, fallback) => {
      const l = h.lake.get(lc(t.name));
      if (!l) return own(t, fallback);
      const name = `${ident(l.catalog)}.${ident(l.schema)}.${ident(l.entity)}`;
      const at = pins[lc(t.name)];
      return at == null ? name : `(SELECT * FROM ${name} AT (VERSION => ${at}))`;
    };
  } else if (h.own) {
    options.tableSource = own;
  }
  const facts = h.frame?.facts;
  if (facts && Object.keys(facts).length) options.facts = facts;
  views.define(h.name, tmslToModel(h.bim), {
    path: h.path, db: h.db, schema: h.schema, mode: 'replace', tablesFrom: tablesFrom(h),
    columns: { data_as_of: dataAsOf(h) }, ...options,
  });
  h.compiler = views.get(viewName(h)).compiler;
}

// A view's name in full, as SQL writes it.
const viewName = h => [h.db, h.schema, h.name].map(ident).join('.');

// Defines a view from its file's text. mode: 'create' (an error if it exists), 'replace',
// 'if_not_exists', or 'restore' (a view stored in a database: an error is kept, not thrown).
// own: the database to read the view's other tables in ('' : the search path). Returns 'created'
// or 'exists', and the OneLake catalogs to attach, one per line after it.
function define(db, schema, name, path, text, mode, own = '') {
  const old = hosts.get(keyOf({ db, schema, name }));
  if (old && mode === 'create') throw fail(`semantic view ${name} already exists`, 'CATALOG');
  if (old && mode === 'if_not_exists') return 'exists';
  let h;
  try {
    const bim = toTmsl(path, text);
    h = { db, schema, name, path, own, bim, lake: oneLake(bim), offline: null, frame: null, frameError: null };
  } catch (e) {
    if (mode !== 'restore') throw e;
    hosts.set(keyOf({ db, schema, name }), { db, schema, name, path, error: e.message });
    views.define(name, null, { path, db, schema, mode: 'restore', error: e.message, tablesFrom: 'search path' });
    return 'created';
  }
  build(h);
  hosts.set(keyOf(h), h);
  return ['created', ...catalogLines(h)].join('\n');
}

// The view's lakehouses, one line each, "<catalog> <workspace>/<item>": what the extension attaches.
function catalogLines(h) {
  const catalogs = [...new Map([...(h.lake?.values() ?? [])].map(l => [l.catalog, l])).values()];
  return catalogs.map(l => `${l.catalog} ${l.workspace}/${l.item}`);
}

// A Direct Lake view whose lakehouse the extension could not attach (no azure extension, as in
// the browser; no secret; no network): its tables are read as their schema.entity on the search
// path instead, such as a DuckDB file holding a copy of the lakehouse. `reason` is the attach's
// error, which SHOW SEMANTIC VIEWS gives.
function localize(name, reason) {
  const h = host(name);
  h.offline = reason || 'not attached';
  h.frame = null;
  build(h);
  return '';
}

// DROP SEMANTIC VIEW (the grammar's dax_sql_drop): the view forgotten; its database, schema and
// name, tab-separated, so the extension deletes its stored row; '' when IF EXISTS finds none.
function forget(name, ifExists) {
  const v = views.get(name);
  if (!v) {
    if (ifExists === 'true') return '';
    throw fail(`semantic view ${name} does not exist`, 'CATALOG');
  }
  views.drop(viewName(v), {});
  hosts.delete(keyOf(v));
  return [v.db, v.schema, v.name].join('\t');
}

// ---------------------------------------------------------------------------------------------
// The frame: what the data held when the view was created, loaded or refreshed (EXTENSION.md,
// Facts from the data). The extension runs each statement and gives back its one row, the
// values tab-separated; a step that fails leaves the view unpinned, or pinned with no answers.

// The view's OneLake tables (model table names, lower case), in the order frameSql reads them.
const lakeTables = h => (h.lake.size && !h.offline ? [...h.lake.keys()] : []);

// The statement that reads the snapshot each OneLake table of the view is at now: one row, a
// column per table. '' when the view reads no OneLake table. The view is unframed meanwhile.
function frameSql(name) {
  const h = host(name);
  h.frame = null;
  h.frameError = null;
  build(h);
  const tables = lakeTables(h);
  if (!tables.length) return '';
  const cols = tables.map((t, i) => {
    const l = h.lake.get(t);
    const at = `${ident(l.catalog)}.${ident(l.schema)}.${ident(l.entity)}`;
    return `(SELECT snapshot_id FROM iceberg_snapshots(${str(at)}) ORDER BY sequence_number DESC LIMIT 1)::VARCHAR AS s${i}`;
  });
  return `SELECT ${cols.join(', ')}`;
}

// The snapshots frameSql read (tab-separated, in its order) and when: the view is pinned to them.
// Returns the statement that checks the data at those snapshots (dax-sql's factsCheck: which
// relationships' many sides hold keys their one side lacks, which text columns hold trailing
// spaces), '' when nothing can be checked. Only the facts that read pinned tables alone are
// checked (factsCheck's `tables`): a table read as it is now could change under its answer.
function frame(name, row, at) {
  const h = host(name);
  const vals = String(row).split('\t');
  const pins = {};
  lakeTables(h).forEach((t, i) => {
    if (/^\d+$/.test(vals[i] ?? '')) pins[t] = vals[i];
  });
  h.frame = { at, pins, facts: {}, check: null };
  build(h);
  const vouched = (h.bim?.model?.tables ?? []).map(t => t.name).filter(n => pins[lc(n)]);
  const { sql, read } = h.compiler.factsCheck({ tables: vouched });
  h.frame.check = { read };
  return sql ?? '';
}

// The row of the statement frame returned, read by dax-sql (factsCheck's read): the facts the
// view's compiler is given (only those of pinned tables were checked).
function factsFound(name, row) {
  const h = host(name);
  const found = h.frame.check.read(String(row).split('\t'));
  const facts = {};
  for (const kind of ['orphans', 'trailingSpaces']) {
    if (Object.keys(found[kind] ?? {}).length) facts[kind] = found[kind];
  }
  h.frame.facts = facts;
  build(h);
  return '';
}

// A step of the frame that failed ('pins' or 'facts'): what was found before it stays, and SHOW
// SEMANTIC VIEWS says why.
function frameFailed(name, step, error) {
  const h = host(name);
  if (step === 'pins') h.frame = null;
  else if (h.frame) h.frame.facts = {};
  h.frameError = `${step}: ${String(error).split('\n')[0]}`;
  build(h);
  return '';
}

// The frame as JSON ({ pins, orphans, trailingSpaces }), and a frame given as JSON: the drift
// check gives the SQL dax-sql writes in Node the same frame as the extension's
// (.github/scripts/drift.py), and the facts check compares answers with and without the orphans
// at the same snapshots.
function facts(name) {
  const h = host(name);
  const f = h.frame?.facts ?? {};
  // the catalog each lakehouse is read from (its name in Fabric, or the session's own tables)
  const catalogs = Object.fromEntries([...(h.lake?.values() ?? [])].map(l => [l.item, l.catalog]));
  return JSON.stringify({
    pins: h.frame?.pins ?? {}, orphans: f.orphans ?? {}, trailingSpaces: f.trailingSpaces ?? {},
    ...(Object.keys(catalogs).length ? { catalogs } : {}),
  });
}
function setFacts(name, json) {
  const f = JSON.parse(json);
  for (const [item, catalog] of Object.entries(f.catalogs ?? {})) setCatalog(item, catalog);
  const h = host(name);
  const given = {};
  if (f.orphans && Object.keys(f.orphans).length) given.orphans = f.orphans;
  if (f.trailingSpaces && Object.keys(f.trailingSpaces).length) given.trailingSpaces = f.trailingSpaces;
  h.frame = { at: h.frame?.at ?? null, pins: f.pins ?? {}, facts: given, check: null };
  build(h);
  return '';
}

// What SHOW SEMANTIC VIEWS says of the view's frame (its data_as_of).
function dataAsOf(h) {
  if (!lakeTables(h).length) return 'current data (no OneLake table to pin)';
  const f = h.frame;
  if (!f || !Object.keys(f.pins).length) return `current data (not pinned: ${h.frameError ?? 'no snapshot read'})`;
  const n = Object.keys(f.pins).length, all = lakeTables(h).length;
  const answers = Object.values(f.facts.orphans ?? {});
  const found = answers.filter(Boolean).length;
  const checked = answers.length
    ? `${answers.length} relationships checked, ${found} with keys their one side lacks`
    : `relationships not checked${h.frameError ? ` (${h.frameError})` : ''}`;
  const unpinned = lakeTables(h).filter(t => !(t in f.pins));
  const read = unpinned.length ? ` (no snapshot, read as they are now: ${unpinned.join(', ')})` : '';
  const texts = Object.entries(f.facts.trailingSpaces ?? {});
  // the keys are dax-sql's (opaque): their table and column from trailingSpacesCheck, read once
  h.textColumns ??= Object.fromEntries((h.compiler?.trailingSpacesCheck().columns ?? []).map(c => [c.key, `${c.table}.${c.column}`]));
  const trimmed = texts.filter(([, x]) => x).map(([k]) => h.textColumns[k] ?? k);
  const spaces = texts.length
    ? `; ${texts.length} text columns checked, ${trimmed.length ? `trimmed: ${trimmed.join(', ')}` : 'none with trailing spaces'}`
    : '';
  return `snapshots of ${f.at ?? 'unknown time'}, ${n} of ${all} tables pinned${read}; ${checked}${spaces}`;
}

// Where a view's tables are read from, as SHOW SEMANTIC VIEWS says (its tables_from).
function tablesFrom(h) {
  if (h.offline) return `search path (OneLake not attached: ${h.offline})`;
  if (!h.lake?.size) return 'search path';
  return `OneLake ${[...new Set([...h.lake.values()].map(l => `${l.catalog} (${l.workspace}/${l.item})`))].join(', ')}`;
}

// ---------------------------------------------------------------------------------------------
// The statements, read by dax-sql's createViews.

// One of the extension's statements (the grammar's text of it: CREATE / DROP / REFRESH / SHOW
// SEMANTIC ...), read by dax-sql's rewrite, which owns its rules and names the view in full. One
// line, tab-separated, for the extension's table function:
//   create <db> <schema> <name> <path> <mode>   ('' for a db or schema not given)
//   drop <full name> <if exists: true|false>
//   refresh <full name> <name>
//   sql <the SELECT that lists SHOW's rows>
function statement(text) {
  const out = views.rewrite(text);
  const e = out?.length === 1 ? out[0] : null;
  if (e?.create) {
    const { db, schema, name, path, mode } = e.create;
    return ['create', db ?? '', schema ?? '', name, path, mode].join('\t');
  }
  if (e?.drop) return ['drop', e.drop.name, e.drop.ifExists ? 'true' : 'false'].join('\t');
  if (e?.refresh) {
    // rewrite reports whether the view exists (#36): refreshing none is the extension's error
    if (!e.refresh.exists) throw fail(`semantic view ${e.refresh.name} does not exist`, 'CATALOG');
    return ['refresh', e.refresh.name, views.get(e.refresh.name).name].join('\t');
  }
  if (e?.sql != null) return `sql\t${e.sql}`;
  throw fail(`not a semantic view statement: ${text}`, 'INPUT');
}

// The SQL a statement that failed to bind stands for (the extension's rebind hook, and its parse
// hook): its model queries compiled in place, and DESCRIBE v and (DESCRIBE v) / (SHOW SEMANTIC
// ...) as their rows. '' when nothing in it is a view's, so DuckDB's own error stands.
function bindSql(sql) {
  const out = views.rewrite(sql);
  if (!out || out.length !== 1 || out[0].sql == null) return '';
  return out[0].sql === String(sql) ? '' : out[0].sql;
}

// The SQL DuckDB runs for a text (dax_sql_rewrite), or '' when nothing in it is a view's: the
// extension's own statements as the SQL that stores, drops or refreshes a view, the others as
// dax-sql rewrites them. A dry run changes no view.
function rewrite(sql, dry = false) {
  const out = views.rewrite(sql);
  if (!out) return '';
  const statements = [];
  for (const entry of out) {
    if (entry.create) {
      const { db, schema, name, path, mode } = entry.create;
      const table = [...[db, schema].filter(Boolean).map(ident), 'dax_sql_semantic_views'].join('.');
      statements.push(
        `CREATE TABLE IF NOT EXISTS ${table} (view_name VARCHAR, path VARCHAR, definition VARCHAR, created_on TIMESTAMP)`,
        ...(mode === 'replace' ? [`DELETE FROM ${table} WHERE lower(view_name) = lower(${str(name)})`] : []),
        `INSERT INTO ${table} SELECT * FROM dax_sql_define(${db ? str(db) : "''"}, ${schema ? str(schema) : "''"}, ${str(name)}, ${str(path)}, ${str(mode)})`,
      );
    } else if (entry.drop) {
      const v = views.get(entry.drop.name);
      if (!v) {
        statements.push(`SELECT NULL AS "Success" WHERE false`);
        continue;
      }
      if (!dry) forget(viewName(v), 'false');
      statements.push(`DELETE FROM ${ident(v.db)}.${ident(v.schema)}.dax_sql_semantic_views WHERE lower(view_name) = lower(${str(v.name)})`);
    } else if (entry.refresh) {
      statements.push(`SELECT * FROM dax_sql_refresh(${str(`REFRESH SEMANTIC VIEW ${entry.refresh.name}`)})`);
    } else {
      statements.push(entry.sql);
    }
  }
  return statements.join(';\n');
}

// The views' names, one per line, lower case: the extension calls rewrite only for a text
// that says SEMANTIC or holds one of them.
const names = () => views.list().map(v => lc(v.name)).join('\n');

globalThis.dax_sql = {
  define,
  rewrite: (sql, dry) => rewrite(sql, dry === 'dry'),
  names,
  localize,
  frame_sql: frameSql,
  frame,
  facts_found: factsFound,
  // the lines define gives after its first, for REFRESH's attach
  catalogs: name => {
    const h = host(name);
    return ['refresh', ...(h.offline ? [] : catalogLines(h))].join('\n');
  },
  frame_failed: frameFailed,
  set_catalog: setCatalog,
  facts,
  set_facts: setFacts,
  data_as_of: name => dataAsOf(host(name)),
  bind_sql: bindSql,
  statement,
  // the files of a folder (one path per line, relative to it) readModel reads, one per line (#35)
  model_files: list => modelFiles(String(list).split('\n').filter(Boolean)).join('\n'),
  forget,
  // a database detached: its views forgotten (its file keeps them)
  forget_database: db => {
    for (const v of views.list().filter(v => lc(v.db) === lc(db))) forget(viewName(v), 'true');
    return '';
  },
};
