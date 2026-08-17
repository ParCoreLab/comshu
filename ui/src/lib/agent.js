// THE agent path. The chat pane reaches the model through here and nowhere
// else — the same rule as `query.js`, for a related reason.
//
// `query.js` exists so that no panel invents its own server-side aggregation.
// This file exists so that the agent never becomes one. Nothing here returns
// capture data: the model's answers arrive as prose plus `{ sql, spec }`, and
// the rows behind a chart are fetched by the panel through `query()` like every
// other panel's rows. If a future version of this file starts returning rows,
// the derived layer has forked and the DuckDB-WASM build has quietly died.
//
// The bridge is a separate process on a second loopback port (`pcomm ui` serves
// the page; `agent/bridge.mjs` runs the model). It is optional: when it is not
// running, `health()` fails and the pane says so.

const DEFAULT_PORT = 8778;

/** Where the bridge is. `?agent=<url>` overrides, for a forwarded port. */
export const agentUrl = () => {
  const q = new URLSearchParams(location.search).get('agent');
  if (q) return q.replace(/\/+$/, '');
  return `${location.protocol}//${location.hostname}:${DEFAULT_PORT}`;
};

// 127.0.0.1 and localhost are the same host; a mismatch on that alone is noise.
const sameOrigin = (a, b) => {
  try {
    const [x, y] = [new URL(a), new URL(b)];
    const host = (u) => (u.hostname === 'localhost' ? '127.0.0.1' : u.hostname);
    return host(x) === host(y) && x.port === y.port;
  } catch {
    return false;
  }
};

export async function health({ timeoutMs = 3000 } = {}) {
  const res = await fetch(`${agentUrl()}/health`, { signal: AbortSignal.timeout(timeoutMs) });
  if (!res.ok) throw new Error(`the agent bridge returned ${res.status}`);
  const h = await res.json();

  // The bridge names the capture it queries, and the page must check it.
  // Two `pcomm ui` servers on one machine share this default port, so without
  // this check the pane on :8779 will confidently answer questions about the
  // capture on :8777 — every number right, every number about the wrong run.
  h.mismatch = h.ui && !sameOrigin(h.ui, location.origin) ? h.ui : null;
  return h;
}

/**
 * Ask a question. Yields events as they stream:
 *
 *   { kind: 'text',  delta }
 *   { kind: 'tool',  name, args }        a tool started (sql args carry the query)
 *   { kind: 'tool_end', name, ok, summary }
 *   { kind: 'chart', chart: { title, sql, spec, rows } }
 *   { kind: 'error', message }
 *   { kind: 'done',  text, charts, queries, error }
 *
 * SSE over POST, so `EventSource` is not usable — it can only GET, and the
 * question does not belong in a URL.
 */
export async function* ask(message, { signal } = {}) {
  const res = await fetch(`${agentUrl()}/ask`, {
    method: 'POST',
    headers: { 'content-type': 'application/json' },
    body: JSON.stringify({ message }),
    signal,
  });
  if (!res.ok) {
    const body = await res.json().catch(() => ({}));
    throw new Error(body.error ?? `the agent bridge returned ${res.status}`);
  }

  const reader = res.body.getReader();
  const decoder = new TextDecoder();
  let buf = '';
  while (true) {
    const { value, done } = await reader.read();
    if (done) break;
    buf += decoder.decode(value, { stream: true });
    // SSE frames are separated by a blank line; each frame here is one
    // `data:` line holding one JSON object.
    let i;
    while ((i = buf.indexOf('\n\n')) !== -1) {
      const frame = buf.slice(0, i);
      buf = buf.slice(i + 2);
      const line = frame.split('\n').find((l) => l.startsWith('data:'));
      if (!line) continue;
      try {
        yield JSON.parse(line.slice(5).trim());
      } catch {
        /* a frame we cannot parse is not worth killing the stream over */
      }
    }
  }
}

export async function abort() {
  await fetch(`${agentUrl()}/abort`, { method: 'POST' }).catch(() => {});
}

/** Drop the conversation and start a new one against the same capture. */
export async function reset() {
  await fetch(`${agentUrl()}/reset`, { method: 'POST' }).catch(() => {});
}
