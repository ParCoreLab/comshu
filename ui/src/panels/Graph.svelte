<script>
  import { query, num } from '../lib/query.js';
  import { logScale, ramp, bytes, count } from '../lib/viz.js';
  import Card from '../lib/Card.svelte';
  import Tooltip from '../lib/Tooltip.svelte';

  // The comm matrix as a node-link graph. Same rows as Matrix.svelte, same
  // `comm_edges`, read as topology instead of as a table.
  //
  // WHY BOTH. A heatmap answers "how much, between which pair" and answers it
  // exactly — you can read a cell. It is bad at the question a topology
  // question actually is: what SHAPE is this. A star, a chain and a sparse mesh
  // are three different pictures here and three similar-looking grids there,
  // and on the 64-rank capture the matrix is 4096 cells of which 862 are lit,
  // which no one reads as "one hub with 63 spokes". This panel is the shape;
  // the matrix stays the numbers. Neither replaces the other.
  //
  // RANKS ARE ORDERED BY PHYSICAL NODE, not by rank id, and that is the whole
  // reason this is worth building at scale. The 64-rank capture is 16 nodes x 4
  // ranks with P2P disabled, so every edge is verbs — including the ones
  // between ranks sharing an HCA. Grouping the ring by `_node` makes an
  // intra-node edge a short chord inside one group's arc and an inter-node edge
  // a long one across the circle, so "did the four co-resident ranks really go
  // out over the NIC" is a thing you SEE rather than a query you write.
  //
  // Rank id order would have destroyed exactly that: on this capture ranks
  // 0-3 happen to share a node, so the two orderings agree here and would
  // silently disagree on any run whose launcher assigns ranks differently.
  // Grouping on the recorded node hash is right by construction.

  let all = $state([]);          // every edge, every transport
  let nodeOf = $state(new Map()); // rank -> physical node index
  let error = $state(null);
  let tip = $state(null);
  let tipRef = $state(null);
  let hover = $state(null);      // a rank, while the pointer is on its mark
  let showTable = $state(false);

  // Same facet rule as the matrix, for the same reason: an IB row is one
  // message the application sent and an NVLink row is one instrumented 4-byte
  // store. Drawing both at once would size a whole NVLink ring below a single
  // IB message.
  let transport = $state(null);

  // How many edges to draw, densest first. NOT cosmetic: 862 edges over 64
  // nodes is a hairball in which the star is invisible, and the honest fix is
  // to show fewer and say so in the subtitle rather than to draw all of them at
  // an opacity where nothing is legible. `top` is bytes-ranked, so what is
  // dropped is always the quietest traffic.
  const LIMITS = [50, 200, 1000, Infinity];
  let limit = $state(200);

  $effect(() => {
    query('SELECT src_rank, dst_rank, transport, unit, sends, bytes FROM comm_edges')
      .then((r) => {
        all = r;
        const present = [...new Set(r.map((e) => e.transport))];
        if (!present.includes(transport)) transport = present[0] ?? null;
      })
      .catch((e) => (error = e.message));
  });

  // Node grouping is optional: `_node` exists only where the capture recorded
  // node identity. Without it every rank falls in group 0 and the ring is a
  // plain circle in rank order — degraded, not broken.
  $effect(() => {
    query('SELECT rank, node FROM _node ORDER BY node, rank')
      .then((r) => {
        const ids = [...new Set(r.map((x) => x.node))];
        nodeOf = new Map(r.map((x) => [num(x.rank), ids.indexOf(x.node)]));
      })
      .catch(() => (nodeOf = new Map()));
  });

  const transports = $derived([...new Set(all.map((e) => e.transport))].sort());
  const facet = $derived(all.filter((e) => e.transport === transport));
  const unit = $derived(facet[0]?.unit ?? 'messages');

  const ranked = $derived(
    [...facet].sort((a, b) => num(b.bytes) - num(a.bytes)),
  );
  const edges = $derived(ranked.slice(0, limit === Infinity ? ranked.length : limit));

  // Every rank in the facet gets a mark, even when the edge filter hid all of
  // its edges — a rank vanishing from the picture because it is quiet is a lie
  // about who was in the job.
  const ranks = $derived(
    [...new Set(facet.flatMap((e) => [num(e.src_rank), num(e.dst_rank)]))]
      .filter((r) => r >= 0)
      .sort((a, b) => a - b),
  );
  const unresolved = $derived(facet.filter((e) => num(e.dst_rank) < 0));

  // Ring order: by (node, rank). Falls back to rank order when node identity is
  // absent, since every rank then shares group 0.
  const groupOf = $derived((r) => nodeOf.get(r) ?? 0);
  const order = $derived(
    [...ranks].sort((a, b) => groupOf(a) - groupOf(b) || a - b),
  );
  const groups = $derived([...new Set(order.map(groupOf))].sort((a, b) => a - b));

  // Per-rank totals drive the mark size, and they are computed over the FULL
  // facet rather than the drawn subset: a node is as big as the traffic it
  // carried, not as big as the traffic that survived the filter.
  const totals = $derived.by(() => {
    const t = new Map(ranks.map((r) => [r, { out: 0, in: 0, sends: 0, peers: new Set() }]));
    for (const e of facet) {
      const s = num(e.src_rank), d = num(e.dst_rank), b = num(e.bytes);
      if (t.has(s)) { t.get(s).out += b; t.get(s).sends += num(e.sends); t.get(s).peers.add(d); }
      if (t.has(d)) { t.get(d).in += b; t.get(d).peers.add(s); }
    }
    return t;
  });

  // --- geometry -------------------------------------------------------------
  // One gap slot between physical nodes, so a group reads as a group. With no
  // node identity there is one group and therefore no gap.
  const GAP = 1;
  const slots = $derived(order.length + (groups.length > 1 ? groups.length * GAP : 0));
  const angle = $derived.by(() => {
    const a = new Map();
    let slot = 0, prev = null;
    for (const r of order) {
      const g = groupOf(r);
      if (prev !== null && g !== prev && groups.length > 1) slot += GAP;
      prev = g;
      a.set(r, (slot / slots) * 2 * Math.PI - Math.PI / 2);
      slot += 1;
    }
    return a;
  });

  const R = $derived(order.length > 32 ? 210 : order.length > 12 ? 170 : 130);
  const M = 62;                                    // margin for labels and arcs
  const SIZE = $derived(2 * (R + M));
  const C = $derived(R + M);
  const px = $derived((r) => C + R * Math.cos(angle.get(r) ?? 0));
  const py = $derived((r) => C + R * Math.sin(angle.get(r) ?? 0));

  const colorOf = $derived(logScale(facet.map((e) => num(e.bytes))));
  const steps = $derived(ramp());

  // Mark radius is sqrt of total traffic, because a disc encodes magnitude by
  // AREA — radius-proportional would show the hub as ~60x the spokes rather
  // than the ~8x it is.
  const maxTotal = $derived(
    Math.max(1, ...ranks.map((r) => (totals.get(r)?.out ?? 0) + (totals.get(r)?.in ?? 0))),
  );
  const rad = $derived((r) => {
    const v = (totals.get(r)?.out ?? 0) + (totals.get(r)?.in ?? 0);
    const small = order.length > 32;
    return (small ? 2.5 : 3.5) + Math.sqrt(v / maxTotal) * (small ? 6 : 9);
  });

  /**
   * A curve bowed toward its DESTINATION, which is what makes direction
   * readable without an arrowhead on all 862 edges.
   *
   * A control point at the plain midpoint would put A->B and B->A on the same
   * path, drawing a reciprocal pair as one line and hiding the volume asymmetry
   * that the hub pattern exists to show (spokes send a full slot, the hub
   * answers with an eighth). Biasing the control toward the destination
   * separates them into two arcs that bow opposite ways.
   */
  function path(s, d) {
    const x1 = px(s), y1 = py(s), x2 = px(d), y2 = py(d);
    const mx = 0.35 * x1 + 0.65 * x2, my = 0.35 * y1 + 0.65 * y2;
    const k = 0.32; // pull toward centre: bundles chords, keeps the ring clear
    return `M${x1},${y1} Q${C + (mx - C) * k},${C + (my - C) * k} ${x2},${y2}`;
  }

  /** Arrowhead for one edge, drawn only when highlighted — see the caption. */
  function head(s, d) {
    const x1 = px(s), y1 = py(s), x2 = px(d), y2 = py(d);
    const mx = 0.35 * x1 + 0.65 * x2, my = 0.35 * y1 + 0.65 * y2;
    const k = 0.32;
    const cx = C + (mx - C) * k, cy = C + (my - C) * k;
    const t = 0.86;
    const bx = (1 - t) ** 2 * x1 + 2 * (1 - t) * t * cx + t * t * x2;
    const by = (1 - t) ** 2 * y1 + 2 * (1 - t) * t * cy + t * t * y2;
    const tx = 2 * (1 - t) * (cx - x1) + 2 * t * (x2 - cx);
    const ty = 2 * (1 - t) * (cy - y1) + 2 * t * (y2 - cy);
    const a = (Math.atan2(ty, tx) * 180) / Math.PI;
    return { x: bx, y: by, a };
  }

  const incident = $derived((e) =>
    hover === null || num(e.src_rank) === hover || num(e.dst_rank) === hover);
  const highlighted = $derived(hover === null ? [] : edges.filter(incident));

  // Group arcs: one labelled band per physical node, outside the ring.
  const arcs = $derived.by(() =>
    groups.map((g) => {
      const members = order.filter((r) => groupOf(r) === g);
      const a0 = angle.get(members[0]), a1 = angle.get(members.at(-1));
      const rr = R + 30;
      const large = a1 - a0 > Math.PI ? 1 : 0;
      const mid = (a0 + a1) / 2;
      return {
        g,
        d: members.length === 1
          ? null
          : `M${C + rr * Math.cos(a0)},${C + rr * Math.sin(a0)}` +
            ` A${rr},${rr} 0 ${large} 1 ${C + rr * Math.cos(a1)},${C + rr * Math.sin(a1)}`,
        lx: C + (rr + 12) * Math.cos(mid),
        ly: C + (rr + 12) * Math.sin(mid) + 3,
      };
    }),
  );

  // Axis labels are thinned the way the matrix thins its ticks, but the busiest
  // mark is always named: it is the one a reader looks for a name on first, and
  // on a star it is the answer to the question the picture raises.
  const every = $derived(order.length > 48 ? 8 : order.length > 24 ? 4 : 1);
  const busiest = $derived(
    order.length ? order.reduce((a, r) => (rad(r) > rad(a) ? r : a), order[0]) : null,
  );

  const shownBytes = $derived(edges.reduce((a, e) => a + num(e.bytes), 0));
  const facetBytes = $derived(facet.reduce((a, e) => a + num(e.bytes), 0));

  function onEdge(ev, e) {
    tip = {
      title: `rank ${e.src_rank} → ${num(e.dst_rank) < 0 ? 'unresolved' : `rank ${e.dst_rank}`}`,
      body: `${bytes(num(e.bytes))} in ${count(num(e.sends))} ${unit}`,
    };
    tipRef?.move(ev);
  }

  function onNode(ev, r) {
    const t = totals.get(r);
    tip = {
      title: `rank ${r}${nodeOf.has(r) ? ` · node ${groupOf(r)}` : ''}`,
      body: `out ${bytes(t.out)} · in ${bytes(t.in)} · ${t.peers.size} peers`,
    };
    tipRef?.move(ev);
  }
