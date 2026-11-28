// dax_sql's glue: what DuckDB needs from dax-sql, and nothing of the compiler's own
// (EXTENSION.md, The compiler does not drift from dax-sql). It keeps the semantic views, one
// compiler each, and turns a statement that names one into SQL DuckDB runs:
//   CREATE [OR REPLACE] SEMANTIC VIEW [IF NOT EXISTS] v FROM 'path'
//                                  the view's row in its schema's dax_sql_semantic_views; the
//                                  path a .bim (TMSL), a .tmdl or a folder of them (TMDL), or a
//                                  .yaml / .yml or a folder of .yaml files (Apache Ossie, as a
//                                  Fabric semantic view's folder), each read by dax-sql's formats/
//   DROP SEMANTIC VIEW [IF EXISTS] v
//   REFRESH SEMANTIC VIEW v        the view framed again on its OneLake tables' current snapshots
//   SHOW SEMANTIC VIEWS | DIMENSIONS IN v [FOR METRIC m] | METRICS IN v, DESCRIBE [SEMANTIC VIEW] v
//                                  the rows, as VALUES
//   any other statement            each model query compiled in place by compileStatement
// The extension (src/js_engine.cpp) calls the functions on globalThis.dax_sql with strings;
// an error carries a `code`: dax-sql's SYNTAX, SEMANTIC, UNSUPPORTED, or CATALOG, INPUT here.
import { createCompiler } from 'dax-sql/src/index.js';
import { tmslToModel, directLakeTables } from 'dax-sql/formats/tmsl.js';
import { readModel } from 'dax-sql/formats/model.js';
import { parse as parseYaml } from 'yaml/dist/index.js';

const lc = s => String(s).toLowerCase();
const str = s => (s == null ? 'NULL' : `'${String(s).replace(/'/g, "''")}'`);
const ident = s => `"${String(s).replace(/"/g, '""')}"`;
const fail = (message, code) => Object.assign(new Error(message), { code });

// The semantic views, by name: { db, schema, name, path, compiler } or, for one stored in a
// database whose file no longer compiles, { db, schema, name, path, error }.
const views = new Map();

// ---------------------------------------------------------------------------------------------
// Tokens: enough to split statements and read the extension's own ones. Strings, quoted
// names, $$ strings and comments are single tokens, so nothing inside them counts.
function tokens(sql) {
  const out = [];
  let i = 0;
  const n = sql.length;
  while (i < n) {
    const c = sql[i];
    if (/\s/.test(c)) { i++; continue; }
    const at = i;
    if (c === '-' && sql[i + 1] === '-') {
      while (i < n && sql[i] !== '\n') i++;
      continue;
    }
    if (c === '/' && sql[i + 1] === '*') {
      const e = sql.indexOf('*/', i + 2);
      i = e < 0 ? n : e + 2;
      continue;
    }
    if (c === "'" || c === '"') {
      i++;
      let v = '';
      while (i < n) {
        if (sql[i] === c) {
          if (sql[i + 1] === c) { v += c; i += 2; continue; }
          i++;
          break;
        }
        v += sql[i++];
      }
      out.push({ t: c === "'" ? 'str' : 'name', v, at, end: i });
      continue;
    }
    if (c === '$') {
      const m = /^\$([A-Za-z_][A-Za-z0-9_]*)?\$/.exec(sql.slice(i));
      if (m) {
        const e = sql.indexOf(m[0], i + m[0].length);
        const stop = e < 0 ? n : e;
        out.push({ t: 'str', v: sql.slice(i + m[0].length, stop), at, end: e < 0 ? n : e + m[0].length });
        i = e < 0 ? n : e + m[0].length;
        continue;
      }
    }
    if (/[A-Za-z_\u0080-￿]/.test(c)) {
      while (i < n && /[A-Za-z0-9_$\u0080-￿]/.test(sql[i])) i++;
      out.push({ t: 'word', v: sql.slice(at, i), at, end: i });
      continue;
    }
    i++;
    out.push({ t: c === ';' ? ';' : 'sym', v: c, at, end: i });
  }
  return out;
}

