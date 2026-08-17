<script>
  // The chat pane: ask in English, get prose, the queries that produced it, and
  // sometimes a chart.
  //
  // It is a side pane rather than a tenth panel because it is not a view of the
  // capture — it is a way of getting to the views. It stays open while you read
  // the matrix, and it renders no aggregate of its own: every number it shows
  // was computed by a query you can read and re-run.
  //
  // Nothing here trusts the model. Its SQL is visible, its charts carry their
  // query, and the "open in Ask" button drops either into the panel where you
  // can edit and re-run it yourself.
  import { ask, abort, health, reset, agentUrl } from '../lib/agent.js';
  import Chart from '../lib/Chart.svelte';

  let { open = false, onclose } = $props();

  let status = $state({ state: 'checking' }); // checking | ready | down
  let turns = $state([]); // { role, text, queries: [], charts: [], error }
  let input = $state('');
  let busy = $state(false);
  let controller = null;
  let log = $state(null);

  $effect(() => {
    if (!open) return;
    health()
      .then((h) => {
        if (h.mismatch) status = { state: 'mismatch', ...h };
        else status = h.ok ? { state: 'ready', ...h } : { state: 'down', detail: h.detail };
      })
      .catch((e) => (status = { state: 'down', detail: e.message }));
  });

  // Follow the tail while an answer streams, but never yank the view back if
  // the reader has scrolled up to re-read a query.
  function scroll() {
    if (!log) return;
    const atBottom = log.scrollHeight - log.scrollTop - log.clientHeight < 80;
    if (atBottom) queueMicrotask(() => (log.scrollTop = log.scrollHeight));
  }

  // Handing a query to the Ask panel.
  //
  // Two mechanisms, because there are two cases and the first version only had
  // one. sessionStorage covers "Ask is not mounted yet" — it reads the key when
  // it starts up. The event covers "Ask is already the open panel", where
  // nothing re-runs and the second query you clicked silently did nothing.
  function toAsk(sql) {
    sessionStorage.setItem('pcomm.ask.sql', sql);
    location.hash = '#ask';
    dispatchEvent(new CustomEvent('pcomm:ask', { detail: sql }));
  }

  async function send() {
    const message = input.trim();
    if (!message || busy) return;
    input = '';
    busy = true;
    turns = [...turns, { role: 'user', text: message }];
    turns = [...turns, { role: 'assistant', text: '', queries: [], charts: [], error: null }];
    const me = turns.length - 1;

    const patch = (fn) => {
      const next = [...turns];
      next[me] = fn({ ...next[me] });
      turns = next;
      scroll();
    };

    controller = new AbortController();
    try {
      for await (const ev of ask(message, { signal: controller.signal })) {
        if (ev.kind === 'text') patch((t) => ({ ...t, text: t.text + ev.delta }));
        else if (ev.kind === 'tool' && ev.name === 'sql')
          patch((t) => ({ ...t, queries: [...t.queries, ev.args.query] }));
        else if (ev.kind === 'chart') patch((t) => ({ ...t, charts: [...t.charts, ev.chart] }));
        else if (ev.kind === 'error') patch((t) => ({ ...t, error: ev.message }));
        else if (ev.kind === 'done' && ev.error) patch((t) => ({ ...t, error: ev.error }));
      }
    } catch (e) {
      if (e.name !== 'AbortError') patch((t) => ({ ...t, error: e.message }));
    } finally {
      busy = false;
      controller = null;
    }
  }

  function stop() {
    controller?.abort();
    abort();
    busy = false;
  }

  async function clear() {
    stop();
    await reset();
    turns = [];
  }

  function onkey(e) {
    if (e.key === 'Enter' && !e.shiftKey) {
      e.preventDefault();
      send();
    }
  }

  // The model writes markdown. Rather than pull in a renderer, split on fenced
  // code — the part that is unreadable as plain text — and leave the rest as
  // written. A half-rendered markdown subset reads worse than none.
  function segments(text) {
    const out = [];
    const re = /```(?:\w+)?\n([\s\S]*?)```/g;
    let last = 0;
    let m;
    while ((m = re.exec(text))) {
      if (m.index > last) out.push({ code: false, text: text.slice(last, m.index) });
      out.push({ code: true, text: m[1] });
      last = re.lastIndex;
    }
    if (last < text.length) out.push({ code: false, text: text.slice(last) });
    return out;
  }
</script>

<aside
  class="flex flex-col border-l shrink-0 h-screen sticky top-0 w-[min(440px,42vw)]"
  style="border-color: var(--border); background: var(--surface-1)"
  aria-label="Ask the capture"
