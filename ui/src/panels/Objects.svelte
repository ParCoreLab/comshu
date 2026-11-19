<script>
  import { query, num, bool } from '../lib/query.js';
  import { bytes, count, logScale, ramp } from '../lib/viz.js';
  import Card from '../lib/Card.svelte';
  import Tooltip from '../lib/Tooltip.svelte';

  // The object view, after Snoopie's (ICS'24, §Object view): each object's
  // address space as a 1D heatmap of operations per address, with the memory
  // address, the instruction types and the participating remote GPUs on hover.
  //
  // ONE TABLE, every object, deliberately. Snoopie splits this into a tab per
  // GPU; we do not. The whole-capture list is the thing you actually want in
  // front of you — every object, its owner, its size and who touched it, sorted
  // and comparable across devices in one place. Tabs hide n-1 devices to remove
  // a column, and the column is cheaper.
  //
  // What the tabs were really fixing was a missing distinction, and that is
  // kept: gpu_access carries `run_device` (the GPU that ran the kernel) and
  // `mem_device` (the GPU whose memory was touched). `device` below is the
  // owner, `touched by` the actor, and they differ exactly when the access was
  // remote. Both are columns here. Rank is neither — it is the process that
  // recorded the row, and one process can drive several GPUs, which is why
  // grouping on it collapsed the 4-GPU ring into a self-loop.
  //
  // The GPU-as-actor rollup lives in Devices.svelte, which is a different
  // question ("what did this GPU do") rather than a filter of this one.
  //
  // Snoopie's version also carries source-line numbers and an optional 2D
  // reshape. Neither is here: source_line is empty in every capture we have,
  // and a 2D reshape needs the user to supply dimensions the capture does not
  // know. The 1D heatmap is the part the data supports today.
  const BINS = 64;

  let objects = $state(null);
  let error = $state(null);
  let sel = $state(null);        // {rank, id, life, base, size, device, accesses}
  let unplacedLife = $state(0);  // accesses naming an object but no incarnation
  let bins = $state([]);
  let tip = $state(null);
  let tipRef = $state(null);

  // ONE ROW PER INCARNATION (object_life), not per handle. A handle is the
  // buffer's base address, so a freed-and-reallocated address is one id for
  // two buffers; listing by id credited every access to both (LAMMPS r2: a
  // 140 KB halo buffer and the 319 KB one that replaced it at the same base,
  // each shown with all 630 sends). object_access names the incarnation each
  // access fell in, and the join is on it.
  //
  // The sort ends in device, base and life so it is total. On the ring capture
  // all four objects tie on every leading key, and without a tiebreak the rows
  // come back in a different order each run — which for a table read by
  // comparing rows is the difference between a view and a shuffle.
  $effect(() => {
    query(`
      SELECT m.rank, m.object AS id, m.life, m.base, m.size, m.device,
             m.attr_status, m.origins, m.emitters, m.open_ns, m.close_ns,
             count(*) OVER (PARTITION BY m.rank, m.object) AS lives,
             ri.node_ix,
             coalesce(a.n, 0) AS accesses, coalesce(a.touched_by, '') AS touched_by,
             coalesce(a.sources, '') AS sources
      FROM object_life m
      LEFT JOIN rank_identity ri ON ri.rank = m.rank
      LEFT JOIN (
        SELECT owner_rank, object, life, count(*) AS n,
               array_to_string(array_agg(DISTINCT 'rank ' || actor_rank || coalesce(' (GPU ' || actor_device || ')', '')), ', ')
                 AS touched_by,
               array_to_string(array_agg(DISTINCT source ORDER BY source), ' + ') AS sources
        FROM object_access WHERE life IS NOT NULL GROUP BY 1, 2, 3
      ) a ON a.owner_rank = m.rank AND a.object = m.object AND a.life = m.life
      ORDER BY accesses DESC, size DESC, rank, m.device, base, life
      LIMIT 500`)
      .then((r) => {
        objects = r;
        if (!sel) sel = r.find((o) => num(o.accesses) > 0) ?? null;
      })
      .catch((e) => (error = e.message));
  });

  // Accesses that name an object but not which of its incarnations: a p2p
  // access to an extent allocated more than once, whose tick is a drain time
  // and cannot pick one. Said, not dropped.
  $effect(() => {
    query(`SELECT count(*) AS n FROM object_access WHERE life IS NULL`)
      .then((r) => (unplacedLife = num(r[0]?.n) ?? 0))
      .catch(() => (unplacedLife = 0));
  });

  // "1 of 2 · 205 ms" — which incarnation, and how long it lived.
  const lifeLabel = (o) => {
    if (num(o.lives) <= 1) return '';
    const ms = o.close_ns == null ? null : (num(o.close_ns) - num(o.open_ns)) / 1e6;
    const span = ms == null ? 'live at exit' : ms >= 1000 ? `${(ms / 1000).toFixed(1)} s` : `${Math.round(ms)} ms`;
    return `${o.life} of ${o.lives} · ${span}`;
  };

  // Distinct owning devices, for the subtitle only — the table shows them all.
  const gpus = $derived(
    objects ? [...new Set(objects.map((o) => num(o.device)))] : [],
  );

  // Binned on object_access.obj_offset, which is where the two providers are
  // reconciled — see link_sql.hpp. Both are shown on one axis because they
  // describe the same buffer, and `exact` travels with them because they do not
  // agree on what offset zero means: a verbs offset is anchored to the object's
  // true base, a p2p offset to the first byte observed, since the peer-mapping
  // delta is not in the record (measured 128 GiB on nvshmem_p2p_fine-du04).
  // `covered` reports the observed extent so a partially touched buffer reads as
  // one rather than being silently rescaled.
  $effect(() => {
    if (!sel) { bins = []; return; }
    const r = num(sel.rank), o = num(sel.id), l = num(sel.life);
    query(`
      -- The window ends at the furthest byte touched. A UCX row's size is a
      -- datatype COUNT, not an extent (object_access.unit), so it contributes
      -- the one byte its offset names rather than stretching the window by a
      -- number of elements.
      WITH w AS (
        SELECT max(obj_offset + CASE WHEN unit = 'bytes' THEN size ELSE 1 END) AS hi
        FROM object_access WHERE owner_rank = ${r} AND object = ${o} AND life = ${l}
      )
      SELECT floor(a.obj_offset * ${BINS} / greatest(w.hi, 1))::INT AS bin,
             count(*)::BIGINT                         AS n,
             coalesce(sum(a.size) FILTER (WHERE a.unit = 'bytes'), 0)::BIGINT AS bytes,
             coalesce(sum(a.size) FILTER (WHERE a.unit <> 'bytes'), 0)::BIGINT AS units,
             min(a.obj_offset)::BIGINT                AS off_lo,
             max(a.obj_offset)::BIGINT                AS off_hi,
             array_to_string(array_agg(DISTINCT a.source ORDER BY a.source), ' + ')
                                                      AS sources,
             array_to_string(array_agg(DISTINCT 'rank ' || a.actor_rank || coalesce(' (GPU ' || a.actor_device || ')', '')), ', ')
                                                      AS actors,
             bool_and(a.exact)                        AS all_exact,
             (SELECT hi FROM w)::BIGINT               AS covered
      FROM object_access a, w
      WHERE a.owner_rank = ${r} AND a.object = ${o} AND a.life = ${l}
      GROUP BY bin ORDER BY bin`)
      .then((b) => (bins = b))
      .catch(() => (bins = []));
  });

  // Per-source totals for the selected object. The heatmap merges the streams
  // into one shape; this says what went into it, which is the difference
  // between combining two measurements and hiding one.
  let split = $state([]);
  $effect(() => {
    if (!sel) { split = []; return; }
    query(`
      SELECT source, bool_and(exact) AS exact, min(unit) AS unit,
             count(*)::BIGINT AS n, sum(size)::BIGINT AS amount,
             array_to_string(array_agg(DISTINCT 'rank ' || actor_rank || coalesce(' (GPU ' || actor_device || ')', '') ORDER BY 'rank ' || actor_rank || coalesce(' (GPU ' || actor_device || ')', '')), ', ') AS actors
      FROM object_access
      WHERE owner_rank = ${num(sel.rank)} AND object = ${num(sel.id)} AND life = ${num(sel.life)}
      GROUP BY 1 ORDER BY n DESC`)
      .then((r) => (split = r))
      .catch(() => (split = []));
  });

  // The query returns a row only for bins that were touched, so rendering
  // `bins` directly draws nothing where nothing happened — and "untouched" then
  // looks identical to "the object ends here". On the skew capture that is 43 of
  // 64 bins invisible, which reads as a much smaller object rather than a
  // sparsely written one. Densify so every bin is drawn, and give the empty ones
  // a visible track.
  const dense = $derived.by(() => {
    const by = new Map(bins.map((b) => [num(b.bin), b]));
    return Array.from({ length: BINS }, (_, i) => by.get(i) ?? null);
  });

  const colorOf = $derived(logScale(bins.map((b) => num(b.n))));
  const steps = $derived(ramp());
  const covered = $derived(bins.length ? num(bins[0].covered) : 0);
  // An object with no device says WHY (mem_object.attr_status, AttrStatus in
  // vocab.hpp): host memory, never asked, or a failure. They used to be one
  // word, 'unknown', which read as a failure for 487 healthy host buffers.
  const NO_DEVICE = {
    1: 'host',
    2: 'host (CUDA-visible)',
    3: 'not asked',
    4: 'failed (null)',
    5: 'failed',
    6: 'not named',
  };
  // What made the object known (mem_object.origin, ObjectOrigin in vocab.hpp),
  // in the words a user would type. An application allocation and a transport
  // registration share a record type; this is the column that tells them apart.
  const ORIGIN = {
    1: 'nvshmem_malloc',
    2: 'cudaMalloc',
    3: 'cudaMallocManaged',
    4: 'cudaMallocAsync',
    5: 'pinned host',
    6: 'cuMemMap',
    7: 'seen by NVBit',
    8: 'mapped region',
    9: 'ibv_reg_mr',
    10: 'DEVX umem',
    11: 'NCCL window',
  };
  const madeBy = (o) =>
    (o.origins ?? '')
      .split(',')
      .filter((x) => x !== '' && x !== '0')
      .map((x) => ORIGIN[+x] ?? `origin ${x}`)
      .join(' + ') || '—';
  // A total in the unit it was counted in. Bytes when the rows are bytes, a
  // plain count when they are UCP datatype units — the two are never added.
  const amount = (b, u) =>
    [num(b) ? bytes(num(b)) : null, num(u) ? `${count(num(u))} units` : null]
      .filter(Boolean)
      .join(' + ') || bytes(0);
  const dev = (d, st) =>
    num(d) === 65535 ? (NO_DEVICE[num(st)] ?? 'unknown') : `GPU ${d}`;
  // rank 9 · n2: the node is where host memory lives, and one fact with the rank.
  const where = (o) => (o.node_ix == null ? `rank ${o.rank}` : `rank ${o.rank} · n${o.node_ix}`);

  function onMove(ev) {
    const i = ev.target?.dataset?.b;
    if (i === undefined) { tip = null; return; }
    const b = dense[+i];
    if (!b) {
      // An untouched bin still has an address range, and saying so is the point
      // of drawing it at all.
      const lo = Math.floor((covered * +i) / BINS);
      const hi = Math.floor((covered * (+i + 1)) / BINS) - 1;
      tip = { title: `+${bytes(lo)} – ${bytes(hi)}`, body: 'no accesses recorded' };
      tipRef?.move(ev);
      return;
    }
    // The paper's hover carries the memory address, the instruction types and
    // the participating GPUs. All three are here; the address is absolute, as
    // the one thing you can paste into a debugger.
    tip = {
      title: `+${bytes(num(b.off_lo))} – ${bytes(num(b.off_hi))}${bool(b.all_exact) ? '' : ' (approx.)'}`,
      body: `${count(num(b.n))} touches · ${amount(b.bytes, b.units)} · via ${b.sources} · by ${b.actors}`,
    };
    tipRef?.move(ev);
  }