// The statements of a text, each { text, toks, start } (toks' positions are in the whole text);
// the separators are dropped.
function statements(sql) {
  const toks = tokens(sql);
  const out = [];
  let start = 0, from = 0;
  for (let k = 0; k <= toks.length; k++) {
    if (k < toks.length && toks[k].t !== ';') continue;
    const end = k < toks.length ? toks[k].at : sql.length;
    const own = toks.slice(from, k);
    if (own.length) out.push({ text: sql.slice(start, end), toks: own, start });
    if (k < toks.length) start = toks[k].end;
    from = k + 1;
  }
  return out;
}

// A small reader over a statement's tokens.
function reader(toks) {
  let k = 0;
  const r = {
    done: () => k >= toks.length,
    peekWord: w => toks[k]?.t === 'word' && toks[k].v.toUpperCase() === w,
    word(w) {
      if (!r.peekWord(w)) return false;
      k++;
      return true;
    },
    words(...ws) {
      const save = k;
      for (const w of ws) if (!r.word(w)) { k = save; return false; }
      return true;
    },
    expect(...ws) {
      if (!r.words(...ws)) throw fail(`syntax error: expected ${ws.join(' ')}`, 'SYNTAX');
    },
    name() {
      const t = toks[k];
      if (t?.t !== 'word' && t?.t !== 'name') throw fail('syntax error: expected a name', 'SYNTAX');
      k++;
      return t.v;
    },
    // [db.][schema.]name, as its parts.
    qualified() {
      const parts = [r.name()];
      while (toks[k]?.t === 'sym' && toks[k].v === '.') { k++; parts.push(r.name()); }
      if (parts.length > 3) throw fail(`syntax error: too many parts in ${parts.join('.')}`, 'SYNTAX');
      return parts;
    },
    string() {
      const t = toks[k];
      if (t?.t !== 'str') throw fail('syntax error: expected a quoted path', 'SYNTAX');
      k++;
      return t.v;
    },
    end() {
      if (k < toks.length) throw fail(`syntax error at or near "${toks[k].v}"`, 'SYNTAX');
    },
  };
  return r;
}

// ---------------------------------------------------------------------------------------------
// The views.

function view(name) {
  const v = views.get(lc(name));
  if (!v) throw fail(`semantic view ${name} does not exist`, 'CATALOG');
  if (v.error) throw fail(`semantic view ${v.name}: ${v.error}`, 'INPUT');
  return v;
}

// The bim's Direct Lake tables, where dax-sql's directLakeTables says they are (a lakehouse named
// by its workspace's and its own GUIDs), each with the catalog its lakehouse is attached as. A
// table named by its lakehouse's name in the model's workspace is not attached yet: it is read
// from the search path.
function oneLake(bim) {
  const tables = new Map();
  for (const [table, l] of directLakeTables(bim)) {
    if (l.inWorkspace || !l.workspace) continue;
    tables.set(table, { ...l, catalog: `dax_sql_onelake_${l.item.replace(/-/g, '')}` });
  }
  return tables;
}

// A model's file as TMSL, by dax-sql's readModel (the format its path says, the BOM dropped, a
// model none of whose metrics can be read refused). A folder comes from the extension as one JSON
// object, { "<path in it>": "<text>" }: its .tmdl files (TMDL), or else its .yaml files (a Fabric
// semantic view's folder of Apache Ossie files, merged as Fabric merges them).
function toTmsl(path, text) {
  let input = String(text);
  if (!/\.(bim|tmdl|ya?ml)$/i.test(path)) {
    try {
      const files = JSON.parse(input);
      if (files && typeof files === 'object' && !Array.isArray(files)) input = files;
    } catch {}
  }
  return readModel(path, input, { parseYaml }).bim;
}

// The view's compiler, from its model and what is known of where its tables are and what they
// hold: a Direct Lake table is read from its attached lakehouse, at the snapshot the view is
// framed on when it has one (`AT (VERSION => …)`, in a subquery, since DuckDB takes no alias
// after it); any other table, and every table of a view whose lakehouse is not attached, as its
// schema.entity on the search path. The orphan answers found at that frame go to dax-sql's
// orphans option, keyed by rel.key; the rest of its relationships are tested in the SQL.
function build(v) {
  const options = {};
  const pins = v.frame?.pins ?? {};
  if (v.lake.size && !v.offline) {
    options.tableSource = t => {
      const l = v.lake.get(lc(t.name));
      if (l) {
        const name = `${ident(l.catalog)}.${ident(l.schema)}.${ident(l.entity)}`;
        const at = pins[lc(t.name)];
        return at == null ? name : `(SELECT * FROM ${name} AT (VERSION => ${at}))`;
      }
      const s = t.source;
      return s.schema ? `${ident(s.schema)}.${ident(s.entity)}` : ident(s.entity);
    };
  }
  // What the frame found of the data at its snapshots (dax-sql's facts: the orphan keys, and the
  // text columns read trimmed as Power BI loads them)
  const facts = v.frame?.facts;
  if (facts && Object.keys(facts).length) options.facts = facts;
  v.compiler = createCompiler(tmslToModel(v.bim), options);
}

