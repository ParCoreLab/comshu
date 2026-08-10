<script>
  import { query, num } from '../lib/query.js';
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
  let sel = $state(null);        // {rank, id, base, size, device, accesses}
  let bins = $state([]);
  let tip = $state(null);
  let tipRef = $state(null);

  // DISTINCT because a handle can open more than once — a freed address gets
  // reused — and the list is of objects, not of lifetimes.
  //
  // The sort ends in device and base so it is total. On the ring capture all
  // four objects tie on every leading key, and without a tiebreak the rows come
  // back in a different order each run — which for a table read by comparing
  // rows is the difference between a view and a shuffle.
  $effect(() => {
    query(`
      SELECT m.rank, m.id, m.range_base AS base, m.range_size AS size, m.device,
             coalesce(a.n, 0) AS accesses, coalesce(a.touched_by, '') AS touched_by,
             coalesce(a.sources, '') AS sources
      FROM (SELECT DISTINCT rank, id, range_base, range_size, device FROM mem_object) m
      LEFT JOIN (
        SELECT owner_rank, object, count(*) AS n,
               array_to_string(array_agg(DISTINCT actor_kind || ' ' || actor), ', ')
                 AS touched_by,
               array_to_string(array_agg(DISTINCT source ORDER BY source), ' + ') AS sources
        FROM object_access GROUP BY 1, 2
      ) a ON a.owner_rank = m.rank AND a.object = m.id
      ORDER BY accesses DESC, size DESC, rank, m.device, base
      LIMIT 500`)
      .then((r) => {
        objects = r;
        if (!sel) sel = r.find((o) => num(o.accesses) > 0) ?? null;
      })
      .catch((e) => (error = e.message));
  });

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
    const r = num(sel.rank), o = num(sel.id);
    query(`
      WITH w AS (
        SELECT max(obj_offset) + max(size) AS hi
        FROM object_access WHERE owner_rank = ${r} AND object = ${o}
      )
      SELECT floor(a.obj_offset * ${BINS} / greatest(w.hi, 1))::INT AS bin,
             count(*)::BIGINT                         AS n,
             sum(a.size)::BIGINT                      AS bytes,
             min(a.obj_offset)::BIGINT                AS off_lo,
             max(a.obj_offset)::BIGINT                AS off_hi,
             array_to_string(array_agg(DISTINCT a.source ORDER BY a.source), ' + ')
                                                      AS sources,
             array_to_string(array_agg(DISTINCT a.actor_kind || ' ' || a.actor), ', ')
                                                      AS actors,
             bool_and(a.exact)                        AS all_exact,
             (SELECT hi FROM w)::BIGINT               AS covered
      FROM object_access a, w
      WHERE a.owner_rank = ${r} AND a.object = ${o}
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
      SELECT source, actor_kind, bool_and(exact) AS exact,
             count(*)::BIGINT AS n, sum(size)::BIGINT AS bytes,
             array_to_string(array_agg(DISTINCT actor ORDER BY actor), ', ') AS actors
      FROM object_access
      WHERE owner_rank = ${num(sel.rank)} AND object = ${num(sel.id)}
      GROUP BY 1, 2 ORDER BY n DESC`)
      .then((r) => (split = r))
      .catch(() => (split = []));
  });

  const colorOf = $derived(logScale(bins.map((b) => num(b.n))));
  const steps = $derived(ramp());
  const covered = $derived(bins.length ? num(bins[0].covered) : 0);
  const dev = (d) => (num(d) === 65535 ? 'unknown' : `GPU ${d}`);

  function onMove(ev) {
    const i = ev.target?.dataset?.b;
    if (i === undefined) { tip = null; return; }
    const b = bins[+i];
    // The paper's hover carries the memory address, the instruction types and
    // the participating GPUs. All three are here; the address is absolute, as
    // the one thing you can paste into a debugger.
    tip = {
      title: `+${bytes(num(b.off_lo))} – ${bytes(num(b.off_hi))}${b.all_exact ? '' : ' (approx.)'}`,
      body: `${count(num(b.n))} touches · ${bytes(num(b.bytes))} · via ${b.sources} · by ${b.actors}`,
    };
    tipRef?.move(ev);
  }
