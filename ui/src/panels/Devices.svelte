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
  // TWO SOURCES, AND THEY ARE NOT THE SAME MEASUREMENT.
  //
  // This panel used to read `gpu_access` and nothing else, which meant it was
  // blank on every IB capture in the corpus — including the only ones that
  // exist above 8 ranks. That was a limit of the PANEL, not of the data: a
  // verbs capture does know which memory moved, because `object_access` maps
  // `ib_post_send.remote_addr` back to the innermost containing object, exactly
  // and per send. What a work request cannot tell you is which GPU ran the code
  // that caused it — so the two sources answer the actor question differently
  // and are faceted, never summed:
  //
  //   nvlink  actor = run_device, OBSERVED. NVBit saw the instruction execute
  //           on that SM. Carries op kind and 4-byte granularity.
  //   verbs   actor = the posting rank's GPU, ATTRIBUTED via _rank_device.
  //           It is the GPU whose memory the transfer came from, inferred from
  //           which device that rank owns — not from watching a kernel run.
  //           Carries exact addresses and message lengths instead.
  //
  // The distinction is stated in the caption too, because "GPU 2 moved 40 MB"
  // reads identically in both modes and is a weaker claim in one of them.
  //
  // IDENTITY IS (node, device), NEVER device ALONE. Across 16 nodes there are
  // sixteen "GPU 0"s. Tabbing on the bare ordinal would merge them into one and
  // report a 64-rank job as a 4-GPU one — the same collision class that
  // produced impossible self-edges when comm_edges keyed peers on qp_num
  // without the lid. The node index is `dense_rank()` over the node hash, which
  // is the same ordering Graph.svelte gives its arcs, so n4 means one host in
  // both panels.

  const OPS = { 0: 'load', 1: 'store', 2: 'atomic', 3: 'prefetch' };
  const opName = (o) => OPS[num(o)] ?? `op ${num(o)}`;
  const dev = (d) => (num(d) === 65535 ? 'unknown' : `GPU ${d}`);

  // (node, device) -> the label a tab wears. NULL device is not hidden: it is
  // what _rank_device reports when one rank drives more than one GPU, and that
  // is a real state the capture can be in.
  const actorKey = (nodeIx, device) => `${nodeIx ?? '-'}:${device ?? '?'}`;
  const actorLabel = (nodeIx, device) =>
    (nodeIx === null || nodeIx === undefined ? '' : `n${nodeIx} · `) +
    (device === null || device === undefined ? 'GPU ?' : dev(device));

  let view = $state(null);        // 'nvlink' | 'verbs'
  let error = $state(null);

  // --- nvlink (gpu_access) --------------------------------------------------
  let nvRows = $state(null);
  let nvOps = $state([]);
  let nvGpu = $state(null);

  // --- verbs (object_access) ------------------------------------------------
  let vbRows = $state(null);
  let vbSizes = $state([]);
  let vbActor = $state(null);

  // Node and device identity for a rank, shared by both verbs queries.
  //
  // This used to compute `dense_rank()` over `_node` and `_rank_device` right
  // here, which made it the private owner of an identity three consumers want:
  // this panel, Graph.svelte's arc ordering, and the chat agent — which could
  // not have it at all, because the views the recipe needs are internal ones it
  // is told never to read. It is `rank_identity` in the derived layer now, so
  // "n14 · GPU 0" means the same thing in a panel and in an answer.
  const ND = `
    WITH nd AS (SELECT rank, node_ix, device, devices FROM rank_identity)`;

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
        nvRows = r;
        if (nvGpu === null && r.length)
          nvGpu = Math.min(...r.map((x) => num(x.run_device)));
      })
      .catch((e) => (nvRows = []));
  });

  $effect(() => {
    query(`
      SELECT run_device, op, count(*)::BIGINT AS n, sum(size)::BIGINT AS bytes
      FROM gpu_access GROUP BY 1, 2 ORDER BY 1, n DESC`)
      .then((r) => (nvOps = r))
      .catch(() => (nvOps = []));
  });

  $effect(() => {
    query(`${ND}
      SELECT an.node_ix AS actor_node, an.device AS actor_device,
             an.devices  AS actor_devices,
             ow.node_ix AS owner_node, ow.device AS owner_device,
             a.actor_rank, a.owner_rank, a.object, a.source,
             count(*)::BIGINT    AS n,
             sum(a.size)::BIGINT AS bytes,
             max(m.range_size)::BIGINT AS size
      FROM object_access a
      LEFT JOIN nd an ON an.rank = a.actor_rank
      LEFT JOIN nd ow ON ow.rank = a.owner_rank
      LEFT JOIN (SELECT DISTINCT rank, id, range_size FROM mem_object) m
        ON m.rank = a.owner_rank AND m.id = a.object
      -- NO LIMIT, deliberately. This query spans every actor and the panel then
      -- filters to one tab, so a global ORDER BY n DESC with a LIMIT does not
      -- shorten each tab a little — it drops the QUIETEST actors' rows outright
      -- and says nothing, which is the failure the matrix keeps its unresolved column to
      -- avoid. The 64-rank capture already uses 1,015 groups against the 2,000
      -- the limit used to impose, so the next scale step would have started
      -- silently lying.
      --
      -- The real ceiling is now the server's own kRowCap (100,000 rows), which
      -- is two orders of magnitude above what this capture needs and applies to
      -- every query alike. If a capture ever approaches it, the fix is to make
      -- this query per-actor rather than to reinstate a cut here.
      --
      -- BOTH verbs branches. They are the same quantity — bytes this actor put
      -- into that object — differing only in which end of the transfer knew the
      -- address, so unlike nvlink-vs-verbs they ARE addable. Keeping
      -- verbs_recv out was what made this panel report 4 peers for a rank that
      -- comm_edges credits with 13: the missing 9 were all two-sided.
      WHERE a.source IN ('verbs', 'verbs_recv')
      GROUP BY 1, 2, 3, 4, 5, 6, 7, 8, 9
      ORDER BY n DESC`)
      .then((r) => {
        vbRows = r;
        if (vbActor === null && r.length) {
          const keys = r.map((x) => [num(x.actor_node) ?? 0, num(x.actor_device) ?? 0, x]);
          keys.sort((a, b) => a[0] - b[0] || a[1] - b[1]);
          vbActor = actorKey(num(keys[0][2].actor_node), num(keys[0][2].actor_device));
        }
      })
      .catch((e) => { vbRows = []; error = e.message; });
  });

  // Transfer sizes, the verbs answer to the instruction-type table. Same size
  // classes the flow panel uses, so one vocabulary describes a message
  // everywhere in the tool.
  $effect(() => {
    query(`${ND}
      SELECT an.node_ix AS actor_node, an.device AS actor_device,
             CASE WHEN a.size = 0       THEN 'empty'
                  WHEN a.size < 1024    THEN '<1 KB'
                  WHEN a.size < 65536   THEN '1–64 KB'
                  WHEN a.size < 1048576 THEN '64 KB–1 MB'
                  ELSE '≥1 MB' END      AS size_class,
             count(*)::BIGINT    AS n,
             sum(a.size)::BIGINT AS bytes
      FROM object_access a
      LEFT JOIN nd an ON an.rank = a.actor_rank
      WHERE a.source IN ('verbs', 'verbs_recv')
      GROUP BY 1, 2, 3`)
      .then((r) => (vbSizes = r))
      .catch(() => (vbSizes = []));
  });

  const SIZE_ORDER = ['empty', '<1 KB', '1–64 KB', '64 KB–1 MB', '≥1 MB'];

  const have = $derived({
    nvlink: (nvRows?.length ?? 0) > 0,
    verbs: (vbRows?.length ?? 0) > 0,
  });
  const loading = $derived(nvRows === null || vbRows === null);
  const sources = $derived(
    [have.nvlink ? 'nvlink' : null, have.verbs ? 'verbs' : null].filter(Boolean),
  );

  // --- nvlink derivations (unchanged behaviour) -----------------------------
  const gpus = $derived(
    nvRows ? [...new Set(nvRows.map((r) => num(r.run_device)))].sort((a, b) => a - b) : [],
  );
  const nvShown = $derived(nvRows ? nvRows.filter((r) => num(r.run_device) === nvGpu) : []);
  const nvShownOps = $derived(nvOps.filter((o) => num(o.run_device) === nvGpu));
  const nvOpTotal = $derived(nvShownOps.reduce((a, o) => a + num(o.n), 0));
  const nvRemote = $derived(nvShown.filter((r) => num(r.run_device) !== num(r.mem_device)));
  const nvRemoteN = $derived(nvRemote.reduce((a, r) => a + num(r.n), 0));
  const nvTotalN = $derived(nvShown.reduce((a, r) => a + num(r.n), 0));

  // --- verbs derivations ----------------------------------------------------
  const actors = $derived.by(() => {
    const m = new Map();
    for (const r of vbRows ?? []) {
      const k = actorKey(num(r.actor_node), num(r.actor_device));
      if (!m.has(k))
        m.set(k, { k, node: num(r.actor_node), device: num(r.actor_device),
                   devices: num(r.actor_devices) });
    }
    return [...m.values()].sort((a, b) => (a.node ?? 0) - (b.node ?? 0) ||
                                          (a.device ?? 0) - (b.device ?? 0));
  });
  const vbShown = $derived(
    (vbRows ?? []).filter((r) => actorKey(num(r.actor_node), num(r.actor_device)) === vbActor),
  );
  const vbShownSizes = $derived(
    vbSizes
      .filter((s) => actorKey(num(s.actor_node), num(s.actor_device)) === vbActor)
      .sort((a, b) => SIZE_ORDER.indexOf(a.size_class) - SIZE_ORDER.indexOf(b.size_class)),
  );
  const vbSizeTotal = $derived(vbShownSizes.reduce((a, s) => a + num(s.n), 0));

  /**
   * Three reach classes, not two.
   *
   * A binary local/remote is the wrong split for this corpus. The 64-rank
   * capture runs four ranks per node with P2P disabled precisely so that
   * co-resident peers use the NIC as well — so "same host, different GPU" is
   * the class the run exists to produce, and folding it into "remote" hides it.
   */
  function reach(r) {
    const an = num(r.actor_node), ad = num(r.actor_device);
    const on = num(r.owner_node), od = num(r.owner_device);
    if (an === on && ad === od) return 'same GPU';
    if (an === on) return 'same host';
    return 'other host';
  }
  const REACH = ['same GPU', 'same host', 'other host'];
  const vbReach = $derived.by(() => {
    const t = new Map(REACH.map((k) => [k, { n: 0, bytes: 0 }]));
    for (const r of vbShown) {
      const c = t.get(reach(r));
      c.n += num(r.n);
      c.bytes += num(r.bytes);
    }
    return t;
  });
  const vbTotalN = $derived(vbShown.reduce((a, r) => a + num(r.n), 0));
  const vbTotalB = $derived(vbShown.reduce((a, r) => a + num(r.bytes), 0));
  const multiDevice = $derived(actors.some((a) => (a.devices ?? 1) > 1));

  // The opening source is decided ONCE, after both queries have settled, and it
  // prefers nvlink. Letting whichever query returned first set it made a
  // capture with both providers open on a different view run to run — the same
  // instability the GPU tabs avoid by picking the lowest device rather than the
  // busiest row. nvlink wins the tie because it is the stronger claim: an
  // observed instruction beats an attributed one.
  $effect(() => {
    if (view === null && !loading)
      view = have.nvlink ? 'nvlink' : have.verbs ? 'verbs' : null;
  });

  const barColor = $derived(ramp().at(-1));

  /** Reach, said as a sentence rather than as a bare class name. */
  const REACH_PHRASE = {
    'same GPU': 'to its own memory',
    'same host': 'to another GPU on the same host',
    'other host': 'to another host',
  };
  const reachParts = $derived(
    REACH.filter((k) => vbReach.get(k).n > 0)
      .map((k) => ({ k, n: vbReach.get(k).n, phrase: REACH_PHRASE[k] })),
  );

  const subtitle = $derived(
    loading
      ? ''
      : view === 'nvlink'
        ? `what each GPU's kernels touched · ${gpus.length} ${gpus.length === 1 ? 'device' : 'devices'} ran accesses`
        : view === 'verbs'
          // "that moved anything" is not padding: only ranks that posted a send
          // appear here, so this counts the devices with NIC traffic, not the
          // devices in the job.
          ? `what each GPU's rank moved over the NIC · ${actors.length} devices on ${
              new Set(actors.map((a) => a.node)).size} hosts moved anything`
          : '',
  );