// Defines a view from its file's text. mode: 'create' (an error if it exists), 'replace',
// 'if_not_exists', or 'restore' (a view stored in a database: an error is kept, not thrown).
// Returns 'created' or 'exists', and the OneLake catalogs to attach, one per line after it.
function define(db, schema, name, path, text, mode) {
  const old = views.get(lc(name));
  if (old && mode === 'create') throw fail(`semantic view ${name} already exists`, 'CATALOG');
  if (old && mode === 'if_not_exists') return 'exists';
  try {
    const bim = toTmsl(path, text);
    const v = { db, schema, name, path, bim, lake: oneLake(bim), offline: null, frame: null, frameError: null };
    build(v);
    views.set(lc(name), v);
    const catalogs = [...new Map([...v.lake.values()].map(l => [l.catalog, l])).values()];
    return ['created', ...catalogs.map(l => `${l.catalog} ${l.workspace}/${l.item}`)].join('\n');
  } catch (e) {
    if (mode !== 'restore') throw e;
    views.set(lc(name), { db, schema, name, path, error: e.message });
    return 'created';
  }
}

// A Direct Lake view whose lakehouse the extension could not attach (no azure extension, as in
// the browser; no secret; no network): its tables are read as their schema.entity on the search
// path instead, such as a DuckDB file holding a copy of the lakehouse. `reason` is the attach's
// error, which SHOW SEMANTIC VIEWS gives.
function localize(name, reason) {
  const v = view(name);
  v.offline = reason || 'not attached';
  v.frame = null;
  build(v);
  return '';
}

// ---------------------------------------------------------------------------------------------
// The frame: what the data held when the view was created, loaded or refreshed (EXTENSION.md,
// Facts from the data). The extension runs each statement and gives back its one row, the
// values tab-separated; a step that fails leaves the view unpinned, or pinned with no answers.

// The view's OneLake tables (model table names, lower case), in the order frameSql reads them.
const lakeTables = v => (v.lake.size && !v.offline ? [...v.lake.keys()] : []);

// The statement that reads the snapshot each OneLake table of the view is at now: one row, a
// column per table. '' when the view reads no OneLake table. The view is unframed meanwhile.
function frameSql(name) {
  const v = view(name);
  v.frame = null;
  v.frameError = null;
  build(v);
  const tables = lakeTables(v);
  if (!tables.length) return '';
  const cols = tables.map((t, i) => {
    const l = v.lake.get(t);
    const at = `${ident(l.catalog)}.${ident(l.schema)}.${ident(l.entity)}`;
    return `(SELECT snapshot_id FROM iceberg_snapshots(${str(at)}) ORDER BY sequence_number DESC LIMIT 1)::VARCHAR AS s${i}`;
  });
  return `SELECT ${cols.join(', ')}`;
}

// The snapshots frameSql read (tab-separated, in its order) and when: the view is pinned to them.
// Returns the statement that checks the data at those snapshots (dax-sql's factsCheck: which
// relationships' many sides hold keys their one side lacks, which text columns hold trailing
// spaces), '' when nothing can be checked. An answer counts only when every table it reads is
// pinned (factsCheck's `tables`): a table read as it is now could change under it.
function frame(name, row, at) {
  const v = view(name);
  const vals = String(row).split('\t');
  const pins = {};
  lakeTables(v).forEach((t, i) => {
    if (/^\d+$/.test(vals[i] ?? '')) pins[t] = vals[i];
  });
  v.frame = { at, pins, facts: {}, check: null };
  build(v);
  const { sql, read, tables } = v.compiler.factsCheck();
  const pinned = list => (list ?? []).every(t => pins[lc(t)]);
  const counts = Object.values(tables).some(byKey => Object.values(byKey).some(pinned));
  v.frame.check = { read, tables, pinned };
  return counts ? sql : '';
}

