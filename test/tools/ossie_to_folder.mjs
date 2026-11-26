// An Apache Ossie file split into a Fabric semantic view's folder (Context/Semantic Views/<view>/),
// for the tests of CREATE SEMANTIC VIEW from a folder of YAML files. Fabric merges every .yaml of
// the folder, at any depth and in any order; each needs version and name; the description is given
// once (or the same everywhere); .yml files are not read. So the folder is:
//   model.yaml                  version, name, description, and the model's extensions
//   datasets/dimensions.yaml    the first half of the datasets
//   datasets/facts.yaml         the rest: one list across two files
//   relationships.yaml
//   metrics/all.yaml
//   notes.yml                   not YAML Fabric reads, and not valid as Ossie either: it must be ignored
// The file is split on its top-level keys, as text, so nothing is rewritten.
//   node test/tools/ossie_to_folder.mjs <model.yaml> <out folder>
import { readFileSync, writeFileSync, mkdirSync } from 'node:fs';
import { dirname, join } from 'node:path';

const [src, out] = process.argv.slice(2);
const lines = readFileSync(src, 'utf8').replace(/^﻿/, '').split(/\r?\n/);

// The top-level sections, by key: their lines, the key's own line first.
const sections = new Map();
let key = null;
for (const line of lines) {
  const m = /^([A-Za-z_]+):/.exec(line);
  if (m) {
    key = m[1];
    sections.set(key, []);
  }
  if (key) sections.get(key).push(line);
}
const section = k => (sections.get(k) ?? []).join('\n');
const head = `${section('version')}\n${section('name')}\n`;

const write = (path, text) => {
  const file = join(out, path);
  mkdirSync(dirname(file), { recursive: true });
  writeFileSync(file, text.endsWith('\n') ? text : `${text}\n`);
};

// The datasets in two files: split at the list item nearest the middle.
const datasets = sections.get('datasets');
const items = datasets.map((l, i) => (/^  - /.test(l) ? i : -1)).filter(i => i >= 0);
const cut = items[Math.floor(items.length / 2)];
write('datasets/dimensions.yaml', head + datasets.slice(0, cut).join('\n'));
write('datasets/facts.yaml', head + 'datasets:\n' + datasets.slice(cut).join('\n'));

const lists = ['datasets', 'relationships', 'metrics'];
const rest = [...sections.keys()].filter(k => !['version', 'name', ...lists].includes(k));
write('model.yaml', head + rest.map(section).join('\n'));
write('relationships.yaml', head + section('relationships'));
write('metrics/all.yaml', head + section('metrics'));
write('notes.yml', 'this file: is not read\n  - at all\n');
console.log(`${src} -> ${out}: ${items.length} datasets in two files, ${rest.join(', ')} in model.yaml`);
