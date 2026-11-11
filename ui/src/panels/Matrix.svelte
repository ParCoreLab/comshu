<script>
  import { query, num } from '../lib/query.js';
  import { logScale, ramp, bytes, count } from '../lib/viz.js';
  import Card from '../lib/Card.svelte';
  import Tooltip from '../lib/Tooltip.svelte';

  let edges = $state([]);
  let error = $state(null);
  let tip = $state(null);
  let tipRef = $state(null);
  let showTable = $state(false);

  // Transport is a facet, never a sum. The measurements are different
  // quantities: an IB row is one message the application sent, an NVLink row is
  // one instrumented 4-byte store from whichever kernels NVBit was told to
  // instrument. A single "bytes" number over both would rank a whole NVLink
  // ring below one IB message, so the panel shows one transport at a time and
  // says which unit it is holding.
  let transport = $state(null);

  $effect(() => {
    query('SELECT src_rank, dst_rank, transport, unit, sends, bytes FROM comm_edges')
      .then((r) => {
        all = r;
        const present = [...new Set(r.map((e) => e.transport))];
        if (!present.includes(transport)) transport = present[0] ?? null;
      })
      .catch((e) => (error = e.message));
  });

  let all = $state([]);
  const transports = $derived([...new Set(all.map((e) => e.transport))].sort());
  $effect(() => {
    edges = all
      .filter((e) => e.transport === transport)
      .sort((a, b) => num(a.src_rank) - num(b.src_rank) || num(a.dst_rank) - num(b.dst_rank));
  });
  const unit = $derived(edges[0]?.unit ?? 'messages');

  // Ranks on each axis. dst_rank = -1 is NOT a rank — it means the peer could
  // not be resolved — so it gets its own trailing column instead of being
  // dropped. A matrix that silently omits unattributed traffic lies about its
  // totals, and this is the visible form of that risk.
  const ranks = $derived(
    [...new Set(edges.flatMap((e) => [num(e.src_rank), num(e.dst_rank)]))]
      .filter((r) => r >= 0)
      .sort((a, b) => a - b),
  );
  const hasUnresolved = $derived(edges.some((e) => num(e.dst_rank) < 0));
  const cols = $derived(hasUnresolved ? [...ranks, -1] : ranks);

  const cells = $derived(
    new Map(edges.map((e) => [`${e.src_rank},${e.dst_rank}`,
      { sends: num(e.sends), bytes: num(e.bytes) }])),
  );
  const colorOf = $derived(logScale([...cells.values()].map((c) => c.bytes)));
  const steps = $derived(ramp());

  const cell = $derived(ranks.length > 48 ? 14 : ranks.length > 24 ? 20 : 28);
  const gap = 2; // surface gap between fills
  const pad = { l: 46, t: 46 };
  const every = $derived(ranks.length > 32 ? 4 : ranks.length > 16 ? 2 : 1);
  const total = $derived([...cells.values()].reduce((a, c) => a + c.bytes, 0));
  const totalOps = $derived([...cells.values()].reduce((a, c) => a + c.sends, 0));

  // One delegated handler instead of one per cell: a 64-rank matrix is ~4k
  // marks, and 4k listeners to show one tooltip is a cost with no return.
  function onMove(ev) {
    const t = ev.target;
    const key = t?.dataset?.k;
    if (!key) { tip = null; return; }
    const v = cells.get(key);
    if (!v) { tip = null; return; }
    const [r, c] = key.split(',');
    tip = {
      title: `rank ${r} → ${Number(c) < 0 ? 'unresolved' : `rank ${c}`}`,
      body: `${bytes(v.bytes)} in ${count(v.sends)} ${unit}`,
    };
    tipRef?.move(ev);
  }
</script>

<Card
  title="Communication matrix"
  subtitle={error
    ? ''
    : `${ranks.length} ranks · ${edges.length} edges · ${bytes(total)} in ${count(totalOps)} ${unit}${
        hasUnresolved ? ' · includes unresolved traffic' : ''}`}
