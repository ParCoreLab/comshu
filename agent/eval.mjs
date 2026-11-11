#!/usr/bin/env node
// Does the agent actually answer correctly? Measure it, don't guess.
//
//   node agent/eval.mjs --ui http://127.0.0.1:8777
//   node agent/eval.mjs --only heaviest-pair,clean --verbose
//
// Property assertions, not golden SQL. There are many correct queries for "how
// many bytes in total", and pinning one would fail the model for being right in
// a different way — which is how an eval starts measuring conformity instead of
// correctness. What is checked instead:
//
//   - it ran a query at all, rather than answering from the prompt;
//   - the query touched a table that could contain the answer;
//   - the number it reported matches one WE compute here, independently;
//   - it drew a chart when asked to, and did not when it had nothing to draw;
//   - when the capture cannot answer, it says so instead of inventing a number.
//
// The reference values are computed against the live capture, so this suite is
// not tied to one fixture: point it at any capture and the expectations follow.

import { readFile } from 'node:fs/promises';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

import { Capture } from './lib/query.mjs';
import { buildSystemPrompt } from './lib/prompt.mjs';
import { PiSession } from './lib/session.mjs';
import { tableCounts } from './lib/preamble.mjs';

const here = dirname(fileURLToPath(import.meta.url));

const opts = { ui: process.env.COMSHU_UI_URL ?? 'http://127.0.0.1:8777', only: null, verbose: false };
const argv = process.argv.slice(2);
for (let i = 0; i < argv.length; i++) {
  const a = argv[i];
  if (a === '--ui') opts.ui = argv[++i];
  else if (a === '--only') opts.only = argv[++i].split(',');
  else if (a === '--model') opts.model = argv[++i];
  else if (a === '--provider') opts.provider = argv[++i];
  else if (a === '--verbose' || a === '-v') opts.verbose = true;
  else if (a === '-h' || a === '--help') {
    process.stdout.write('usage: node agent/eval.mjs [--ui <url>] [--only id,id] [--model M] [--verbose]\n');
    process.exit(0);
  }
}

const capture = new Capture(opts.ui);
const counts = Object.fromEntries((await tableCounts(capture)).map((c) => [c.table, c.rows]));
const systemPrompt = await buildSystemPrompt(capture);

const cases = (await readFile(join(here, 'eval', 'questions.jsonl'), 'utf8'))
  .split('\n')
  .filter((l) => l.trim())
  .map((l) => JSON.parse(l))
  .filter((c) => !opts.only || opts.only.includes(c.id));

/** Digits only, so "81,920" / "81 920" / "81920" all compare equal. */
const digits = (s) => String(s).replace(/[\s,_]/g, '');

/**
 * Did the answer state this number?
 *
 * Scans every number in the text rather than substring-matching the expected
 * one: '2' appears inside '2017', and a check that accepts that is a check that
 * always passes.
 */
function statesValue(text, expected, tolerance = 0) {
  const want = Number(expected);
  if (!Number.isFinite(want)) return false;
  // "No work requests completed with an error status" is a correct answer to a
  // count of zero, and demanding the numeral would score prose as a failure.
  if (want === 0 && /\b(no|none|zero|nothing|never)\b/i.test(text)) return true;
  const found = digits(text).match(/\d+(?:\.\d+)?/g) ?? [];
  return found.some((f) => {
    const got = Number(f);
    if (!Number.isFinite(got)) return false;
    if (tolerance === 0) return got === want;
    return Math.abs(got - want) <= Math.abs(want) * tolerance;
  });
}

const results = [];

