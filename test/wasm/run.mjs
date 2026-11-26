// Opens the wasm probe (test/wasm/index.html, served at <url>) in headless Chromium, prints what
// works and what does not, and writes the report. Exit code 1 when DuckDB-Wasm does not start, or
// when an item the extension must do there fails; the items marked `finding` only report.
//
//   node run.mjs <url> <report.json>
import { writeFileSync } from 'node:fs';
import { chromium } from 'playwright';

const [url, report] = process.argv.slice(2);
const browser = await chromium.launch();
const page = await browser.newPage();
// A OneLake token for the page (ONELAKE_TOKEN, storage.azure.com), never in a URL or a log.
if (process.env.ONELAKE_TOKEN) await page.addInitScript(t => { window.__onelakeToken = t; }, process.env.ONELAKE_TOKEN);
page.on('console', m => console.log(`[page] ${m.text()}`));
page.on('pageerror', e => console.log(`[page error] ${e.message}`));
await page.goto(url);
await page.waitForFunction(() => window.__done || window.__fatal, null, { timeout: 600000 });
const fatal = await page.evaluate(() => window.__fatal ?? null);
const results = await page.evaluate(() => window.__probe ?? []);
await browser.close();

writeFileSync(report, JSON.stringify({ fatal, results }, null, 1));
for (const r of results) {
  console.log(`${r.ok ? 'works  ' : 'fails  '}${r.finding ? '(finding) ' : ''}${r.name}`);
  console.log(`         ${(r.ok ? r.out : r.error) ?? ''}`);
}
if (fatal) console.log(`DuckDB-Wasm did not start: ${fatal}`);
const failed = results.filter(r => !r.ok && !r.finding);
process.exit(fatal || failed.length ? 1 : 0);