// The row of the statement frame returned, read by dax-sql (factsCheck's read): the facts the
// view's compiler is given, those of tables not pinned left out.
function factsFound(name, row) {
  const v = view(name);
  const { read, tables, pinned } = v.frame.check;
  const found = read(String(row).split('\t'));
  const facts = {};
  for (const kind of ['orphans', 'trailingSpaces']) {
    const kept = Object.entries(found[kind] ?? {}).filter(([key]) => pinned(tables[kind]?.[key]));
    if (kept.length) facts[kind] = Object.fromEntries(kept);
  }
  v.frame.facts = facts;
  build(v);
  return '';
}

// A step of the frame that failed ('pins' or 'facts'): what was found before it stays, and SHOW
// SEMANTIC VIEWS says why.
function frameFailed(name, step, error) {
  const v = view(name);
  if (step === 'pins') v.frame = null;
  else if (v.frame) v.frame.facts = {};
  v.frameError = `${step}: ${String(error).split('\n')[0]}`;
  build(v);
  return '';
}

// The frame as JSON ({ pins, orphans, trailingSpaces }), and a frame given as JSON: the drift
// check gives the SQL dax-sql writes in Node the same frame as the extension's
// (.github/scripts/drift.py), and the facts check compares answers with and without the orphans
// at the same snapshots.
function facts(name) {
  const v = view(name);
  const f = v.frame?.facts ?? {};
  return JSON.stringify({ pins: v.frame?.pins ?? {}, orphans: f.orphans ?? {}, trailingSpaces: f.trailingSpaces ?? {} });
}
function setFacts(name, json) {
  const v = view(name);
  const f = JSON.parse(json);
  const given = {};
  if (f.orphans && Object.keys(f.orphans).length) given.orphans = f.orphans;
  if (f.trailingSpaces && Object.keys(f.trailingSpaces).length) given.trailingSpaces = f.trailingSpaces;
  v.frame = { at: v.frame?.at ?? null, pins: f.pins ?? {}, facts: given, check: null };
  build(v);
  return '';
}

// What SHOW SEMANTIC VIEWS says of the view's frame.
function dataAsOf(v) {
  if (!lakeTables(v).length) return 'current data (no OneLake table to pin)';
  const f = v.frame;
  if (!f || !Object.keys(f.pins).length) return `current data (not pinned: ${v.frameError ?? 'no snapshot read'})`;
  const n = Object.keys(f.pins).length, all = lakeTables(v).length;
  const answers = Object.values(f.facts.orphans ?? {});
  const found = answers.filter(Boolean).length;
  const checked = answers.length
    ? `${answers.length} relationships checked, ${found} with keys their one side lacks`
    : `relationships not checked${v.frameError ? ` (${v.frameError})` : ''}`;
  const unpinned = lakeTables(v).filter(t => !(t in f.pins));
  const read = unpinned.length ? ` (no snapshot, read as they are now: ${unpinned.join(', ')})` : '';
  const texts = Object.entries(f.facts.trailingSpaces ?? {});
  const trimmed = texts.filter(([, x]) => x).map(([k]) => k.replace('|', '.'));
  const spaces = texts.length
    ? `; ${texts.length} text columns checked, ${trimmed.length ? `trimmed: ${trimmed.join(', ')}` : 'none with trailing spaces'}`
    : '';
  return `snapshots of ${f.at ?? 'unknown time'}, ${n} of ${all} tables pinned${read}; ${checked}${spaces}`;
}

// Where a view's tables are read from, as SHOW SEMANTIC VIEWS says.
function tablesFrom(v) {
  if (v.offline) return `search path (OneLake not attached: ${v.offline})`;
  if (!v.lake?.size) return 'search path';
  return `OneLake ${[...new Set([...v.lake.values()].map(l => `${l.workspace}/${l.item}`))].join(', ')}`;
}

// ---------------------------------------------------------------------------------------------
// What the inspecting statements list.

const DUCK_TYPES = { int: 'BIGINT', double: 'DOUBLE', decimal: 'DOUBLE', string: 'VARCHAR', datetime: 'TIMESTAMP', bool: 'BOOLEAN', variant: 'VARCHAR' };
const duckType = t => DUCK_TYPES[t] ?? 'VARCHAR';

