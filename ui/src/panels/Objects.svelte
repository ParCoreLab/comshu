<script>
  import { query, num } from '../lib/query.js';
  import { bytes, count, logScale, ramp } from '../lib/viz.js';
  import Card from '../lib/Card.svelte';
  import Tooltip from '../lib/Tooltip.svelte';

  // The object view, after Snoopie's (ICS'24, §Object view): each object's
  // address space as a 1D heatmap of operations per address, with the memory
  // address, the instruction types and the participating remote GPUs on hover.
  //
  // GROUPED BY THE OWNING GPU, which is Snoopie's "each GPU has its separate
  // object view tab" and is not the same as grouping by rank. gpu_access
  // carries both `run_device` (the GPU that ran the kernel) and `mem_device`
  // (the GPU whose memory was touched); an object belongs to the latter. One
  // process can drive several GPUs — on the 4-GPU ring capture a single rank
  // owns objects on devices 0..3 — so keying the tab on rank collapses four
  // GPUs into one tab and silently mixes them. Rank stays in the table, as the
  // process that recorded the row.
  //
  // Snoopie's version also carries source-line numbers and an optional 2D
  // reshape. Neither is here: source_line is empty in every capture we have,
  // and a 2D reshape needs the user to supply dimensions the capture does not
  // know. The 1D heatmap is the part the data supports today.
  const BINS = 64;

  let objects = $state(null);
  let error = $state(null);
  let gpu = $state(null);        // the owning device whose tab is open
  let sel = $state(null);        // {rank, id, base, size, device, accesses}
  let bins = $state([]);
  let tip = $state(null);
  let tipRef = $state(null);

  // DISTINCT because a handle can open more than once — a freed address gets
  // reused — and the list is of objects, not of lifetimes.
  $effect(() => {
    query(`
      SELECT m.rank, m.id, m.range_base AS base, m.range_size AS size, m.device,
             coalesce(a.n, 0) AS accesses, coalesce(a.touched_by, '') AS touched_by
      FROM (SELECT DISTINCT rank, id, range_base, range_size, device FROM mem_object) m
      LEFT JOIN (
        SELECT rank, object, count(*) AS n,
               array_to_string(array_agg(DISTINCT run_device ORDER BY run_device), ', ')
                 AS touched_by
        FROM gpu_access GROUP BY 1, 2
      ) a ON a.rank = m.rank AND a.object = m.id
      ORDER BY accesses DESC, size DESC, rank
      LIMIT 500`)
      .then((r) => {
        objects = r;
        const first = r.find((o) => num(o.accesses) > 0) ?? r[0] ?? null;
        if (gpu === null && first) gpu = num(first.device);
        if (!sel) sel = r.find((o) => num(o.accesses) > 0) ?? null;
      })
      .catch((e) => (error = e.message));
  });

  // The tabs, in device order, with `unknown` (65535) last because it is a
  // failure bucket and not a GPU.
  const gpus = $derived(
    objects
      ? [...new Set(objects.map((o) => num(o.device)))].sort((a, b) => a - b)
      : [],
  );
  const shown = $derived(objects ? objects.filter((o) => num(o.device) === gpu) : []);

  // The heatmap is binned over the OBSERVED address window, not over
  // [base, base+size). For a peer access the address is this process's mapping
  // of the peer's buffer, which sits far from the object's own base — measured
  // ~275 GB away — so subtracting `base` would produce nonsense. min(address)
  // is the mapping base, and it is exact whenever the workload touches the
  // whole buffer: on the capture this was built against the observed span is
  // 4092 over a 4096-byte object. `covered` below reports that, so a partially
  // touched buffer is visible as such rather than silently mislabelled.
  $effect(() => {
    if (!sel) { bins = []; return; }
    const r = num(sel.rank), o = num(sel.id);
    query(`
      WITH w AS (
        SELECT min(address) AS lo, max(address) + max(size) AS hi
        FROM gpu_access WHERE rank = ${r} AND object = ${o}
      )
      SELECT floor((g.address - w.lo) * ${BINS} / greatest(w.hi - w.lo, 1))::INT AS bin,
             count(*)::BIGINT                        AS n,
             sum(g.size)::BIGINT                     AS bytes,
             min(g.address - w.lo)::BIGINT           AS off_lo,
             max(g.address - w.lo)::BIGINT           AS off_hi,
             min(g.address)::BIGINT                  AS addr_lo,
             array_to_string(array_agg(DISTINCT g.run_device ORDER BY g.run_device), ', ')
                                                     AS from_gpus,
             array_to_string(array_agg(DISTINCT
               CASE g.op WHEN 0 THEN 'load' WHEN 1 THEN 'store'
                         WHEN 2 THEN 'atomic' WHEN 3 THEN 'prefetch'
                         ELSE 'op' || g.op END), ', ') AS ops,
             array_to_string(array_agg(DISTINCT g.mem_device::VARCHAR), ', ') AS peers,
             (SELECT hi - lo FROM w)::BIGINT          AS covered
      FROM gpu_access g, w
      WHERE g.rank = ${r} AND g.object = ${o}
      GROUP BY bin ORDER BY bin`)
      .then((b) => (bins = b))
      .catch(() => (bins = []));
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
      title: `0x${num(b.addr_lo).toString(16)}  (+${bytes(num(b.off_lo))}–${bytes(num(b.off_hi))})`,
      body: `${count(num(b.n))} ${b.ops} · ${bytes(num(b.bytes))} · from GPU ${b.from_gpus} · on GPU ${b.peers}`,
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
    <!-- One tab per owning GPU, after the paper. Rendered even for a single
         GPU: it names which device you are looking at, which a bare table
         does not. -->
    <div class="flex flex-wrap gap-1 mb-3">
      {#each gpus as g}
        <button
          onclick={() => {
            gpu = g;
            // Move the plot with the tab. Leaving the previous device's object
            // selected would render one GPU's heatmap under another's tab —
            // the exact confusion the tabs exist to prevent.
            sel = objects.find((o) => num(o.device) === g && num(o.accesses) > 0) ?? null;
          }}
          class="text-xs rounded px-2.5 py-1 border cursor-pointer"
          style="border-color: {gpu === g ? 'var(--text-3)' : 'var(--border)'};
                 background: {gpu === g ? 'var(--surface-1)' : 'transparent'};
                 color: {gpu === g ? 'var(--text-1)' : 'var(--text-2)'}"
        >{dev(g)}</button>
      {/each}
    </div>

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
        <p class="text-xs mt-2 m-0" style="color: var(--text-3)">
          Binned over the address window actually touched
          ({bytes(covered)} of {bytes(num(sel.size))}). A peer access arrives at
          this process's mapping of the peer's buffer, not at the object's own
          base, so offsets are measured from the first byte observed rather than
          from <code>base</code>.
        </p>
      </div>
    {/if}

    <div class="overflow-x-auto max-h-[50vh]">
      <table class="w-full text-xs border-collapse">
        <thead>
          <tr>
            {#each ['', 'rank', 'base', 'size', 'touched by', 'accesses'] as h}
              <th class="px-2.5 py-1 font-medium border-b {h === 'accesses' || h === 'size' ? 'text-right' : 'text-left'}"
                  style="color: var(--text-2); border-color: var(--border)">{h}</th>
            {/each}
          </tr>
        </thead>
        <tbody>
          {#each shown as o}
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
              <td class="px-2.5 py-1 border-b font-mono" style="border-color: var(--border)">0x{num(o.base).toString(16)}</td>
              <td class="px-2.5 py-1 text-right border-b whitespace-nowrap" style="border-color: var(--border)">{bytes(num(o.size))}</td>
              <td class="px-2.5 py-1 border-b whitespace-nowrap" style="border-color: var(--border)">
                <span style="color: {o.touched_by ? 'var(--text-1)' : 'var(--text-3)'}">{o.touched_by ? 'GPU ' + o.touched_by : '—'}</span>
              </td>
              <td class="px-2.5 py-1 text-right border-b" style="border-color: var(--border)">{count(num(o.accesses))}</td>
            </tr>
          {/each}
        </tbody>
      </table>
    </div>
    <p class="text-xs mt-2 m-0" style="color: var(--text-3)">
      Tabs are the GPU that <i>owns</i> the memory; <b>touched by</b> is the GPU
      that ran the accessing kernel, and the two differ exactly when the access
      was remote. <code>unknown</code> is device id 65535 — attribution failed,
      which is not the same as host memory and is deliberately not merged with
      GPU 0. Objects with no recorded accesses are listed but not viewable: the
      transport registers buffers the instrumented kernels never touch.
    </p>
  {/if}
</Card>

<Tooltip bind:this={tipRef} {tip} />
