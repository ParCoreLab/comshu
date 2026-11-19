// CR-65(3): counting aggregates leave their field alone; arithmetic ones coerce.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { coerce } from '../src/lib/chart.js';

test('distinct/count do not coerce strings to NaN; sum does coerce', () => {
  const rows = [{ transport: 'ib', bytes: '10' }];
  const spec = {
    mark: 'bar',
    encoding: {
      x: { field: 'transport', aggregate: 'distinct' },
      y: { field: 'bytes', aggregate: 'sum' },
    },
  };
  const [r] = coerce(spec, rows);
  assert.equal(r.transport, 'ib');
  assert.equal(r.bytes, 10);
});