</script>

<Card
  title="Communication graph"
  subtitle={error
    ? ''
    : `${ranks.length} ranks · ${edges.length === facet.length
        ? `${facet.length} edges`
        : `${edges.length} of ${facet.length} edges (top by bytes)`} · ${bytes(shownBytes)}${
        groups.length > 1 ? ` · grouped by ${groups.length} nodes` : ''}`}
>
  {#if error}
    <p class="text-[13px] m-0" style="color: var(--critical)">{error}</p>
    <p class="text-xs mt-1 m-0" style="color: var(--text-2)">
      Needs the derived <code>comm_edges</code> table — a capture, or an export
      run through <code>pcomm link</code>.
    </p>
  {:else if !facet.length}
    <p class="text-[13px] m-0" style="color: var(--text-2)">loading…</p>
  {:else}
    <div class="flex flex-wrap items-center gap-1 mb-3">
      {#if transports.length > 1}
        {#each transports as t}
          <button
            onclick={() => (transport = t)}
            class="text-xs rounded px-2.5 py-1 border cursor-pointer"
            style="border-color: {transport === t ? 'var(--text-3)' : 'var(--border)'};
                   background: {transport === t ? 'var(--surface-0)' : 'transparent'};
                   color: {transport === t ? 'var(--text-1)' : 'var(--text-2)'}"
          >{t}</button>
        {/each}
        <span class="w-3"></span>
      {/if}
      <span class="text-xs mr-1" style="color: var(--text-3)">edges</span>
      {#each LIMITS as l}
        {#if l === Infinity || l < facet.length}
          <button
            onclick={() => (limit = l)}
            class="text-xs rounded px-2.5 py-1 border cursor-pointer"
            style="border-color: {limit === l ? 'var(--text-3)' : 'var(--border)'};
                   background: {limit === l ? 'var(--surface-0)' : 'transparent'};
                   color: {limit === l ? 'var(--text-1)' : 'var(--text-2)'}"
          >{l === Infinity ? `all ${facet.length}` : `top ${l}`}</button>
        {/if}
      {/each}
    </div>

    <div class="overflow-x-auto">
      <svg
        width={SIZE} height={SIZE} viewBox="0 0 {SIZE} {SIZE}"
        role="img"
        aria-label="communication graph: ranks on a ring grouped by physical node, edges curved from source to destination and coloured by bytes"
        onmouseleave={() => { tip = null; hover = null; }}
      >
        <!-- Node arcs first: they are context, so they sit under the data. -->
        {#each arcs as a}
          {#if a.d}
            <path d={a.d} fill="none" stroke="var(--border)" stroke-width="2" aria-hidden="true" />
          {/if}
          {#if groups.length > 1}
            <text x={a.lx} y={a.ly} font-size="9" text-anchor="middle"
                  fill="var(--text-3)" aria-hidden="true">n{a.g}</text>
          {/if}
        {/each}

        <!-- Edges. Thin, and dimmed rather than removed when a rank is hovered:
             the shape stays on screen as context for the highlighted subset. -->
        <g fill="none">
          {#each edges as e (e.src_rank + ':' + e.dst_rank)}
            {@const s = num(e.src_rank)}
            {@const d = num(e.dst_rank)}
            {#if d >= 0}
              <path
                d={path(s, d)}
                stroke={colorOf(num(e.bytes))}
                stroke-width={hover !== null && incident(e) ? 2 : 1.25}
                stroke-opacity={hover === null ? 0.75 : incident(e) ? 0.95 : 0.07}
                stroke-linecap="round"
                onmousemove={(ev) => onEdge(ev, e)}
                onmouseleave={() => (tip = null)}
                role="presentation"
              />
            {/if}
          {/each}
        </g>

        <!-- Arrowheads only on the highlighted subset. 862 heads at 64 ranks is
             ink that hides the thing it annotates; direction is carried for
             every edge by the curve's bow (see path()), and exactly on demand
             by these. -->
        {#each highlighted as e}
          {@const h = head(num(e.src_rank), num(e.dst_rank))}
          <path d="M0,0 L-6,2.6 L-6,-2.6 Z"
                transform="translate({h.x},{h.y}) rotate({h.a})"
                fill={colorOf(num(e.bytes))} aria-hidden="true" />
        {/each}

        <!-- Rank marks last, so an edge never crosses over one. -->
        {#each order as r}
          <circle
            cx={px(r)} cy={py(r)} r={rad(r)}
            fill={hover === null || hover === r ? 'var(--text-2)' : 'var(--text-3)'}
            fill-opacity={hover === null || hover === r ? 1 : 0.35}
            stroke="var(--surface-1)" stroke-width="2"
            onmouseenter={() => (hover = r)}
            onmousemove={(ev) => onNode(ev, r)}
            onmouseleave={() => { hover = null; tip = null; }}
            role="presentation"
          />
        {/each}

        <!-- Rank labels, thinned the way the matrix thins its axis ticks. The
             hub is always labelled: the busiest mark is the one a reader looks
             for a name on first. -->
        {#each order as r, i}
          {#if i % every === 0 || r === busiest || r === hover}
            {@const a = angle.get(r)}
            <!-- Outside the ring, not inside it: the interior is where every
                 chord converges, and a label there sits on top of the densest
                 ink in the picture. The band between the marks and the node
                 arcs is the only empty annulus available. -->
            <text
              x={C + (R + 13) * Math.cos(a)} y={C + (R + 13) * Math.sin(a) + 3}
              font-size="9" text-anchor="middle"
              fill={r === busiest || r === hover ? 'var(--text-1)' : 'var(--text-2)'}
              font-weight={r === busiest ? '600' : '400'}
              aria-hidden="true">{r}</text>
          {/if}
        {/each}
      </svg>
    </div>

    <div class="flex flex-wrap items-center gap-2 mt-3.5 text-xs" style="color: var(--text-2)">
      <span>less</span>
      <span class="flex">
        {#each steps as s}<i class="block w-4 h-2.5" style="background: {s}"></i>{/each}
      </span>
      <span>more</span>
      <span class="ml-1.5">bytes, log scale · {unit}</span>
      <span class="ml-2" style="color: var(--text-3)">
        mark area = total bytes in + out
      </span>
    </div>

    {#if unresolved.length}
      <p class="text-xs mt-2 m-0" style="color: var(--critical)">
        {unresolved.length} edge{unresolved.length === 1 ? '' : 's'} has no resolved
        destination and is not drawn — the matrix shows it in the “?” column.
      </p>
    {/if}

    <p class="text-xs mt-3 m-0" style="color: var(--text-3)">
      Each curve leaves its source and bows toward its destination, so a
      reciprocal pair is two arcs and not one line. Hover a rank to isolate its
      edges and show arrowheads; hover a curve for its totals. Ranks are
      arranged by physical node, so a chord inside one arc is traffic between
      ranks on the same host.
      {#if edges.length !== facet.length}
        Showing the {edges.length} heaviest of {facet.length} edges,
        {Math.round((shownBytes / facetBytes) * 100)}% of the bytes.
      {/if}
    </p>

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
              {#each ['rank', 'node', 'out', 'in', 'peers'] as h}
                <th class="px-2.5 py-1 font-medium border-b {h === 'rank' || h === 'node' ? 'text-left' : 'text-right'}"
                    style="color: var(--text-2); border-color: var(--border)">{h}</th>
              {/each}
            </tr>
          </thead>
          <tbody>
            {#each order as r}
              {@const t = totals.get(r)}
              <tr>
                <td class="px-2.5 py-1 border-b" style="border-color: var(--border)">{r}</td>
                <td class="px-2.5 py-1 border-b" style="border-color: var(--border)">
                  {nodeOf.has(r) ? `n${groupOf(r)}` : '—'}</td>
                <td class="px-2.5 py-1 text-right border-b whitespace-nowrap" style="border-color: var(--border)">{bytes(t.out)}</td>
                <td class="px-2.5 py-1 text-right border-b whitespace-nowrap" style="border-color: var(--border)">{bytes(t.in)}</td>
                <td class="px-2.5 py-1 text-right border-b" style="border-color: var(--border)">{t.peers.size}</td>
              </tr>
            {/each}
          </tbody>
        </table>
      </div>
    {/if}
  {/if}
</Card>

<Tooltip bind:this={tipRef} {tip} />
