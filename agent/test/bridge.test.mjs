// The bridge end to end, against a fake `comshu ui` and a fake `pi`: the races
// the CR audit found in bridge.mjs (CR-34, CR-35, CR-71), which nothing tested.
//
// The fake ui answers every query with an empty result, which is enough for the
// system prompt to build. The fake pi answers each prompt line, after a short
// pause, with one text delta and agent_settled — the pause is what gives a
// second /ask the window to race the first.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { createServer, request } from 'node:http';
import { mkdtempSync, writeFileSync, chmodSync, readFileSync, existsSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const BRIDGE = join(dirname(fileURLToPath(import.meta.url)), '..', 'bridge.mjs');

function fakePi() {
  const dir = mkdtempSync(join(tmpdir(), 'fakepi-'));
  writeFileSync(
    join(dir, 'pi'),
    `#!/bin/sh
echo $$ >> "$(dirname "$0")/pids"
while read line; do
  sleep 0.5
  echo '{"type":"message_update","assistantMessageEvent":{"type":"text_delta","delta":"hi"}}'
  echo '{"type":"agent_settled"}'
done
`,
  );
  chmodSync(join(dir, 'pi'), 0o755);
  return dir;
}

// `slow.ms` delays every answer, set after the bridge is up so a session's
// prompt build can be held open.
function fakeUi(port = 0, slow = { ms: 0 }) {
  const srv = createServer((req, res) => {
    req.resume();
    req.on('end', () =>
      setTimeout(() => {
        res.writeHead(200, { 'content-type': 'application/json' });
        res.end(JSON.stringify({ columns: [], rows: [] }));
      }, slow.ms),
    );
  });
  return new Promise((resolve) => srv.listen(port, '127.0.0.1', () => resolve(srv)));
}

const freePort = async () => {
  const srv = await fakeUi();
  const { port } = srv.address();
  await new Promise((r) => srv.close(r));
  return port;
};

async function startBridge(uiPort) {
  const port = await freePort();
  const piDir = fakePi();
  const child = spawn(process.execPath, [BRIDGE, '--ui', `http://127.0.0.1:${uiPort}`, '--port', String(port)], {
    env: { ...process.env, PATH: `${piDir}:/usr/bin:/bin` },
    stdio: ['ignore', 'ignore', 'pipe'],
  });
  let stderr = '';
  child.stderr.on('data', (d) => (stderr += d));
  const base = `http://127.0.0.1:${port}`;
  for (let i = 0; i < 100; i++) {
    try {
      await fetch(`${base}/health`);
      return { base, child, piDir, stderr: () => stderr };
    } catch {
      await new Promise((r) => setTimeout(r, 50));
    }
  }
  child.kill();
  throw new Error(`bridge did not come up: ${stderr}`);
}

const ask = (base, message) =>
  fetch(`${base}/ask`, {
    method: 'POST',
    headers: { 'content-type': 'application/json' },
    body: JSON.stringify({ message }),
  }).then(async (r) => ({ status: r.status, body: await r.text() }));

test('two concurrent asks: one answers, the other is refused', { timeout: 20000 }, async () => {
  const ui = await fakeUi();
  const b = await startBridge(ui.address().port);
  try {
    // The first ask's body is held open, so its handler sits in readBody()
    // while the second arrives whole. busy used to be claimed only after the
    // body was read, so both got in.
    const body = JSON.stringify({ message: 'one' });
    const first = new Promise((resolve, reject) => {
      const req = request(`${b.base}/ask`, { method: 'POST', headers: { 'content-type': 'application/json' } }, (res) => {
        res.resume();
        res.on('end', () => resolve({ status: res.statusCode }));
      });
      req.on('error', reject);
      req.write(body.slice(0, 5));
      setTimeout(() => req.end(body.slice(5)), 300);
    });
    await new Promise((r) => setTimeout(r, 100));
    const [x, y] = await Promise.all([first, ask(b.base, 'two')]);
    assert.deepEqual([x.status, y.status].sort(), [200, 409], b.stderr());
  } finally {
    b.child.kill();
    ui.close();
  }
});

test('a capture that comes up after the bridge: /health recovers after an ask', { timeout: 20000 }, async () => {
  const uiPort = await freePort();
  const b = await startBridge(uiPort);  // nothing listening yet: startup fails
  let ui;
  try {
    assert.equal((await (await fetch(`${b.base}/health`)).json()).ok, false);
    ui = await fakeUi(uiPort);
    const r = await ask(b.base, 'hello');
    assert.equal(r.status, 200);
    // The ask built the prompt; /health then skipped its own rebuild and kept
    // reporting the startup error forever.
    const h = await (await fetch(`${b.base}/health`)).json();
    assert.equal(h.ok, true, JSON.stringify(h));
  } finally {
    b.child.kill();
    ui?.close();
  }
});

// Live fake-pi processes the bridge started.
const livePis = (dir) => {
  const f = join(dir, 'pids');
  if (!existsSync(f)) return [];
  return readFileSync(f, 'utf8').split('\n').filter(Boolean).map(Number).filter((pid) => {
    try {
      process.kill(pid, 0);
      return true;
    } catch {
      return false;
    }
  });
};

test('a /reset while a session is starting leaves no orphaned agent', { timeout: 30000 }, async () => {
  const slow = { ms: 0 };
  const ui = await fakeUi(0, slow);
  const b = await startBridge(ui.address().port);
  try {
    slow.ms = 400;  // the next prompt build takes a few round trips of this
    const first = ask(b.base, 'one');  // starts a session, held in the prompt build
    await new Promise((r) => setTimeout(r, 150));
    await fetch(`${b.base}/reset`, { method: 'POST' });
    const second = await ask(b.base, 'two');  // starts its own
    await first;
    assert.equal(second.status, 200);
    await new Promise((r) => setTimeout(r, 300));
    // One session is current; the one the reset overtook must have been
    // stopped, not left running under an overwritten slot.
    assert.equal(livePis(b.piDir).length, 1, `live agents: ${livePis(b.piDir)}`);
  } finally {
    b.child.kill();
    for (const pid of livePis(b.piDir)) process.kill(pid);
    ui.close();
  }
});