// Rows as a SELECT over VALUES; with none, the columns typed and no row.
function rows(columns, list) {
  const names = columns.map(([n]) => ident(n)).join(', ');
  if (!list.length) {
    const nulls = columns.map(([, t]) => `NULL::${t}`).join(', ');
    return `SELECT * FROM (VALUES (${nulls})) t(${names}) WHERE false`;
  }
  const cell = (x, t) => (x == null ? `NULL::${t}` : t === 'BOOLEAN' ? (x ? 'true' : 'false') : str(x));
  const values = list.map(r => `(${r.map((x, i) => cell(x, columns[i][1])).join(', ')})`).join(',\n  ');
  return `SELECT * FROM (VALUES\n  ${values}) t(${names})`;
}

const ITEM_COLUMNS = [['database_name', 'VARCHAR'], ['schema_name', 'VARCHAR'], ['view_name', 'VARCHAR'], ['table_name', 'VARCHAR'],
  ['name', 'VARCHAR'], ['data_type', 'VARCHAR'], ['expression', 'VARCHAR'], ['format_string', 'VARCHAR'], ['hidden', 'BOOLEAN'],
  ['comment', 'VARCHAR']];

// The model as dax-sql lists it (describe(), dimensionsOf(): its API, not its own objects).
function dimensions(v, tables = v.compiler.describe().tables) {
  const out = [];
  for (const t of tables) {
    for (const c of t.columns) out.push([v.db, v.schema, v.name, t.name, c.name, duckType(c.type), c.expression, null, t.hidden, null]);
  }
  return out;
}

function metrics(v) {
  return v.compiler.describe().measures.map(m =>
    [v.db, v.schema, v.name, m.table, m.name, duckType(m.type), m.expression, m.formatString, false, null]);
}

function describe(v) {
  const cols = [['column_name', 'VARCHAR'], ['column_type', 'VARCHAR'], ['null', 'VARCHAR'], ['key', 'VARCHAR'], ['default', 'VARCHAR'], ['extra', 'VARCHAR']];
  const list = [];
  const model = v.compiler.describe();
  for (const t of model.tables) {
    for (const c of t.columns) list.push([`${t.name}.${c.name}`, duckType(c.type), c.nullable ? 'YES' : 'NO', c.unique ? 'PRI' : null, null, 'column']);
  }
  for (const m of model.measures) list.push([m.name, duckType(m.type), 'YES', null, null, 'measure']);
  return rows(cols, list);
}

// ---------------------------------------------------------------------------------------------
// The statements.

// The extension's own statements, or null for any other.
function ownStatement(toks) {
  const r = reader(toks);
  if (r.word('CREATE')) {
    const replace = r.words('OR', 'REPLACE');
    if (!r.words('SEMANTIC', 'VIEW')) return null;
    const ifNotExists = r.words('IF', 'NOT', 'EXISTS');
    const parts = r.qualified();
    r.expect('FROM');
    const path = r.string();
    r.end();
    const name = parts[parts.length - 1];
    const table = [...parts.slice(0, -1).map(ident), 'dax_sql_semantic_views'].join('.');
    const mode = replace ? 'replace' : ifNotExists ? 'if_not_exists' : 'create';
    // [db.][schema.]name: the database and the schema; '' is the session's (dax_sql_define).
    const db = parts.length === 3 ? str(parts[0]) : "''";
    const schema = parts.length > 1 ? str(parts[parts.length - 2]) : "''";
    return [
      `CREATE TABLE IF NOT EXISTS ${table} (view_name VARCHAR, path VARCHAR, definition VARCHAR, created_on TIMESTAMP)`,
      ...(replace ? [`DELETE FROM ${table} WHERE lower(view_name) = lower(${str(name)})`] : []),
      `INSERT INTO ${table} SELECT * FROM dax_sql_define(${db}, ${schema}, ${str(name)}, ${str(path)}, ${str(mode)})`,
    ];
  }
  if (r.word('DROP')) {
    if (!r.words('SEMANTIC', 'VIEW')) return null;
    const ifExists = r.words('IF', 'EXISTS');
    const parts = r.qualified();
    r.end();
    const name = parts[parts.length - 1];
    const v = views.get(lc(name));
    if (!v) {
      if (ifExists) return [`SELECT NULL AS "Success" WHERE false`];
      throw fail(`semantic view ${name} does not exist`, 'CATALOG');
    }
    if (!dryRun) views.delete(lc(name));
    return [`DELETE FROM ${ident(v.db)}.${ident(v.schema)}.dax_sql_semantic_views WHERE lower(view_name) = lower(${str(v.name)})`];
  }
  if (r.word('REFRESH')) {
    if (!r.words('SEMANTIC', 'VIEW')) return null;
    const v = view(r.qualified().pop());
    r.end();
    return [`SELECT * FROM dax_sql_refresh(${str(v.name)})`];
  }
  if (r.word('SHOW')) {
    if (!r.word('SEMANTIC')) return null;
    if (r.word('VIEWS')) {
      r.end();
      return [show('views', '', '')];
    }
    const dims = r.word('DIMENSIONS');
    if (!dims) r.expect('METRICS');
    r.expect('IN');
    const name = r.qualified().pop();
    const metric = dims && r.words('FOR', 'METRIC') ? r.name() : '';
    r.end();
    return [show(dims ? 'dimensions' : 'metrics', name, metric)];
  }
  if (r.word('DESCRIBE') || r.word('DESC')) {
    // DESCRIBE SEMANTIC VIEW v names a view, so one that does not exist is an error
    const semantic = r.words('SEMANTIC', 'VIEW');
    if (r.done()) return null;
    let parts;
    try {
      parts = r.qualified();
      r.end();
    } catch {
      return null;
    }
    const v = views.get(lc(parts[parts.length - 1]));
    if (!v && !semantic) return null;
    return [describe(view(v ? v.name : parts[parts.length - 1]))];
  }
  return null;
}