</script>

<Card
  title="Objects"
  subtitle={objects
    ? `${objects.length} objects on ${gpus.length} ${gpus.length === 1 ? 'device' : 'devices'} · ` +
      `${objects.filter((o) => num(o.accesses) > 0).length} with recorded accesses`
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
          <b>{bytes(num(sel.size))}</b> object on rank {sel.rank}, {dev(sel.device)}
          <span style="color: var(--text-3)">— {count(num(sel.accesses))} accesses</span>
        </div>
        <div class="overflow-x-auto" onmousemove={onMove} onmouseleave={() => (tip = null)}>
          <svg width={BINS * 11 + 2} height="46" role="img"
               aria-label="access density across the object's address range">
            {#each bins as b, i}
              <rect x={num(b.bin) * 11} y="0" width="9" height="34" rx="2"
                    fill={colorOf(num(b.n))} data-b={i} />
            {/each}
            <text x="0" y="44" font-size="10" fill="var(--text-3)">0</text>
            <text x={BINS * 11} y="44" font-size="10" text-anchor="end"
                  fill="var(--text-3)">{bytes(covered)}</text>
          </svg>
        </div>
        <div class="flex items-center gap-2 mt-1.5 text-xs" style="color: var(--text-2)">
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
                  {count(num(sp.n))} touches · {bytes(num(sp.bytes))} · by {sp.actor_kind}
                  {sp.actors}
                </span>
                {#if !sp.exact}
                  <span style="color: var(--text-3)"> · offsets approximate</span>
                {/if}
              </span>
            {/each}
          </div>
        {/if}

        <p class="text-xs mt-2 m-0" style="color: var(--text-3)">
          Binned over the address window actually touched
          ({bytes(covered)} of {bytes(num(sel.size))}). The two sources do not
          agree on where offset zero is: a <code>verbs</code> offset is exact,
          because RDMA names the peer's real address and the object's base is
          known; a <code>p2p</code> access arrives at this process's mapping of
          the peer's buffer, whose distance from the base is not in the record
          (measured 128 GiB on one capture), so its offsets run from the first
          byte observed. Any bin mixing the two is marked approximate on hover.
        </p>
      </div>
    {/if}

    <div class="overflow-x-auto max-h-[50vh]">
      <table class="w-full text-xs border-collapse">
        <thead>
          <tr>
            {#each ['', 'rank', 'device', 'base', 'size', 'touched by', 'via', 'accesses'] as h}
              <th class="px-2.5 py-1 font-medium border-b {h === 'accesses' || h === 'size' ? 'text-right' : 'text-left'}"
                  style="color: var(--text-2); border-color: var(--border)">{h}</th>
            {/each}
          </tr>
        </thead>
        <tbody>
          {#each objects as o}
            {@const active = sel && sel.rank === o.rank && sel.id === o.id}
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
              <td class="px-2.5 py-1 border-b" style="border-color: var(--border)">{o.rank}</td>
              <td class="px-2.5 py-1 border-b whitespace-nowrap" style="border-color: var(--border)">
                <span style="color: {num(o.device) === 65535 ? 'var(--text-3)' : 'var(--text-1)'}">{dev(o.device)}</span>
              </td>
              <td class="px-2.5 py-1 border-b font-mono" style="border-color: var(--border)">0x{num(o.base).toString(16)}</td>
              <td class="px-2.5 py-1 text-right border-b whitespace-nowrap" style="border-color: var(--border)">{bytes(num(o.size))}</td>
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
      whoever did the touching — a GPU for <code>p2p</code>, a rank for
      <code>verbs</code>, because the two providers observe in different spaces.
      <b>via</b> names which of them saw it, and an object seen by both is not
      double-counted so much as observed twice. <code>rank</code> is the process
      that recorded the row, which is neither of the above. <code>unknown</code> is device id 65535 — attribution failed,
      which is not the same as host memory and is deliberately not merged with
      GPU 0. Objects with no recorded accesses are listed but not viewable: the
      transport registers buffers the instrumented kernels never touch.
    </p>
  {/if}
</Card>

<Tooltip bind:this={tipRef} {tip} />
