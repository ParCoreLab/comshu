<script>
  import { query, num } from '../lib/query.js';
  import { bytes, count, logScale, ramp } from '../lib/viz.js';
  import Card from '../lib/Card.svelte';
  import Tooltip from '../lib/Tooltip.svelte';

  // The object view, after Snoopie's (ICS'24, §Object view): each object's
  // address space as a 1D heatmap of operations per address, with the
  // instruction types and the participating remote GPUs on hover.
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
  $effect(() => {
    query(`
      SELECT m.rank, m.id, m.range_base AS base, m.range_size AS size, m.device,
             coalesce(a.n, 0) AS accesses
      FROM (SELECT DISTINCT rank, id, range_base, range_size, device FROM mem_object) m
      LEFT JOIN (SELECT rank, object, count(*) AS n FROM gpu_access GROUP BY 1, 2) a
        ON a.rank = m.rank AND a.object = m.id
      ORDER BY accesses DESC, size DESC, rank
      LIMIT 500`)
      .then((r) => {
        objects = r;
        if (!sel) sel = r.find((o) => num(o.accesses) > 0) ?? null;
      })
      .catch((e) => (error = e.message));
  });

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
    tip = {
      title: `offset ${bytes(num(b.off_lo))} – ${bytes(num(b.off_hi))}`,
      body: `${count(num(b.n))} ${b.ops} · ${bytes(num(b.bytes))} · peer ${b.peers}`,
    };
    tipRef?.move(ev);
  }
</script>

<Card
  title="Objects"
  subtitle={objects ? `${objects.length} objects · ${objects.filter((o) => num(o.accesses) > 0).length} with recorded accesses` : ''}
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
            {#each ['', 'rank', 'device', 'base', 'size', 'accesses'] as h}
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
              <td class="px-2.5 py-1 text-right border-b" style="border-color: var(--border)">{count(num(o.accesses))}</td>
            </tr>
          {/each}
        </tbody>
      </table>
    </div>
    <p class="text-xs mt-2 m-0" style="color: var(--text-3)">
      <code>unknown</code> is device id 65535 — attribution failed, which is not
      the same as host memory and is deliberately not merged with GPU 0. Objects
      with no recorded accesses are listed but not viewable: the transport
      registers buffers the instrumented kernels never touch.
    </p>
  {/if}
</Card>

<Tooltip bind:this={tipRef} {tip} />
