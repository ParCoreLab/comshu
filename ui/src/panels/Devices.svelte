<script>
  import { query, num } from '../lib/query.js';
  import { bytes, count, ramp } from '../lib/viz.js';
  import Card from '../lib/Card.svelte';

  // The device view, after Snoopie's (ICS'24, §Device view). The paper is
  // explicit about how it relates to the object view: "the object view displays
  // objects allocated on a selected GPU and the details regarding remote
  // accesses of these objects by other devices, the device view complements it
  // by instead showing the data transfers performed by the selected GPU
  // itself."
  //
  // So the two views are the same rows read from opposite ends, and the column
  // that separates them is not `rank`. gpu_access carries `run_device` (the GPU
  // that ran the kernel) and `mem_device` (the GPU whose memory was touched).
  // Objects.svelte tabs on mem_device — the GPU as OWNER. This tabs on
  // run_device — the GPU as ACTOR. On the 4-GPU ring capture both are a single
  // rank, which is why keying either on rank would have collapsed the whole
  // thing into one tab and shown the ring as a self-loop.
  //
  // The paper's version also carries source line numbers, in the object table
  // and as a third table. Not here — source_line is empty in every capture we
  // have, and source attribution is out of scope for now.

  const OPS = {
    0: 'load', 1: 'store', 2: 'atomic', 3: 'prefetch',
  };
  const opName = (o) => OPS[num(o)] ?? `op ${num(o)}`;
  const dev = (d) => (num(d) === 65535 ? 'unknown' : `GPU ${d}`);

  let rows = $state(null);       // per (run_device, mem_device, object)
  let ops = $state([]);          // per (run_device, op)
  let error = $state(null);
  let gpu = $state(null);

  $effect(() => {
    query(`
      SELECT g.run_device, g.mem_device, g.object, g.rank,
             count(*)::BIGINT   AS n,
             sum(g.size)::BIGINT AS bytes,
             max(m.range_size)::BIGINT AS size,
             array_to_string(array_agg(DISTINCT
               CASE g.op WHEN 0 THEN 'load' WHEN 1 THEN 'store'
                         WHEN 2 THEN 'atomic' WHEN 3 THEN 'prefetch'
                         ELSE 'op' || g.op END), ', ') AS op_kinds
      FROM gpu_access g
      LEFT JOIN (SELECT DISTINCT rank, id, range_size FROM mem_object) m
        ON m.rank = g.rank AND m.id = g.object
      GROUP BY 1, 2, 3, 4
      ORDER BY n DESC
      LIMIT 500`)
      .then((r) => {
        rows = r;
        // Lowest device, not r[0]: rows are ordered by access count and ties
        // between equally busy GPUs would otherwise open a different tab run
        // to run. A view that opens somewhere else each time is not readable.
        if (gpu === null && r.length)
          gpu = Math.min(...r.map((x) => num(x.run_device)));
      })
      .catch((e) => (error = e.message));
  });

  $effect(() => {
    query(`
      SELECT run_device, op, count(*)::BIGINT AS n, sum(size)::BIGINT AS bytes
      FROM gpu_access GROUP BY 1, 2 ORDER BY 1, n DESC`)
      .then((r) => (ops = r))
      .catch(() => (ops = []));
  });

  const gpus = $derived(
    rows ? [...new Set(rows.map((r) => num(r.run_device)))].sort((a, b) => a - b) : [],
  );
  const shown = $derived(rows ? rows.filter((r) => num(r.run_device) === gpu) : []);
  const shownOps = $derived(ops.filter((o) => num(o.run_device) === gpu));
  const opTotal = $derived(shownOps.reduce((a, o) => a + num(o.n), 0));

  // Local vs remote is run_device === mem_device, and it is the whole point of
  // the view: a kernel touching its own GPU's memory is not communication.
  const remote = $derived(shown.filter((r) => num(r.run_device) !== num(r.mem_device)));
  const remoteN = $derived(remote.reduce((a, r) => a + num(r.n), 0));
  // The far end of the same sequential ramp the heatmaps use — one measure, so
  // one hue, and ramp() already orients it away from the surface in dark mode.
  const barColor = $derived(ramp().at(-1));
  const totalN = $derived(shown.reduce((a, r) => a + num(r.n), 0));
</script>

<Card
  title="Devices"
  subtitle={rows
    ? `what each GPU's kernels touched · ${gpus.length} ${gpus.length === 1 ? 'device' : 'devices'} ran accesses`
    : ''}