// What SHOW SEMANTIC lists, as a SELECT over VALUES: kind 'views', 'metrics' or 'dimensions' (of
// view `name`; with `metric`, only those that filter it). The grammar's dax_sql_show runs it.
const VIEW_COLUMNS = [['database_name', 'VARCHAR'], ['schema_name', 'VARCHAR'], ['view_name', 'VARCHAR'], ['path', 'VARCHAR'],
  ['tables_from', 'VARCHAR'], ['data_as_of', 'VARCHAR']];
function show(kind, name, metric) {
  if (kind === 'views') {
    return rows(VIEW_COLUMNS, [...views.values()].map(v => [v.db, v.schema, v.name, v.path, tablesFrom(v), v.error ? null : dataAsOf(v)]));
  }
  const v = view(name);
  if (kind === 'metrics') return rows(ITEM_COLUMNS, metrics(v));
  if (!metric) return rows(ITEM_COLUMNS, dimensions(v));
  const m = v.compiler.describe().measures.find(x => lc(x.name) === lc(metric));
  if (!m) throw fail(`semantic view ${v.name} has no metric ${metric}`, 'CATALOG');
  return rows(ITEM_COLUMNS, dimensions(v, v.compiler.dimensionsOf(m.name)));
}

// DROP SEMANTIC VIEW (the grammar's dax_sql_drop): the view forgotten; its database, schema and
// name, tab-separated, so the extension deletes its stored row; '' when IF EXISTS finds none.
function forget(name, ifExists) {
  const v = views.get(lc(name));
  if (!v) {
    if (ifExists === 'true') return '';
    throw fail(`semantic view ${name} does not exist`, 'CATALOG');
  }
  views.delete(lc(name));
  return [v.db, v.schema, v.name].join('	');
}

// A statement that failed to bind (the extension's rebind hook): its text with each model query
// compiled in place, and DESCRIBE v and (DESCRIBE v) / (SHOW SEMANTIC ...) as their rows. '' when
// nothing in it is the extension's, so DuckDB's own error stands.
function bindSql(sql) {
  const list = statements(sql);
  if (list.length !== 1) return '';
  const s = list[0];
  const r = reader(s.toks);
  if (r.word('DESCRIBE') || r.word('DESC')) {
    const semantic = r.words('SEMANTIC', 'VIEW');
    let parts = null;
    try {
      parts = r.qualified();
      r.end();
    } catch {
      parts = null;
    }
    if (parts) {
      const v = views.get(lc(parts[parts.length - 1]));
      if (v) return describe(view(v.name));
      // DESCRIBE SEMANTIC VIEW names a view: one that does not exist is an error of its own
      if (semantic) view(parts[parts.length - 1]);
    }
  }
  const out = statementSql(s);
  return out === s.text ? '' : out;
}

