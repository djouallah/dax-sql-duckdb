// The SQL dax-sql gives for each query, in Node: js/glue.js with dax-sql's files as the submodule
// has them, the same code the extension embeds. drift.py compares it with what the extension's
// dax_sql_rewrite() gives, so the extension cannot drift from dax-sql (EXTENSION.md, The compiler
// does not drift from dax-sql).
//
//   node drift.mjs <out.json> [--facts <facts.json>] <view>=<model file> ... -- <queries.sql> ...
//
// facts.json ({ view: { pins, orphans } }) is each view's frame as the extension found it on the
// data (dax_sql_facts); the glue in Node is given the same, so both write the same SQL.
//
// A queries file holds one query a paragraph; comment lines are dropped. Needs dax-sql resolvable
// as a package: node_modules/dax-sql/{src,formats} linked to the submodule's.
import { readFileSync, writeFileSync } from 'node:fs';

await import('../../js/glue.js');
const api = globalThis.dax_sql;

const args = process.argv.slice(2);
const out = args.shift();
const facts = args[0] === '--facts' ? (args.shift(), JSON.parse(readFileSync(args.shift(), 'utf8'))) : {};
const split = args.indexOf('--');
const views = args.slice(0, split);
const files = args.slice(split + 1);

for (const v of views) {
  const [name, path] = v.split('=');
  api.define('memory', 'main', name, path, readFileSync(path, 'utf8'), 'create');
  if (facts[name]) api.set_facts(name, JSON.stringify(facts[name]));
}

const queries = files.flatMap(f => readFileSync(f, 'utf8')
  .split(/\r?\n\s*\r?\n/)
  .map(p => p.split(/\r?\n/).filter(l => !/^\s*--/.test(l)).join('\n').trim())
  .filter(Boolean));

const results = queries.map(query => ({ query, sql: api.rewrite(query, 'dry') || query }));
writeFileSync(out, JSON.stringify(results, null, 1));
console.log(`${results.length} queries rewritten by dax-sql in Node`);