</script>

<Card title="Devices" subtitle={subtitle}>
  {#if loading}
    <p class="text-[13px] m-0" style="color: var(--text-2)">loading…</p>
  {:else if !sources.length}
    <p class="text-[13px] m-0" style="color: var(--text-2)">
      This capture records neither GPU accesses nor object-resolved sends. The
      device view needs the gpumem provider, or an ibverbs capture whose sends
      resolve to registered objects.
    </p>
    {#if error}
      <p class="text-xs mt-1 m-0" style="color: var(--text-3)">{error}</p>
    {/if}
  {:else}
    {#if sources.length > 1}
      <div class="flex flex-wrap items-center gap-1 mb-3">
        {#each sources as s}
          <button
            onclick={() => (view = s)}
            class="text-xs rounded px-2.5 py-1 border cursor-pointer"
            style="border-color: {view === s ? 'var(--text-3)' : 'var(--border)'};
                   background: {view === s ? 'var(--surface-0)' : 'transparent'};
                   color: {view === s ? 'var(--text-1)' : 'var(--text-2)'}"
          >{s}</button>
        {/each}
        <span class="text-xs self-center ml-2" style="color: var(--text-3)">
          different measurements — one is observed execution, the other attributed
        </span>
      </div>
    {/if}

    {#if view === 'nvlink'}
      <div class="flex flex-wrap gap-1 mb-3">
        {#each gpus as g}
          <button
            onclick={() => (nvGpu = g)}
            class="text-xs rounded px-2.5 py-1 border cursor-pointer"
            style="border-color: {nvGpu === g ? 'var(--text-3)' : 'var(--border)'};
                   background: {nvGpu === g ? 'var(--surface-1)' : 'transparent'};
                   color: {nvGpu === g ? 'var(--text-1)' : 'var(--text-2)'}"
          >{dev(g)}</button>
        {/each}
      </div>

      <p class="text-[13px] mt-0 mb-3">
        Kernels on <b>{dev(nvGpu)}</b> made {count(nvTotalN)} accesses,
        <b>{count(nvRemoteN)}</b> of them to another GPU's memory
        <span style="color: var(--text-3)">
          ({nvTotalN ? Math.round((nvRemoteN / nvTotalN) * 100) : 0}%)
        </span>
      </p>

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
            {#each nvShown as r}
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
          {#each nvShownOps as o}
            <tr>
              <td class="px-2.5 py-1 border-b" style="border-color: var(--border)">{opName(o.op)}</td>
              <td class="px-2.5 py-1 border-b w-1/2" style="border-color: var(--border)">
                <div class="flex items-center gap-2">
                  <div class="h-2 rounded-sm"
                       style="width: {nvOpTotal ? (num(o.n) / nvOpTotal) * 100 : 0}%;
                              min-width: 2px; background: {barColor}"></div>
                  <span style="color: var(--text-3)">
                    {nvOpTotal ? Math.round((num(o.n) / nvOpTotal) * 100) : 0}%
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

    {:else if view === 'verbs'}
      <div class="flex flex-wrap gap-1 mb-3">
        {#each actors as a}
          <button
            onclick={() => (vbActor = a.k)}
            class="text-xs rounded px-2.5 py-1 border cursor-pointer whitespace-nowrap"
            style="border-color: {vbActor === a.k ? 'var(--text-3)' : 'var(--border)'};
                   background: {vbActor === a.k ? 'var(--surface-1)' : 'transparent'};
                   color: {vbActor === a.k ? 'var(--text-1)' : 'var(--text-2)'}"
          >{actorLabel(a.node, a.device)}</button>
        {/each}
      </div>

      <p class="text-[13px] mt-0 mb-3">
        <b>{actorLabel(actors.find((a) => a.k === vbActor)?.node,
                       actors.find((a) => a.k === vbActor)?.device)}</b>
        moved {bytes(vbTotalB)} in {count(vbTotalN)} messages —
        {#each reachParts as p, i}<b>{count(p.n)}</b> {p.phrase}{i <
          reachParts.length - 1 ? ', ' : ''}{/each}
      </p>

      <div class="overflow-x-auto max-h-[42vh] mb-5">
        <table class="w-full text-xs border-collapse">
          <thead>
            <tr>
              {#each ['object', 'memory on', 'owner rank', 'size', 'reach', 'via', 'messages', 'bytes'] as h}
                <th class="px-2.5 py-1 font-medium border-b {['messages', 'bytes', 'size'].includes(h) ? 'text-right' : 'text-left'}"
                    style="color: var(--text-2); border-color: var(--border)">{h}</th>
              {/each}
            </tr>
          </thead>
          <tbody>
            {#each vbShown as r}
              {@const c = reach(r)}
              <tr>
                <td class="px-2.5 py-1 border-b font-mono" style="border-color: var(--border)">#{r.object}</td>
                <td class="px-2.5 py-1 border-b whitespace-nowrap" style="border-color: var(--border)">
                  {actorLabel(num(r.owner_node), num(r.owner_device))}
                </td>
                <td class="px-2.5 py-1 border-b" style="border-color: var(--border)">{r.owner_rank}</td>
                <td class="px-2.5 py-1 text-right border-b whitespace-nowrap" style="border-color: var(--border)">
                  {r.size === null ? '—' : bytes(num(r.size))}
                </td>
                <td class="px-2.5 py-1 border-b whitespace-nowrap text-[10px] uppercase tracking-wide"
                    style="border-color: var(--border); color: {c === 'other host' ? 'var(--text-1)' : 'var(--text-3)'}">{c}</td>
                <!-- How the address was learned, not what the app called it: an
                     RDMA write names its target, a two-sided send does not and
                     was recovered from the receiver's posted buffer. -->
                <td class="px-2.5 py-1 border-b whitespace-nowrap text-[10px] uppercase tracking-wide"
                    style="border-color: var(--border); color: var(--text-3)"
                    title={r.source === 'verbs_recv'
                      ? 'two-sided send, address recovered from the receiver'
                      : 'RDMA write, address named by the sender'}
                >{r.source === 'verbs_recv' ? 'send' : 'write'}</td>
                <td class="px-2.5 py-1 text-right border-b" style="border-color: var(--border)">{count(num(r.n))}</td>
                <td class="px-2.5 py-1 text-right border-b whitespace-nowrap" style="border-color: var(--border)">{bytes(num(r.bytes))}</td>
              </tr>
            {/each}
          </tbody>
        </table>
      </div>

      <h3 class="text-[13px] font-medium mt-0 mb-2">Transfer sizes</h3>
      <table class="w-full text-xs border-collapse">
        <thead>
          <tr>
            {#each ['class', 'share', 'messages', 'bytes'] as h}
              <th class="px-2.5 py-1 font-medium border-b {['messages', 'bytes'].includes(h) ? 'text-right' : 'text-left'}"
                  style="color: var(--text-2); border-color: var(--border)">{h}</th>
            {/each}
          </tr>
        </thead>
        <tbody>
          {#each vbShownSizes as s}
            <tr>
              <td class="px-2.5 py-1 border-b" style="border-color: var(--border)">{s.size_class}</td>
              <td class="px-2.5 py-1 border-b w-1/2" style="border-color: var(--border)">
                <div class="flex items-center gap-2">
                  <div class="h-2 rounded-sm"
                       style="width: {vbSizeTotal ? (num(s.n) / vbSizeTotal) * 100 : 0}%;
                              min-width: 2px; background: {barColor}"></div>
                  <span style="color: var(--text-3)">
                    {vbSizeTotal ? Math.round((num(s.n) / vbSizeTotal) * 100) : 0}%
                  </span>
                </div>
              </td>
              <td class="px-2.5 py-1 text-right border-b" style="border-color: var(--border)">{count(num(s.n))}</td>
              <td class="px-2.5 py-1 text-right border-b whitespace-nowrap" style="border-color: var(--border)">{bytes(num(s.bytes))}</td>
            </tr>
          {/each}
        </tbody>
      </table>

      {#if multiDevice}
        <p class="text-xs mt-3 m-0" style="color: var(--critical)">
          At least one rank drives more than one GPU, so its device cannot be
          named from <code>_rank_device</code> — those rows are tabbed as
          <b>GPU ?</b> rather than assigned to a guess.
        </p>
      {/if}

      <p class="text-xs mt-3 m-0" style="color: var(--text-3)">
        Tabs are <b>(host, GPU)</b>, never the bare ordinal — across hosts there
        is more than one “GPU 0”. The actor device here is <i>attributed</i>: it
        is the GPU the posting rank owns, not a device observed executing an
        instruction, because a work request does not name a kernel. The target,
        by contrast, is exact — <code>remote_addr</code> resolved to the
        innermost containing object. <b>reach</b> separates same-host traffic
        from cross-host, which on a run with P2P disabled is the difference
        worth seeing. <b>via</b> says how the address was learned: a
        <code>write</code> named its own target, a <code>send</code> named none
        and was recovered from the buffer the receiver posted, so a two-sided
        transfer is attributed from the far end. When that recovery is
        uncertain, <code>link_health</code> reports it in the banner above
        rather than here.
      </p>
    {/if}
  {/if}
</Card>
