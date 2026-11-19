// CR-32: a middle-stage node is as tall as the flow through it, and its
// ribbons fill it on both sides. Run: node --test ui/test/
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { sankey } from '../src/lib/sankey.js';

const near = (a, b) => Math.abs(a - b) < 1e-9;

test('ribbons fill a conserving middle node exactly', () => {
  const links = [
    { from: '0:a', to: '1:m', value: 30 },
    { from: '0:b', to: '1:m', value: 10 },
    { from: '1:m', to: '2:x', value: 25 },
    { from: '1:m', to: '2:y', value: 15 },
  ];
  const g = sankey(links, [['a', 'b'], ['m'], ['x', 'y']], { width: 300, height: 100, gap: 0 });
  const m = g.nodes.find((n) => n.key === '1:m');
  assert.equal(m.value, 40, 'flow through m is 40, not 80');
  const inH = g.ribbons.filter((r) => r.to === '1:m').reduce((s, r) => s + r.h1, 0);
  const outH = g.ribbons.filter((r) => r.from === '1:m').reduce((s, r) => s + r.h0, 0);
  assert.ok(near(inH, m.h), `incoming ribbons ${inH} fill m (${m.h})`);
  assert.ok(near(outH, m.h), `outgoing ribbons ${outH} fill m (${m.h})`);
});

test('first and last stages are unchanged', () => {
  const g = sankey([{ from: '0:a', to: '1:b', value: 5 }], [['a'], ['b']],
                   { width: 100, height: 50, gap: 0 });
  for (const n of g.nodes) assert.equal(n.value, 5);
  assert.ok(near(g.ribbons[0].h0, g.nodes[0].h) && near(g.ribbons[0].h1, g.nodes[1].h));
});