>
  <header class="flex items-center gap-2 px-4 py-3 border-b" style="border-color: var(--border)">
    <h2 class="text-[15px] font-semibold m-0 grow">Chat</h2>
    {#if turns.length}
      <button onclick={clear} class="text-xs cursor-pointer bg-transparent border-0 p-0" style="color: var(--text-3)">clear</button>
    {/if}
    <button onclick={onclose} class="text-xs cursor-pointer bg-transparent border-0 p-0" style="color: var(--text-3)" aria-label="close the chat pane">close</button>
  </header>

  <div bind:this={log} class="grow overflow-y-auto px-4 py-3">
    {#if status.state === 'checking'}
      <p class="text-[13px] m-0" style="color: var(--text-2)">looking for the agent…</p>
    {:else if status.state === 'down'}
      <div class="text-[13px]" style="color: var(--text-2)">
        <p class="m-0 mb-2" style="color: var(--critical)">No agent at {agentUrl()}.</p>
        <p class="m-0 mb-2">The chat pane needs the sidecar running beside this server:</p>
        <pre class="text-[11px] font-mono p-2 rounded m-0 whitespace-pre-wrap" style="background: var(--surface-0); color: var(--text-2)">node agent/bridge.mjs --ui {location.origin}</pre>
        <p class="mt-2 mb-0 text-[11px]">
          A bridge may be running and still not answer this page: it accepts only the origin it was
          started with, so one started for a different capture is refused rather than allowed to
          answer about the wrong run. Point at another with <code class="font-mono">?agent=&lt;url&gt;</code>.
        </p>
        {#if status.detail}<p class="mt-2 mb-0 text-[11px]">{status.detail}</p>{/if}
      </div>
    {:else if status.state === 'mismatch'}
      <div class="text-[13px]" style="color: var(--text-2)">
        <p class="m-0 mb-2" style="color: var(--critical)">That agent is reading a different capture.</p>
        <p class="m-0 mb-2">
          The bridge at {agentUrl()} queries <code class="font-mono">{status.mismatch}</code>, but this page is
          <code class="font-mono">{location.origin}</code>. Its answers would be about the other run.
        </p>
        <pre class="text-[11px] font-mono p-2 rounded m-0 whitespace-pre-wrap" style="background: var(--surface-0); color: var(--text-2)">node agent/bridge.mjs --ui {location.origin} --port 8779</pre>
        <p class="mt-2 mb-0 text-[11px]">then reload with <code class="font-mono">?agent=http://127.0.0.1:8779</code></p>
      </div>
    {:else if !turns.length}
      <div class="text-[13px]" style="color: var(--text-2)">
        <p class="m-0 mb-2">Ask about this capture in plain English. Every answer comes with the SQL behind it.</p>
        <ul class="m-0 pl-4">
          <li>which rank pair moved the most bytes?</li>
          <li>is the traffic balanced in both directions?</li>
          <li>plot the distribution of message sizes</li>
          <li>did anything fail, or is the capture clean?</li>
        </ul>
      </div>
    {/if}

    <!-- A question and its answer have to be told apart at a glance, or a long
         thread reads as one continuous wall of text. The question gets the
         raised surface, a left rule and the tighter measure; the answer sits
         flat on the pane. Not colour alone: the shape differs too. -->
    {#each turns as t}
      {#if t.role === 'user'}
        <div
          class="mt-4 mb-2 rounded-md border-l-2 px-2.5 py-1.5"
          style="background: var(--surface-0); border-color: var(--text-3)"
        >
          <p class="text-[13px] font-medium m-0" style="color: var(--text-1)">{t.text}</p>
        </div>
      {:else}
        <div class="mb-5 pl-0.5">
          {#each t.queries as q}
            <details class="mb-1.5">
              <summary class="cursor-pointer text-[11px] font-mono truncate" style="color: var(--text-3)">
                sql · {q.replace(/\s+/g, ' ').slice(0, 60)}
              </summary>
              <pre class="text-[11px] font-mono whitespace-pre-wrap mt-1 mb-1" style="color: var(--text-2)">{q}</pre>
              <button
                onclick={() => toAsk(q)}
                class="text-[11px] rounded px-2 py-0.5 border cursor-pointer"
                style="background: var(--surface-0); border-color: var(--border); color: var(--text-2)"
              >open in Ask</button>
            </details>
          {/each}

          {#each segments(t.text) as s}
            {#if s.code}
              <pre class="text-[11px] font-mono whitespace-pre-wrap rounded p-2 my-1.5" style="background: var(--surface-0); color: var(--text-2)">{s.text}</pre>
            {:else}
              <p class="text-[13px] whitespace-pre-wrap m-0" style="color: var(--text-1)">{s.text}</p>
            {/if}
          {/each}

          {#each t.charts as c}
            <Chart chart={c} onask={toAsk} />
          {/each}

          {#if t.error}
            <p class="text-[12px] mt-2 m-0" style="color: var(--critical)">{t.error}</p>
          {/if}
        </div>
      {/if}
    {/each}
  </div>

  <div class="border-t p-3" style="border-color: var(--border)">
    <textarea
      bind:value={input}
      onkeydown={onkey}
      disabled={status.state !== 'ready'}
      placeholder={status.state === 'ready' ? 'Ask about this capture…' : 'the agent is not running'}
      rows="2"
      class="w-full text-[13px] rounded-md border p-2 resize-none disabled:opacity-50"
      style="background: var(--surface-0); border-color: var(--border); color: var(--text-1)"
    ></textarea>
    <div class="flex items-center gap-2 mt-1.5">
      <button
        onclick={busy ? stop : send}
        disabled={status.state !== 'ready' || (!busy && !input.trim())}
        class="text-[13px] rounded-md px-3 py-1.5 border cursor-pointer disabled:opacity-50"
        style="background: var(--surface-0); border-color: var(--border); color: var(--text-1)"
      >{busy ? 'stop' : 'ask'}</button>
      {#if busy}<span class="text-xs" style="color: var(--text-3)">thinking…</span>{/if}
    </div>
  </div>
</aside>
