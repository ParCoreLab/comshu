// CR-34: a PiSession whose child cannot start, or dies mid-question, settles
// its ask() at once — it used to crash the bridge (no 'error' listener) or wait
// out a 10-minute timer.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { mkdtempSync, writeFileSync, chmodSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { PiSession } from '../lib/session.mjs';

const withPath = async (dir, fn) => {
  const saved = process.env.PATH;
  process.env.PATH = dir;
  try {
    return await fn();
  } finally {
    process.env.PATH = saved;
  }
};
const session = () => new PiSession({ uiUrl: 'http://127.0.0.1:1', systemPrompt: 'x' });

test('pi not on PATH: ask() rejects, the process survives', { timeout: 5000 }, async () => {
  const empty = mkdtempSync(join(tmpdir(), 'nopi-'));
  await withPath(empty, async () => {
    const s = session().start();
    await assert.rejects(s.ask('hello'), /could not run|not running/);
  });
});

test('pi dies mid-question: ask() rejects promptly', { timeout: 5000 }, async () => {
  const dir = mkdtempSync(join(tmpdir(), 'fakepi-'));
  // Reads the prompt line, then exits without answering.
  writeFileSync(join(dir, 'pi'), '#!/bin/sh\nread line\nexit 3\n');
  chmodSync(join(dir, 'pi'), 0o755);
  await withPath(`${dir}:/usr/bin:/bin`, async () => {
    const s = session().start();
    await assert.rejects(s.ask('hello'), /exited \(code 3\)/);
  });
});