>
  {#if error}
    <p class="text-[13px] m-0" style="color: var(--critical)">{error}</p>
  {:else if !rows}
    <p class="text-[13px] m-0" style="color: var(--text-2)">loading…</p>
  {:else if !rows.length}
    <p class="text-[13px] m-0" style="color: var(--text-2)">
      This capture records no GPU accesses. The device view needs the gpumem
      provider — the transport records what it sends, not what a kernel touched.
    </p>
  {:else}
    <div class="flex flex-wrap gap-1 mb-3">
      {#each gpus as g}
        <button
          onclick={() => (gpu = g)}
          class="text-xs rounded px-2.5 py-1 border cursor-pointer"
          style="border-color: {gpu === g ? 'var(--text-3)' : 'var(--border)'};
                 background: {gpu === g ? 'var(--surface-1)' : 'transparent'};
                 color: {gpu === g ? 'var(--text-1)' : 'var(--text-2)'}"
        >{dev(g)}</button>
      {/each}
    </div>

    <p class="text-[13px] mt-0 mb-3">
      Kernels on <b>{dev(gpu)}</b> made {count(totalN)} accesses,
      <b>{count(remoteN)}</b> of them to another GPU's memory
      <span style="color: var(--text-3)">
        ({totalN ? Math.round((remoteN / totalN) * 100) : 0}%)
      </span>
    </p>

    <!-- The object table: what this GPU reached into. -->
    <div class="overflow-x-auto max-h-[42vh] mb-5">
      <table class="w-full text-xs border-collapse">
        <thead>
          <tr>
            {#each ['object', 'memory on', 'rank', 'size', 'ops', 'accesses', 'bytes'] as h}
              <th class="px-2.5 py-1 font-medium border-b {['accesses', 'bytes', 'size'].includes(h) ? 'text-right' : 'text-left'}"
                  style="color: var(--text-2); border-color: var(--border)">{h}</th>
            {/each}
          </tr>
        </thead>
        <tbody>
          {#each shown as r}
            {@const isRemote = num(r.run_device) !== num(r.mem_device)}
            <tr>
              <td class="px-2.5 py-1 border-b font-mono" style="border-color: var(--border)">#{r.object}</td>
              <td class="px-2.5 py-1 border-b whitespace-nowrap" style="border-color: var(--border)">
                <span style="color: {isRemote ? 'var(--text-1)' : 'var(--text-3)'}">{dev(r.mem_device)}</span>
                <span class="ml-1.5 text-[10px] uppercase tracking-wide"
                      style="color: var(--text-3)">{isRemote ? 'remote' : 'local'}</span>
              </td>
              <td class="px-2.5 py-1 border-b" style="border-color: var(--border)">{r.rank}</td>
              <td class="px-2.5 py-1 text-right border-b whitespace-nowrap" style="border-color: var(--border)">
                {r.size === null ? '—' : bytes(num(r.size))}
              </td>
              <td class="px-2.5 py-1 border-b whitespace-nowrap" style="border-color: var(--border)">{r.op_kinds}</td>
              <td class="px-2.5 py-1 text-right border-b" style="border-color: var(--border)">{count(num(r.n))}</td>
              <td class="px-2.5 py-1 text-right border-b whitespace-nowrap" style="border-color: var(--border)">{bytes(num(r.bytes))}</td>
            </tr>
          {/each}
        </tbody>
      </table>
    </div>

    <!-- The instruction table. A bar per row rather than a chart: there are
         four instruction kinds at most, and a proportion is what is being read.
         Width is linear in share, which for four categories is honest. -->
    <h3 class="text-[13px] font-medium mt-0 mb-2">Instruction types</h3>
    <table class="w-full text-xs border-collapse">
      <thead>
        <tr>
          {#each ['type', 'share', 'count', 'bytes'] as h}
            <th class="px-2.5 py-1 font-medium border-b {['count', 'bytes'].includes(h) ? 'text-right' : 'text-left'}"
                style="color: var(--text-2); border-color: var(--border)">{h}</th>
          {/each}
        </tr>
      </thead>
      <tbody>
        {#each shownOps as o}
          <tr>
            <td class="px-2.5 py-1 border-b" style="border-color: var(--border)">{opName(o.op)}</td>
            <td class="px-2.5 py-1 border-b w-1/2" style="border-color: var(--border)">
              <div class="flex items-center gap-2">
                <div class="h-2 rounded-sm"
                     style="width: {opTotal ? (num(o.n) / opTotal) * 100 : 0}%;
                            min-width: 2px; background: {barColor}"></div>
                <span style="color: var(--text-3)">
                  {opTotal ? Math.round((num(o.n) / opTotal) * 100) : 0}%
                </span>
              </div>
            </td>
            <td class="px-2.5 py-1 text-right border-b" style="border-color: var(--border)">{count(num(o.n))}</td>
            <td class="px-2.5 py-1 text-right border-b whitespace-nowrap" style="border-color: var(--border)">{bytes(num(o.bytes))}</td>
          </tr>
        {/each}
      </tbody>
    </table>

    <p class="text-xs mt-3 m-0" style="color: var(--text-3)">
      Tabs are the GPU that <i>ran</i> the kernel; <b>memory on</b> is the GPU
      that owns the bytes. The Objects view is the same data read the other way
      — tabbed by owner, asking who reached in. <code>rank</code> is the process
      that recorded the row, which is not the same as either device: one process
      can drive several GPUs.
    </p>
  {/if}
</Card>
