#!/usr/bin/env node
// The bridge: a chat pane on one side, one `pi --mode rpc` child on the other.
//
//   node agent/bridge.mjs --ui http://127.0.0.1:8777 [--port 8778]
//
// It deliberately does NOT read the capture itself beyond building the system
// prompt. Answers are assembled by the model through the `sql` tool, and charts
// leave here as { sql, spec } with no rows attached — the page runs the query
// through its own `query()`, exactly as every curated panel does. That is what
// keeps one derived layer instead of two.
//
// Loopback by default, like `pcomm ui`: this is one person's capture, on one
// person's machine, and it has no authentication because it must never be
// reachable by anything that would need to authenticate.
//
// `--bind` off loopback is allowed but must be said twice — the address AND
// `--allow-remote` — because the failure mode is silent and total: anyone who
// can route to the address gets an unauthenticated agent with a live query
// connection to the capture. The second flag exists so that no one reaches this
// state by copying a command they did not read. `pcomm ui --bind` takes the
// same position and hardens DuckDB when it does.

import { createServer } from 'node:http';
import { Capture } from './lib/query.mjs';
import { buildSystemPrompt } from './lib/prompt.mjs';
import { PiSession, DEFAULT_MODEL } from './lib/session.mjs';

const opts = {
  ui: process.env.PCOMM_UI_URL ?? 'http://127.0.0.1:8777',
  port: 8778,
  bind: '127.0.0.1',
  allowRemote: false,
  provider: undefined,
  model: undefined,
};
const argv = process.argv.slice(2);
for (let i = 0; i < argv.length; i++) {
  const a = argv[i];
  if (a === '--ui') opts.ui = argv[++i];
  else if (a === '--port') opts.port = Number(argv[++i]);
  else if (a === '--bind') opts.bind = argv[++i];
  else if (a === '--allow-remote') opts.allowRemote = true;
  else if (a === '--model') opts.model = argv[++i];
  else if (a === '--provider') opts.provider = argv[++i];
  else if (a === '-h' || a === '--help') {
    process.stdout.write(
      'usage: node agent/bridge.mjs [--ui <url>] [--port N] [--bind ADDR] [--allow-remote]\n' +
        '                            [--model M] [--provider P]\n\n' +
        '  --bind off loopback also needs --allow-remote, and --ui must then name\n' +
        '  the address the BROWSER uses, not 127.0.0.1 — the page checks that the\n' +
        '  bridge is reading the capture the page is showing.\n',
    );
    process.exit(0);
  }
}

const isLoopback = (a) => a === '127.0.0.1' || a === 'localhost' || a === '::1';

if (!isLoopback(opts.bind) && !opts.allowRemote) {
  process.stderr.write(
    `refusing to bind ${opts.bind}: the bridge runs an agent with a live query connection to a ` +
      'capture and has no authentication.\n' +
      '  Use an SSH forward, or pass --allow-remote if the address is on a private\n' +
      '  network you trust (a tailnet, say) and you accept that anyone who can route\n' +
      '  to it can read the capture and drive the model.\n',
  );
  process.exit(2);
}

// The page checks that the bridge names the same origin it was loaded from, so
// a bridge bound off loopback while still advertising 127.0.0.1 reports a FALSE
// "that agent is reading a different capture". Catch it here, where the message
// can name the fix, rather than in the pane where it looks like a real mismatch.
if (!isLoopback(opts.bind) && isLoopback(new URL(opts.ui).hostname)) {
  process.stderr.write(
    `--bind ${opts.bind} with --ui ${opts.ui}: the page will be served from ${opts.bind} and will\n` +
      `  reject this bridge as reading a different capture. Pass --ui http://${opts.bind}:` +
      `${new URL(opts.ui).port || '80'} instead.\n`,
  );
  process.exit(2);
}

const capture = new Capture(opts.ui);

// The page is served by `pcomm ui` on a different port, so this is a genuine
// cross-origin fetch. Allow exactly the origins that can legitimately hold the
// page: the UI itself, and vite in dev.
//
// The UI's own host is taken from --ui rather than assumed to be loopback. It
// used to be hardcoded to 127.0.0.1/localhost, which meant that even once the
// bind refusal above was satisfied, a page served over a tailnet address was a
// disallowed origin and every request from it failed CORS — the bridge running,
// reachable, and mute. Two loopback spellings stay in the set regardless,
// because a bridge bound to 0.0.0.0 is still reachable at 127.0.0.1.
const uiUrl = new URL(opts.ui);
const uiPort = uiUrl.port || '80';
const ALLOWED = new Set([
  ...[uiPort, '5180'].flatMap((p) => [`http://127.0.0.1:${p}`, `http://localhost:${p}`]),
  `${uiUrl.protocol}//${uiUrl.host}`,
]);

function cors(req, res) {
  const origin = req.headers.origin;
  if (origin && ALLOWED.has(origin)) {
    res.setHeader('Access-Control-Allow-Origin', origin);
    res.setHeader('Vary', 'Origin');
  }
  res.setHeader('Access-Control-Allow-Headers', 'content-type');
  res.setHeader('Access-Control-Allow-Methods', 'GET, POST, OPTIONS');
}

