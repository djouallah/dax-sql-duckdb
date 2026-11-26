// A model.bim written as TMDL, for the tests of CREATE SEMANTIC VIEW from TMDL: as one file (a
// script, as Fabric publishes a semantic view) and as a folder (a PBIP's definition/). The writer
// is dax-sql's own test writer (formats/test/tmdl.test.js), copied as it is: test code, not the
// compiler's, which reads TMDL with formats/tmdl.js.
//   node test/tools/bim_to_tmdl.mjs <model.bim> <out>   ->  <out>.tmdl and <out>/definition/...
import { readFileSync, writeFileSync, mkdirSync } from 'node:fs';
import { dirname, join } from 'node:path';

// --- a TMSL model written as TMDL, as Power BI writes it ---------------------------------------
const T = d => '\t'.repeat(d);
const q = n => (/^[A-Za-z_]\w*$/.test(n) ? n : `'${n.replace(/'/g, "''")}'`);
const text = e => (Array.isArray(e) ? e.join('\n') : String(e));
// An expression after `=`: on its line, or on the lines below at depth `at`.
const after = (e, at) => (text(e).includes('\n') ? '\n' + text(e).split('\n').map(l => T(at) + l).join('\n') : ' ' + text(e));
const SKIP = new Set(['name', 'description', 'type', 'expression', 'columns', 'measures', 'partitions', 'calculationGroup',
  'calculationItems', 'annotations', 'extendedProperties', 'tablePermissions', 'columnPermissions', 'filterExpression']);
function obj(d, type, name, value, o = {}, kids = []) {
  let out = o.description ? o.description.split('\n').map(l => `${T(d)}/// ${l}\n`).join('') : '';
  out += `${T(d)}${type}${name != null ? ' ' + q(name) : ''}${value != null ? ' =' + after(value, d + 2) : ''}\n`;
  for (const [k, v] of Object.entries(o)) {
    if (SKIP.has(k) || v == null || v === false && k.startsWith('is') && k !== 'isActive' && k !== 'isNullable') continue;
    if (v === true) out += `${T(d + 1)}${k}\n`;
    else if (typeof v === 'object' && !Array.isArray(v) && v.expression != null) out += `${T(d + 1)}${k} =${after(v.expression, d + 2)}\n`;
    else if (typeof v !== 'object') out += `${T(d + 1)}${k}: ${v}\n`;
  }
  return out + '\n' + kids.join('');
}
const column = (c, d) => obj(d, 'column', c.name, c.type === 'calculated' ? c.expression : null, c,
  (c.extendedProperties ?? []).map(p => obj(d + 1, 'extendedProperty', p.name, JSON.stringify(p.value))));
const measure = (m, d) => obj(d, 'measure', m.name, m.expression, m,
  (m.annotations ?? []).map(a => obj(d + 1, 'annotation', a.name, a.value)));
function partition(p, d) {
  const s = p.source ?? {};
  if (s.type === 'calculated') return obj(d, 'partition', p.name, 'calculated', { source: { expression: s.expression } });
  if (s.type === 'calculationGroup') return obj(d, 'partition', p.name, 'calculationGroup');
  const { type, ...where } = s;
  return obj(d, 'partition', p.name, type, { mode: p.mode }, [obj(d + 1, 'source', null, null, where)]);
}
const calculationGroup = (g, d) => obj(d, 'calculationGroup', null, null, g,
  (g.calculationItems ?? []).map(it => obj(d + 1, 'calculationItem', it.name, it.expression, it)));
const table = (t, d) => obj(d, 'table', t.name, null, t, [
  ...(t.calculationGroup ? [calculationGroup(t.calculationGroup, d + 1)] : []),
  ...(t.columns ?? []).map(c => column(c, d + 1)),
  ...(t.measures ?? []).map(m => measure(m, d + 1)),
  ...(t.partitions ?? []).map(p => partition(p, d + 1)),
  ...(t.annotations ?? []).map(a => obj(d + 1, 'annotation', a.name, a.value)),
]);
const relationship = (r, i, d) => obj(d, 'relationship', r.name ?? `rel${i}`, null,
  { ...r, fromTable: undefined, toTable: undefined, fromColumn: `${q(r.fromTable)}.${q(r.fromColumn)}`, toColumn: `${q(r.toTable)}.${q(r.toColumn)}` });
const role = (r, d) => obj(d, 'role', r.name, null, r, (r.tablePermissions ?? []).map(p =>
  obj(d + 1, 'tablePermission', p.name, p.filterExpression ?? null, p,
    (p.columnPermissions ?? []).map(c => obj(d + 2, 'columnPermission', c.name, null, c)))));

// One file: a script, a database, its model, as Fabric publishes a semantic view.
const script = bim => 'createOrReplace\n\n' + obj(1, 'database', bim.name ?? 'Model', null, { compatibilityLevel: bim.compatibilityLevel }, [
  obj(2, 'model', 'Model', null, { culture: 'en-US' }, [
    ...bim.model.tables.map(t => table(t, 3)),
    ...(bim.model.relationships ?? []).map((r, i) => relationship(r, i, 3)),
    ...(bim.model.roles ?? []).map(r => role(r, 3)),
  ])]);
// A folder: each object in a file of its own, at the file's root, as a PBIP's definition/ holds them.
const folder = bim => ({
  'database.tmdl': obj(0, 'database', bim.name ?? 'Model', null, { compatibilityLevel: bim.compatibilityLevel }),
  'model.tmdl': obj(0, 'model', 'Model', null, { culture: 'en-US' }) + bim.model.tables.map(t => `ref table ${q(t.name)}\n`).join(''),
  ...Object.fromEntries(bim.model.tables.map(t => [`tables/${t.name}.tmdl`, table(t, 0)])),
  'relationships.tmdl': (bim.model.relationships ?? []).map((r, i) => relationship(r, i, 0)).join(''),
  ...Object.fromEntries((bim.model.roles ?? []).map(r => [`roles/${r.name}.tmdl`, role(r, 0)])),
});

const [bimPath, out] = process.argv.slice(2);
if (!bimPath || !out) throw new Error('usage: node bim_to_tmdl.mjs <model.bim> <out>');
const bim = JSON.parse(readFileSync(bimPath, 'utf8').replace(/^﻿/, ''));
mkdirSync(dirname(out), { recursive: true });
writeFileSync(`${out}.tmdl`, script(bim));
for (const [file, body] of Object.entries(folder(bim))) {
  const path = join(out, 'definition', file);
  mkdirSync(dirname(path), { recursive: true });
  writeFileSync(path, body);
}
console.log(`${out}.tmdl and ${out}/definition/ (${Object.keys(folder(bim)).length} files)`);