</script>

<Card
  title="Objects"
  subtitle={objects
    ? `${objects.length} objects on ${gpus.length} ${gpus.length === 1 ? 'device' : 'devices'} · ` +
      `${objects.filter((o) => num(o.accesses) > 0).length} with recorded accesses` +
      (unplacedLife ? ` · ${count(unplacedLife)} accesses name an object but not which incarnation` : '')
    : ''}
>
  {#if error}
    <p class="text-[13px] m-0" style="color: var(--critical)">{error}</p>
  {:else if !objects}
    <p class="text-[13px] m-0" style="color: var(--text-2)">loading…</p>
  {:else if !objects.length}
    <p class="text-[13px] m-0" style="color: var(--text-2)">
      This capture records no memory objects.
    </p>
  {:else}
    <!-- svelte-ignore a11y_no_static_element_interactions -->
    {#if sel && bins.length}
      <div class="mb-4">
        <div class="text-[13px] mb-1">
          <b>{bytes(num(sel.size))}</b> {madeBy(sel)} object on {where(sel)}, {dev(sel.device, sel.attr_status)}
          {#if lifeLabel(sel)}<span style="color: var(--text-2)"> — incarnation {lifeLabel(sel)}</span>{/if}
          <span style="color: var(--text-3)">— {count(num(sel.accesses))} accesses</span>
        </div>
        <div class="overflow-x-auto" onmousemove={onMove} onmouseleave={() => (tip = null)}>
          <svg width={BINS * 11 + 2} height="46" role="img"
               aria-label="access density across the object's address range">
            {#each dense as b, i}
              {#if b}
                <rect x={i * 11} y="0" width="9" height="34" rx="2"
                      fill={colorOf(num(b.n))} data-b={i} />
              {:else}
                <!-- Untouched: a faint track, not absence. Outlined rather than
                     filled so it cannot be mistaken for the lightest ramp step,
                     which is a real value. -->
                <rect x={i * 11} y="0" width="9" height="34" rx="2"
                      fill="var(--surface-1)" stroke="var(--border)" stroke-width="1"
                      data-b={i} />
              {/if}
            {/each}
            <text x="0" y="44" font-size="10" fill="var(--text-3)">0</text>
            <text x={BINS * 11} y="44" font-size="10" text-anchor="end"
                  fill="var(--text-3)">{bytes(covered)}</text>
          </svg>
        </div>
        <div class="flex items-center gap-2 mt-1.5 text-xs" style="color: var(--text-2)">
          <span class="inline-flex items-center gap-1">
            <i class="block w-3 h-2" style="background: var(--surface-1);
                      box-shadow: inset 0 0 0 1px var(--border)"></i>
            untouched
          </span>
          <span>fewer</span>
          <span class="flex">
            {#each steps as s}<i class="block w-3 h-2" style="background: {s}"></i>{/each}
          </span>
          <span>more accesses, log scale</span>
        </div>
        {#if split.length}
          <div class="flex flex-wrap gap-x-5 gap-y-1 mt-2 text-xs">
            {#each split as sp}
              <span>
                <b>{sp.source}</b>
                <span style="color: var(--text-2)">
                  {count(num(sp.n))} touches ·
                  {sp.unit === 'bytes' ? bytes(num(sp.amount)) : `${count(num(sp.amount))} units`}
                  · by {sp.actors}
                </span>
                {#if !bool(sp.exact)}
                  <span style="color: var(--text-3)"> · offsets approximate</span>
                {/if}
              </span>
            {/each}
          </div>
        {/if}

        <p class="text-xs mt-2 m-0" style="color: var(--text-3)">
          Binned over the address window actually touched
          ({bytes(covered)} of {bytes(num(sel.size))}). Offsets are
          from the object's real base: RDMA names the peer's own address, and a
          GPU access carries the address its owner recorded the buffer at. The
          one inexact case is a receive whose work-request id named several
          posted buffers; a bin holding one is marked approximate on hover.
          A <code>ucx_send</code> touch is a message that LEFT this object, by
          the rank it went to; a <code>ucx_rma</code> touch is a put or get that
          named this object's address. Both count UCP datatype
          <i>units</i>, which are bytes only for a byte datatype, so they are
          totalled apart from bytes and never added to them.
        </p>
      </div>
    {/if}

    <div class="overflow-x-auto max-h-[50vh]">
      <table class="w-full text-xs border-collapse">
        <thead>
          <tr>
            {#each ['', 'rank', 'device', 'made by', 'base', 'size', 'lifetime', 'touched by', 'via', 'accesses'] as h}
              <th class="px-2.5 py-1 font-medium border-b {h === 'accesses' || h === 'size' ? 'text-right' : 'text-left'}"
                  style="color: var(--text-2); border-color: var(--border)">{h}</th>
            {/each}
          </tr>
        </thead>
        <tbody>
          {#each objects as o}
            {@const active = sel && sel.rank === o.rank && sel.id === o.id && sel.life === o.life}
            <tr>
              <td class="px-2.5 py-1 border-b" style="border-color: var(--border)">
                <button
                  onclick={() => (sel = o)}
                  disabled={num(o.accesses) === 0}
                  class="text-xs rounded px-2 py-0.5 border cursor-pointer disabled:opacity-30 disabled:cursor-default"
                  style="border-color: {active ? 'var(--text-3)' : 'var(--border)'};
                         background: {active ? 'var(--surface-0)' : 'transparent'};
                         color: var(--text-2)"
                >{active ? 'shown' : 'view'}</button>
              </td>
              <td class="px-2.5 py-1 border-b whitespace-nowrap" style="border-color: var(--border)">{o.node_ix == null ? o.rank : `${o.rank} · n${o.node_ix}`}</td>
              <td class="px-2.5 py-1 border-b whitespace-nowrap" style="border-color: var(--border)">
                <span style="color: {num(o.device) === 65535 ? 'var(--text-3)' : 'var(--text-1)'}">{dev(o.device, o.attr_status)}</span>
              </td>
              <td class="px-2.5 py-1 border-b whitespace-nowrap" style="border-color: var(--border); color: var(--text-2)">{madeBy(o)}</td>
              <td class="px-2.5 py-1 border-b font-mono" style="border-color: var(--border)">0x{num(o.base).toString(16)}</td>
              <td class="px-2.5 py-1 text-right border-b whitespace-nowrap" style="border-color: var(--border)">{bytes(num(o.size))}</td>
              <td class="px-2.5 py-1 border-b whitespace-nowrap" style="border-color: var(--border); color: var(--text-2)">{lifeLabel(o) || '—'}</td>
              <td class="px-2.5 py-1 border-b whitespace-nowrap" style="border-color: var(--border)">
                <span style="color: {o.touched_by ? 'var(--text-1)' : 'var(--text-3)'}">{o.touched_by || '—'}</span>
              </td>
              <td class="px-2.5 py-1 border-b whitespace-nowrap" style="border-color: var(--border)">
                <span style="color: {o.sources ? 'var(--text-1)' : 'var(--text-3)'}">{o.sources || '—'}</span>
              </td>
              <td class="px-2.5 py-1 text-right border-b" style="border-color: var(--border)">{count(num(o.accesses))}</td>
            </tr>
          {/each}
        </tbody>
      </table>
    </div>
    <p class="text-xs mt-2 m-0" style="color: var(--text-3)">
      <b>device</b> is the GPU that <i>owns</i> the memory; <b>touched by</b> is
      whoever did the touching, as <code>rank N (GPU M)</code>. The GPU is
      omitted rather than guessed when a rank drives more than one — which is the
      normal case for a single-process multi-GPU run.
      <b>via</b> names which of them saw it, and an object seen by both is not
      double-counted so much as observed twice. <code>rank</code> is the process
      that recorded the row, which is neither of the above, and the node it ran on.
      With no GPU, the device says why: <code>host</code> (CUDA says it is not
      GPU memory), <code>not asked</code> (registered before CUDA was loaded),
      or <code>failed</code> — the only one that is a problem. None of them is
      merged with GPU 0. <b>made by</b> is the call that made the object known:
      an application allocation (<code>cudaMalloc</code>,
      <code>nvshmem_malloc</code>, <code>cuMemMap</code>…) or a transport
      registration (<code>ibv_reg_mr</code>, <code>DEVX umem</code>), which
      usually covers memory some allocation already named. A row is one
      <b>lifetime</b> of a buffer: an address freed and allocated again is a
      new row, and each access is credited to the one live when it happened.
      Objects with no
      recorded accesses are listed but not viewable: the transport registers
      buffers the instrumented kernels never touch.
    </p>
  {/if}
</Card>

<Tooltip bind:this={tipRef} {tip} />