// A query's text, its model queries compiled in place; the same text when it names no view.
function compiled(text, toks) {
  let out = text;
  for (const key of views.keys()) {
    if (!toks.some(t => (t.t === 'word' || t.t === 'name') && lc(t.v) === key)) continue;
    const v = view(key);
    out = v.compiler.compileStatement(out, v.name).sql;
  }
  return out;
}

// A statement's text with each (DESCRIBE v) and (SHOW SEMANTIC ...) in it replaced by the rows
// it lists, so they can be read as subqueries.
function nested(s) {
  const { toks } = s;
  const found = [];
  for (let k = 0; k < toks.length; k++) {
    const w = toks[k + 1];
    if (toks[k].t !== 'sym' || toks[k].v !== '(' || w?.t !== 'word' || !/^(DESCRIBE|DESC|SHOW|EXPLAIN)$/i.test(w.v)) continue;
    let depth = 0, e = k;
    for (; e < toks.length; e++) {
      if (toks[e].t !== 'sym') continue;
      if (toks[e].v === '(') depth++;
      else if (toks[e].v === ')' && --depth === 0) break;
    }
    if (e >= toks.length) break;
    const inner = toks.slice(k + 1, e);
    const start = toks[k].end - s.start, end = toks[e].at - s.start;
    const own = ownStatement(inner);
    if (own?.length === 1) found.push({ start, end, sql: own[0] });
    // (DESCRIBE SELECT ...), (EXPLAIN SELECT ...): the query after the prefix compiled.
    else if (!own) found.push({ start, end, sql: statementSql({ text: s.text.slice(start, end), toks: inner, start: toks[k].end }) });
    k = e;
  }
  let out = s.text;
  for (const f of found.reverse()) out = `${out.slice(0, f.start)}${f.sql}${out.slice(f.end)}`;
  return out;
}

// Where a statement's query starts when something comes before it (EXPLAIN, DESCRIBE,
// CREATE TABLE ... AS, INSERT INTO ...): its first SELECT or WITH outside parentheses after the
// first token; -1 when there is none.
function queryStart(toks) {
  let depth = 0;
  for (let k = 0; k < toks.length; k++) {
    const t = toks[k];
    if (t.t === 'sym' && t.v === '(') depth++;
    else if (t.t === 'sym' && t.v === ')') depth--;
    else if (k > 0 && depth === 0 && t.t === 'word' && /^(SELECT|WITH)$/i.test(t.v)) return k;
  }
  return -1;
}

// A statement that is not the extension's own, as DuckDB runs it: the query after any prefix
// compiled as a statement of its own, so that a model query can follow EXPLAIN or AS.
function statementSql(s) {
  const first = s.toks[0];
  const k = first?.t === 'word' && !/^(SELECT|WITH)$/i.test(first.v) ? queryStart(s.toks) : -1;
  if (k < 0) return compiled(nested(s), s.toks);
  const at = s.toks[k].at;
  const head = { text: s.text.slice(0, at - s.start), toks: s.toks.slice(0, k), start: s.start };
  const tail = { text: s.text.slice(at - s.start), toks: s.toks.slice(k), start: at };
  return nested(head) + compiled(nested(tail), tail.toks);
}

// A dry run rewrites without changing the views (DROP SEMANTIC VIEW forgets one as it is
// rewritten): what dax_sql_rewrite() shows.
let dryRun = false;

// The SQL DuckDB runs for a text, or null when nothing in it is the extension's.
function rewrite(sql, dry = false) {
  dryRun = dry;
  try {
    let changed = false;
    const out = [];
    for (const s of statements(sql)) {
      const own = ownStatement(s.toks);
      if (own) {
        changed = true;
        out.push(...own);
        continue;
      }
      const c = statementSql(s);
      if (c !== s.text) changed = true;
      out.push(c);
    }
    return changed ? out.join(';\n') : null;
  } finally {
    dryRun = false;
  }
}

// The views' names, one per line, lower case: the extension calls rewrite only for a text
// that says SEMANTIC or holds one of them.
const names = () => [...views.keys()].join('\n');

globalThis.dax_sql = {
  define,
  rewrite: (sql, dry) => rewrite(sql, dry === 'dry') ?? '',
  names,
  localize,
  frame_sql: frameSql,
  frame,
  facts_found: factsFound,
  frame_failed: frameFailed,
  facts,
  set_facts: setFacts,
  data_as_of: name => dataAsOf(view(name)),
  bind_sql: bindSql,
  show,
  forget,
};