let session = null;
let systemPrompt = null;
let busy = false;
let startupError = null;

async function ensureSession() {
  if (session) return session;
  // Rebuilt per session, not cached from startup. A long-lived bridge outlives
  // the `pcomm ui` it points at, and when that server is restarted on a
  // different capture the port stays the same while everything behind it
  // changes. Caching the prompt meant the model kept being told "2 ranks" about
  // a 64-rank capture — its queries still ran against the real data, so the
  // answers were right and the description it reasoned from was false, which is
  // the worst shape for a bug like this. It costs ~1 s.
  systemPrompt = await buildSystemPrompt(capture);
  session = new PiSession({
    uiUrl: opts.ui,
    systemPrompt,
    provider: opts.provider,
    model: opts.model,
  }).start();
  session.exited.then((code) => {
    if (session) process.stderr.write(`agent process exited (${code}); the next question will start a new one\n`);
    session = null;
  });
  return session;
}

const json = (res, code, body) => {
  res.writeHead(code, { 'content-type': 'application/json' });
  res.end(JSON.stringify(body));
};

async function readBody(req) {
  const chunks = [];
  for await (const c of req) chunks.push(c);
  return Buffer.concat(chunks).toString();
}

const server = createServer(async (req, res) => {
  cors(req, res);
  if (req.method === 'OPTIONS') {
    res.writeHead(204);
    res.end();
    return;
  }

  const url = new URL(req.url, `http://${req.headers.host}`);

  if (url.pathname === '/health') {
    let capture_ok = true;
    let detail = null;
    try {
      await capture.scalar('SELECT 1');
    } catch (e) {
      capture_ok = false;
      detail = e.message;
    }
    json(res, 200, {
      ok: capture_ok && !startupError,
      ui: opts.ui,
      capture_ok,
      detail: detail ?? startupError,
      model: opts.model ?? DEFAULT_MODEL,
      provider: opts.provider ?? null,
      busy,
      prompt_ready: systemPrompt !== null,
    });
    return;
  }

  if (url.pathname === '/abort' && req.method === 'POST') {
    session?.abort();
    json(res, 200, { ok: true });
    return;
  }

  if (url.pathname === '/reset' && req.method === 'POST') {
    session?.stop();
    session = null;
    busy = false;
    json(res, 200, { ok: true });
    return;
  }

  if (url.pathname === '/ask' && req.method === 'POST') {
    if (busy) {
      json(res, 409, { error: 'the agent is still answering the previous question' });
      return;
    }
    let message;
    try {
      message = JSON.parse(await readBody(req)).message;
    } catch {
      json(res, 400, { error: 'expected a JSON body with a `message`' });
      return;
    }
    if (!message || typeof message !== 'string') {
      json(res, 400, { error: 'expected a JSON body with a `message`' });
      return;
    }

    res.writeHead(200, {
      'content-type': 'text/event-stream',
      'cache-control': 'no-cache',
      connection: 'keep-alive',
      'x-accel-buffering': 'no',
    });
    const send = (obj) => res.write(`data: ${JSON.stringify(obj)}\n\n`);

    busy = true;
    let closed = false;
    req.on('close', () => {
      closed = true;
      session?.abort();
    });

    try {
      const s = await ensureSession();
      const result = await s.ask(message, (ev) => {
        if (!closed) send(ev);
      });
      if (!closed) {
        send({ kind: 'done', text: result.text, charts: result.charts, queries: result.queries, error: result.error });
        res.end();
      }
    } catch (e) {
      if (!closed) {
        send({ kind: 'error', message: e.message });
        send({ kind: 'done', text: '', charts: [], queries: [], error: e.message });
        res.end();
      }
    } finally {
      busy = false;
    }
    return;
  }

  json(res, 404, { error: 'not found' });
});

// Fail at startup, not at the first question: a chat pane that spins for a
// minute and then says "cannot reach the capture" is worse than one that was
// never offered.
try {
  systemPrompt = await buildSystemPrompt(capture);
} catch (e) {
  startupError = `cannot read the capture at ${opts.ui}: ${e.message}`;
  process.stderr.write(`${startupError}\nis \`pcomm ui <capture>\` running?\n`);
}

// Loudly, because the failure it prevents is subtle: a bridge left running from
// an earlier session keeps the port, the new one dies quietly, and the page
// happily talks to a stale agent that may be pointed at another capture.
server.on('error', (e) => {
  if (e.code === 'EADDRINUSE') {
    process.stderr.write(
      `port ${opts.port} is already in use — another agent bridge is probably still running.\n` +
        `check it is the one you want (curl http://127.0.0.1:${opts.port}/health), or stop it.\n`,
    );
  } else {
    process.stderr.write(`${e.message}\n`);
  }
  process.exit(2);
});

server.listen(opts.port, opts.bind, () => {
  process.stdout.write(
    `pcomm agent bridge: http://${opts.bind}:${opts.port}  ->  capture ${opts.ui}\n` +
      `  model: ${opts.model ?? DEFAULT_MODEL} (provider: ${opts.provider ?? "pi's default"})\n` +
      `  prompt: ${systemPrompt ? `${systemPrompt.length} chars` : 'NOT BUILT — capture unreachable'}\n`,
  );
});
