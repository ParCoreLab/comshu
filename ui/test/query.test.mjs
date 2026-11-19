// CR-33: /query returns every value as a string; bool() is how a panel reads a
// BOOLEAN column, because "false" is truthy.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { bool, num } from '../src/lib/query.js';

test('bool reads DuckDB booleans as served', () => {
  assert.equal(bool('true'), true);
  assert.equal(bool('false'), false);
  assert.equal(bool(null), false, 'NULL is unknown, not true');
  assert.equal(bool(true), true);
});

test('num keeps NULL distinct from 0', () => {
  assert.equal(num(null), null);
  assert.equal(num('0'), 0);
});
