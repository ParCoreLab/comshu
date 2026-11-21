// The Timeline's opening view and axis labels.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { busiest, fmtTick } from '../src/lib/timeline.js';

test('opens on the burst, not on the setup that sets the extent', () => {
  // nccl_internode's shape: a trickle of pre-posted receives early, then 82%
  // of all events in two adjacent bins at the end of a 5 s run.
  const bins = new Array(400).fill(0);
  for (const i of [0, 8, 24, 72, 80, 104, 112, 120]) bins[i] = 1500;
  bins[396] = 42000; bins[397] = 30000;
  const w = { lo: 0, hi: 5.07e9 };
  const v = busiest(bins, w);
  assert.ok(v.lo > 4.9e9 && v.hi <= w.hi, `view ${v.lo}..${v.hi}`);
  assert.ok(v.hi - v.lo < 0.1e9, 'a narrow window, not the run');
});

test('a run that is busy throughout opens whole', () => {
  const w = { lo: 0, hi: 1e9 };
  assert.deepEqual(busiest(new Array(400).fill(10), w), w);
  assert.deepEqual(busiest(new Array(400).fill(0), w), w, 'no events: the run');
});

test('axis labels carry the precision their step needs', () => {
  // The old labels: 5.0400 s, 5.0400 s, 5.0401 s for a 50 µs step.
  const ticks = [5.04e9, 5.04005e9, 5.0401e9].map((t) => fmtTick(t, 50e3));
  assert.deepEqual(ticks, ['5.04000 s', '5.04005 s', '5.04010 s']);
  assert.equal(fmtTick(92.2e6, 0.2e6), '92.2 ms');
  assert.equal(fmtTick(25e9, 1e9), '25 s');
  assert.equal(new Set([1, 2, 3].map((k) => fmtTick(5e9 + k * 100, 100))).size, 3);
});
