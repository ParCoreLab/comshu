// node --test agent/test
//
// The deterministic half of the sidecar: what a query is allowed to be, and
// what a chart is allowed to be. Neither needs a model, and both are where a
// mistake is silent — a spec that quietly carries its own data still renders.

import { test } from 'node:test';
import assert from 'node:assert/strict';

import { stripComments, checkReadOnly, renderTable, Capture } from '../lib/query.mjs';
import { guardSpec, specFields } from '../lib/spec-guard.mjs';

test('comments are stripped, including the leading ones the server rejects', () => {
  assert.equal(stripComments('-- what\nSELECT 1'), 'SELECT 1');
  assert.equal(stripComments('/* a */ SELECT 1;'), 'SELECT 1');
  assert.equal(stripComments('SELECT 1 -- trailing\n'), 'SELECT 1');
});

test('only single reads pass', () => {
  assert.equal(checkReadOnly('SELECT 1'), null);
  assert.equal(checkReadOnly('WITH x AS (SELECT 1) SELECT * FROM x'), null);
  assert.equal(checkReadOnly('-- c\nSELECT 1'), null);
  assert.match(checkReadOnly('DELETE FROM comm_edges'), /only SELECT/);
  assert.match(checkReadOnly('SELECT 1; SELECT 2'), /one statement/);
  assert.match(checkReadOnly('   '), /empty/);
});

test('a result renders as a table the model can read', () => {
  const text = renderTable({
    columns: ['rank', 'bytes'],
    rows: [['0', '81920'], ['1', null]],
    total: 2,
    truncated: false,
  });
  assert.match(text, /rank {2}bytes/);
  assert.match(text, /NULL/);
  assert.match(text, /\(2 rows\)/);
});

test('a capped result says so', () => {
  const text = renderTable({ columns: ['a'], rows: [['1']], total: 900, truncated: true });
  assert.match(text, /1 of 900 rows/);
});

test('a plain spec is accepted and gets its data named', () => {
  const { ok, spec } = guardSpec({
    mark: 'bar',
    encoding: { x: { field: 'dst_rank', type: 'ordinal' }, y: { field: 'bytes', type: 'quantitative' } },
  });
  assert.ok(ok);
  assert.deepEqual(spec.data, { name: 'rows' });
  assert.match(spec.$schema, /vega-lite\/v6/);
});

test('a spec carrying its own data is refused, whichever way it carries it', () => {
  for (const data of [{ url: 'http://x/y.json' }, { values: [{ a: 1 }] }]) {
    const { ok, errors } = guardSpec({ mark: 'bar', data, encoding: {} });
    assert.equal(ok, false, JSON.stringify(data));
    assert.match(errors.join(' '), /may not carry or fetch its own data/);
  }
  const nested = guardSpec({ mark: 'bar', encoding: {}, datasets: { extra: [] } });
  assert.equal(nested.ok, false);
});

test('unknown marks, channels and keys are named in the error', () => {
  const bad = guardSpec({ mark: 'sankey', encoding: { wat: { field: 'x' } } });
  assert.equal(bad.ok, false);
  assert.match(bad.errors.join('\n'), /sankey/);
  assert.match(bad.errors.join('\n'), /encoding\.wat/);
});

test('interaction and escape hatches are refused', () => {
  for (const key of ['params', 'signal', 'config', 'projection']) {
    const { ok } = guardSpec({ mark: 'point', encoding: {}, [key]: {} });
    assert.equal(ok, false, key);
  }
});

test('layers are allowed, and validated the same way', () => {
  const good = guardSpec({ layer: [{ mark: 'line', encoding: {} }, { mark: 'point', encoding: {} }] });
  assert.ok(good.ok);
  const bad = guardSpec({ layer: [{ mark: 'nope', encoding: {} }] });
  assert.equal(bad.ok, false);
});

test('spec fields are collected so a chart can be checked against its query', () => {
  const fields = specFields({
    mark: 'bar',
    encoding: { x: { field: 'a', type: 'ordinal' }, y: { field: 'b', type: 'quantitative' }, tooltip: [{ field: 'c' }] },
  });
  assert.deepEqual(fields.sort(), ['a', 'b', 'c']);
});

// The rest needs a capture. Skipped rather than mocked: a fake /query that
// agrees with our expectations proves nothing about the one thing this layer
// does, which is talk to the real server.
const UI = process.env.PCOMM_UI_URL ?? 'http://127.0.0.1:8777';
const live = await fetch(`${UI}/query`, { method: 'POST', body: 'SELECT 1', signal: AbortSignal.timeout(2000) })
  .then((r) => r.ok)
  .catch(() => false);

test('the row cap bites and reports the true total', { skip: live ? false : `no pcomm ui at ${UI}` }, async () => {
  const capture = new Capture(UI);
  const r = await capture.modelQuery('SELECT * FROM timeline_span', { limit: 5 });
  assert.equal(r.rows.length, 5);
  assert.equal(r.truncated, true);
  assert.ok(r.total > 5);
});

test('a commented query still runs', { skip: live ? false : `no pcomm ui at ${UI}` }, async () => {
  const capture = new Capture(UI);
  const r = await capture.modelQuery('-- the heaviest pairs\nSELECT src_rank, bytes FROM comm_edges');
  assert.deepEqual(r.columns, ['src_rank', 'bytes']);
});

test('a write is refused before it reaches the server', { skip: live ? false : `no pcomm ui at ${UI}` }, async () => {
  const capture = new Capture(UI);
  await assert.rejects(() => capture.modelQuery('CREATE TABLE t AS SELECT 1'), /only SELECT/);
});