>
  {#if error}
    <p class="text-[13px]" style="color: var(--critical)">{error}</p>
    <p class="text-xs" style="color: var(--text-2)">
      Export and link a capture first:
      <code>comshu-read &lt;trace&gt; --parquet &lt;dir&gt; &amp;&amp; comshu-read --link &lt;dir&gt;</code>
    </p>
  {:else if !edges.length}
    <p class="text-[13px]" style="color: var(--text-2)">loading…</p>
  {:else}
    {#if transports.length > 1}
      <div class="flex gap-1 mb-3">
        {#each transports as t}
          <button
            onclick={() => (transport = t)}
            class="text-xs rounded px-2.5 py-1 border cursor-pointer"
            style="border-color: {transport === t ? 'var(--text-3)' : 'var(--border)'};
                   background: {transport === t ? 'var(--surface-0)' : 'transparent'};
                   color: {transport === t ? 'var(--text-1)' : 'var(--text-2)'}"
          >{t}</button>
        {/each}
        <span class="text-xs self-center ml-2" style="color: var(--text-3)">
          shown separately — these are different measurements, not addable
        </span>
      </div>
    {/if}

    <!-- The marks are aria-hidden and the SVG carries one text alternative: a
         heatmap cell is not a control, and 4k focusable rects would be a worse
         experience than the table view below, which is the accessible path to
         the same numbers. The handler is on the image for delegation only. -->
    <!-- svelte-ignore a11y_no_static_element_interactions -->
    <div class="overflow-x-auto">
      <svg
        width={pad.l + cols.length * cell + 8}
        height={pad.t + ranks.length * cell + 8}
        role="img"
        aria-label="communication matrix, bytes sent from each rank to each rank"
        onmousemove={onMove}
        onmouseleave={() => (tip = null)}
      >
        <text x="4" y="14" font-size="11" fill="var(--text-3)">from \ to</text>

        {#each cols as c, j}
          {#if c < 0 || j % every === 0}
            <text
              x={pad.l + j * cell + cell / 2} y={pad.t - 8} font-size="10"
              text-anchor="middle"
              fill={c < 0 ? 'var(--critical)' : 'var(--text-2)'}>{c < 0 ? '?' : c}</text>
          {/if}
        {/each}

        {#each ranks as r, i}
          {#if i % every === 0}
            <text
              x={pad.l - 8} y={pad.t + i * cell + cell / 2 + 3} font-size="10"
              text-anchor="end" fill="var(--text-2)">{r}</text>
          {/if}
        {/each}

        {#each ranks as r, i}
          {#each cols as c, j}
            {@const v = cells.get(`${r},${c}`)}
            <rect
              x={pad.l + j * cell} y={pad.t + i * cell}
              width={cell - gap} height={cell - gap} rx="2"
              fill={v ? colorOf(v.bytes) : 'var(--surface-0)'}
              data-k={v ? `${r},${c}` : undefined}
              aria-hidden="true"
            />
          {/each}
        {/each}
      </svg>
    </div>

    <div class="flex items-center gap-2 mt-3.5 text-xs" style="color: var(--text-2)">
      <span>less</span>
      <span class="flex">
        {#each steps as s}<i class="block w-4 h-2.5" style="background: {s}"></i>{/each}
      </span>
      <span>more</span>
      <span class="ml-1.5">bytes, log scale · {unit}</span>
    </div>

    <button
      class="mt-3 text-[13px] underline cursor-pointer bg-transparent border-0 p-0"
      style="color: var(--text-2)"
      onclick={() => (showTable = !showTable)}
    >{showTable ? 'Hide' : 'Show'} table view</button>

    {#if showTable}
      <div class="overflow-x-auto mt-2 max-h-80">
        <table class="w-full text-xs border-collapse">
          <thead>
            <tr>
              {#each ['src', 'dst', 'sends', 'bytes'] as h}
                <th class="text-right px-2.5 py-1 font-medium border-b"
                    style="color: var(--text-2); border-color: var(--border)">{h}</th>
              {/each}
            </tr>
          </thead>
          <tbody>
            {#each edges as e}
              <tr>
                <td class="text-right px-2.5 py-1 border-b" style="border-color: var(--border)">{e.src_rank}</td>
                <td class="text-right px-2.5 py-1 border-b" style="border-color: var(--border)">
                  {num(e.dst_rank) < 0 ? '?' : e.dst_rank}</td>
                <td class="text-right px-2.5 py-1 border-b" style="border-color: var(--border)">{count(num(e.sends))}</td>
                <td class="text-right px-2.5 py-1 border-b" style="border-color: var(--border)">{count(num(e.bytes))}</td>
              </tr>
            {/each}
          </tbody>
        </table>
      </div>
    {/if}
  {/if}
</Card>

<Tooltip bind:this={tipRef} {tip} />