for (const c of cases) {
  const missing = (c.requires ?? []).filter((t) => !(counts[t] > 0));
  const notEmpty = (c.requires_empty ?? []).filter((t) => counts[t] > 0);
  if (missing.length || notEmpty.length) {
    results.push({ id: c.id, skip: `capture has no ${[...missing, ...notEmpty].join(', ')}` });
    continue;
  }

  const session = new PiSession({
    uiUrl: opts.ui,
    systemPrompt,
    provider: opts.provider,
    model: opts.model,
  }).start();

  const t0 = Date.now();
  let answer;
  try {
    answer = await session.ask(c.q);
  } catch (e) {
    results.push({ id: c.id, checks: [['answered', false, e.message]], ms: Date.now() - t0 });
    session.stop();
    continue;
  }
  session.stop();
  const ms = Date.now() - t0;

  const text = answer.text ?? '';
  // A chart carries its own query, and that query is as much "a query the agent
  // ran" as an `sql` call is — the chart tool executes it to check the spec's
  // columns. Counting only `sql` calls failed a model that went straight to a
  // correct chart, which is a style difference, not an error.
  const sql = [...answer.queries, ...answer.charts.map((c) => c.sql)].join('\n').toLowerCase();
  const checks = [];

  checks.push(['answered', !answer.error && text.trim().length > 0, answer.error ?? 'empty answer']);

  if (c.expect.tables) {
    const hit = c.expect.tables.find((t) => sql.includes(t.toLowerCase()));
    checks.push(['queried', Boolean(hit), `no query touched ${c.expect.tables.join(' / ')}`]);
  }

  if (c.expect.value) {
    const want = await capture.scalar(c.expect.value.sql);
    const ok = statesValue(text, want, c.expect.value.tolerance ?? 0);
    checks.push(['value', ok, `expected ${want}, not found in the answer`]);
  }

  if (c.expect.says) {
    // A string the harness computes for itself — the dominant stream, a label —
    // rather than one written into the question, so the case follows the
    // capture instead of pinning a fixture's answer.
    const want = String(await capture.scalar(c.expect.says.sql));
    // Compare on letters only, so the prose name matches the column name:
    // "GPU kernels" and `gpu_kernel` are the same answer, and an eval that
    // scores the difference is measuring transcription, not correctness.
    const flat = (s) => s.toLowerCase().replace(/[^a-z0-9]/g, '');
    checks.push(['says', flat(text).includes(flat(want)), `expected the answer to name "${want}"`]);
  }

  for (const re of c.expect.mentions_re ?? []) {
    checks.push([`/${re}/`, new RegExp(re, 'i').test(text), `nothing matching /${re}/i in the answer`]);
  }

  if (c.expect.no_impossible_pct) {
    // The failure this exists for: summing overlapping spans and reporting the
    // total as a share of runtime. Concurrency makes those sums exceed the wall
    // clock many times over, so any percentage above 100 is proof of it.
    const over = (text.match(/(\d+(?:\.\d+)?)\s*%/g) ?? []).filter((m) => parseFloat(m) > 100);
    checks.push(['no impossible %', over.length === 0, `claimed ${over.join(', ')} of the runtime`]);
  }

  if (c.expect.chart) checks.push(['chart', answer.charts.length > 0, 'no chart was drawn']);
  if (c.expect.no_chart) checks.push(['no chart', answer.charts.length === 0, 'drew a chart it was not asked for']);

  if (c.expect.admits_missing) {
    const lower = text.toLowerCase();
    const ok = c.expect.admits_missing.some((w) => lower.includes(w.toLowerCase()));
    checks.push(['admits the gap', ok, 'did not say the capture lacks this data']);
    // The failure this case exists for: a confident number from an empty table.
    checks.push(['invents nothing', !/\b\d{3,}\b/.test(digits(text)) || /no |not |empty|lacks/i.test(text), 'reported a figure the capture cannot support']);
  }

  results.push({ id: c.id, checks, ms, text, queries: answer.queries, charts: answer.charts.length });
}

let pass = 0;
let fail = 0;
let skipped = 0;
for (const r of results) {
  if (r.skip) {
    skipped++;
    process.stdout.write(`skip  ${r.id.padEnd(18)} ${r.skip}\n`);
    continue;
  }
  const bad = r.checks.filter(([, ok]) => !ok);
  if (bad.length) fail++;
  else pass++;
  process.stdout.write(
    `${bad.length ? 'FAIL' : 'ok  '}  ${r.id.padEnd(18)} ${String(Math.round(r.ms / 1000)).padStart(3)}s  ` +
      `${r.checks.map(([n, ok]) => `${ok ? '+' : '-'}${n}`).join(' ')}\n`,
  );
  for (const [, , why] of bad) process.stdout.write(`        ${why}\n`);
  if (opts.verbose) {
    for (const q of r.queries) process.stdout.write(`        sql: ${q.replace(/\s+/g, ' ').slice(0, 120)}\n`);
    process.stdout.write(`        ${r.text.replace(/\n+/g, ' ').slice(0, 300)}\n`);
  }
}

process.stdout.write(`\n${pass} passed, ${fail} failed, ${skipped} skipped\n`);
process.exit(fail ? 1 : 0);
