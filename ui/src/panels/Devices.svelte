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
  //   devx    actor = the GPU that owns the sending queue (mlx5_devx_qp.device),
  //           per send: the GPU built the WQE, a doorbell decoded it. Same
  //           exact addresses and lengths as verbs, and addable to it.
  //   ucx     actor = the sending rank's GPU, ATTRIBUTED via rank_identity, and
  //           the peer is the rank at the far end of the UCP endpoint
  //           (_ucx_peer_of_ep), its GPU attributed the same way. What IS
  //           observed is where each send's buffer lived (ucx_tag_send.device
  //           and its siblings): a GPU-aware MPI hands device memory straight
  //           to UCX, and 65535 there means host memory, not "unknown GPU".
  //           No object resolution and no bytes — UCP counts DATATYPE UNITS,
  //           which are bytes only when the caller hands UCP an 8-bit
  //           datatype and nothing in the record says whether it did. (On
  //           LAMMPS r2 it did: 569,125,025 units sent against 568,519,070
  //           bytes UCT read back — so the check is transport_mix, per
  //           capture, not an assumption either way.) This source therefore
  //           never shares a column with verbs: a third tab, not an addend.
  //
  //           Without it, every `-p ucx` capture — LAMMPS, QE, GROMACS, i.e.
  //           every MPI science app in the corpus — opened this panel blank
  //           while the capture plainly knew which GPU sent what to whom.
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
  // `devices === 0` is a rank with no GPU evidence at all — a host-only MPI
  // rank on a ucx capture. That is not the multi-GPU 'GPU ?' and must not
  // read like it.
  const actorLabel = (nodeIx, device, devices) =>
    (nodeIx === null || nodeIx === undefined ? '' : `n${nodeIx} · `) +
    (device === null || device === undefined
      ? (devices === 0 ? 'host' : 'GPU ?')
      : dev(device));

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
      SELECT an.node_ix AS actor_node,
             -- A devx row knows its GPU per send (the queue's device), which is
             -- an observation; every other source falls back to the rank's GPU.
             CASE WHEN a.source = 'devx' AND a.actor_device IS NOT NULL
                  THEN a.actor_device ELSE an.device END AS actor_device,
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
      --
      -- And devx, the GPU-initiated sends a doorbell decoded: the same quantity
      -- again, with the address named by a WQE the GPU wrote rather than by an
      -- ibv_post_send. Disjoint from verbs by construction (a send takes one
      -- door), so still addable.
      WHERE a.source IN ('verbs', 'verbs_recv', 'devx')
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
      SELECT an.node_ix AS actor_node,
             -- A devx row knows its GPU per send (the queue's device), which is
             -- an observation; every other source falls back to the rank's GPU.
             CASE WHEN a.source = 'devx' AND a.actor_device IS NOT NULL
                  THEN a.actor_device ELSE an.device END AS actor_device,
             CASE WHEN a.size = 0       THEN 'empty'
                  WHEN a.size < 1024    THEN '<1 KB'
                  WHEN a.size < 65536   THEN '1–64 KB'
                  WHEN a.size < 1048576 THEN '64 KB–1 MB'
                  ELSE '≥1 MB' END      AS size_class,
             count(*)::BIGINT    AS n,
             sum(a.size)::BIGINT AS bytes
      FROM object_access a
      LEFT JOIN nd an ON an.rank = a.actor_rank
      WHERE a.source IN ('verbs', 'verbs_recv', 'devx')
      GROUP BY 1, 2, 3`)
      .then((r) => (vbSizes = r))
      .catch(() => (vbSizes = []));
  });

  const SIZE_ORDER = ['empty', '<1 KB', '1–64 KB', '64 KB–1 MB', '≥1 MB'];

  // --- ucx (UCP sends, peer by endpoint) ---------------------------------------
  //
  // One row per (actor, peer, buffer residency, door). The three UCP doors are
  // the ones _ucx_sends feeds comm_edges from, with the same status filter, so
  // this panel's totals and the matrix's ucx lane are the same sum cut another
  // way. A get is kept and labelled rather than swapped: the tab is the GPU
  // whose rank ISSUED the operation, and `via` says the bytes came back.
  let ucRows = $state(null);
  let ucSizes = $state([]);
  let ucActor = $state(null);

  const UCX_OPS = `
    ops AS (
      SELECT rank, endpoint, device, 'send' AS via, "count" AS units
      FROM ucx_tag_send WHERE status_class <> 1
      UNION ALL
      SELECT rank, endpoint, device, 'am', "count"
      FROM ucx_am_send  WHERE status_class <> 1
      UNION ALL
      SELECT rank, endpoint, device, CASE op WHEN 0 THEN 'put' ELSE 'get' END, "count"
      FROM ucx_rma      WHERE status_class <> 1
    )`;

  $effect(() => {
    query(`${ND}, ${UCX_OPS}
      SELECT an.node_ix AS actor_node, an.device AS actor_device,
             an.devices AS actor_devices,
             o.rank AS actor_rank, p.peer_rank AS owner_rank,
             ow.node_ix AS owner_node, ow.device AS owner_device,
             ow.devices AS owner_devices,
             o.device AS buf_device, o.via,
             count(*)::BIGINT       AS n,
             sum(o.units)::BIGINT   AS units
      FROM ops o
      LEFT JOIN _ucx_peer_of_ep p ON p.rank = o.rank AND p.endpoint = o.endpoint
      LEFT JOIN nd an ON an.rank = o.rank
      LEFT JOIN nd ow ON ow.rank = p.peer_rank
      GROUP BY ALL
      ORDER BY units DESC, n DESC`)
      .then((r) => {
        ucRows = r;
        if (ucActor === null && r.length) {
          const keys = r.map((x) => [num(x.actor_node) ?? 0, num(x.actor_device) ?? 0, x]);
          keys.sort((a, b) => a[0] - b[0] || a[1] - b[1]);
          ucActor = actorKey(num(keys[0][2].actor_node), num(keys[0][2].actor_device));
        }
      })
      // A capture without the ucx provider has no ucx_* tables at all; that is
      // "this source is absent", not an error worth surfacing.
      .catch(() => (ucRows = []));
  });

  $effect(() => {
    query(`${ND}, ${UCX_OPS}
      SELECT an.node_ix AS actor_node, an.device AS actor_device,
             CASE WHEN o.units = 0         THEN 'empty'
                  WHEN o.units < 1024      THEN '<1 K'
                  WHEN o.units < 65536     THEN '1–64 K'
                  WHEN o.units < 1048576   THEN '64 K–1 M'
                  ELSE '≥1 M' END          AS size_class,
             count(*)::BIGINT     AS n,
             sum(o.units)::BIGINT AS units
      FROM ops o
      LEFT JOIN nd an ON an.rank = o.rank
      GROUP BY 1, 2, 3`)
      .then((r) => (ucSizes = r))
      .catch(() => (ucSizes = []));
  });

  const UC_SIZE_ORDER = ['empty', '<1 K', '1–64 K', '64 K–1 M', '≥1 M'];

  const have = $derived({
    nvlink: (nvRows?.length ?? 0) > 0,
    verbs: (vbRows?.length ?? 0) > 0,
    ucx: (ucRows?.length ?? 0) > 0,
  });
  const loading = $derived(nvRows === null || vbRows === null || ucRows === null);
  const sources = $derived(
    ['nvlink', 'verbs', 'ucx'].filter((s) => have[s]),
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
    // A UCX endpoint whose peer prefix did not resolve to exactly one rank.
    // Counted under its own class rather than guessed into one of the three —
    // the same refusal comm_edges makes with dst_rank = -1.
    if (r.owner_rank === null || r.owner_rank === undefined) return 'unresolved';
    const an = num(r.actor_node), ad = num(r.actor_device);
    const on = num(r.owner_node), od = num(r.owner_device);
    // Two ranks whose GPUs are both unknown are NOT "same GPU" — null === null
    // is an absence matching an absence. With no device on either end the only
    // claims available are the rank and the host.
    if (ad === null || od === null) {
      if (num(r.actor_rank) === num(r.owner_rank)) return 'same rank';
      return an === on ? 'same host' : 'other host';
    }
    if (an === on && ad === od) return 'same GPU';
    if (an === on) return 'same host';
    return 'other host';
  }
  const REACH = ['same rank', 'same GPU', 'same host', 'other host', 'unresolved'];
  const reachTally = (rows) => {
    const t = new Map(REACH.map((k) => [k, { n: 0, bytes: 0 }]));
    for (const r of rows) {
      const c = t.get(reach(r));
      c.n += num(r.n);
      c.bytes += num(r.bytes ?? r.units);
    }
    return t;
  };
  const vbReach = $derived(reachTally(vbShown));
  const vbTotalN = $derived(vbShown.reduce((a, r) => a + num(r.n), 0));
  const vbTotalB = $derived(vbShown.reduce((a, r) => a + num(r.bytes), 0));

  // --- ucx derivations --------------------------------------------------------
  const ucActors = $derived.by(() => {
    const m = new Map();
    for (const r of ucRows ?? []) {
      const k = actorKey(num(r.actor_node), num(r.actor_device));
      if (!m.has(k))
        m.set(k, { k, node: num(r.actor_node), device: num(r.actor_device),
                   devices: num(r.actor_devices) });
    }
    return [...m.values()].sort((a, b) => (a.node ?? 0) - (b.node ?? 0) ||
                                          (a.device ?? 0) - (b.device ?? 0));
  });
  const ucShown = $derived(
    (ucRows ?? []).filter((r) => actorKey(num(r.actor_node), num(r.actor_device)) === ucActor),
  );
  const ucShownSizes = $derived(
    ucSizes
      .filter((s) => actorKey(num(s.actor_node), num(s.actor_device)) === ucActor)
      .sort((a, b) => UC_SIZE_ORDER.indexOf(a.size_class) - UC_SIZE_ORDER.indexOf(b.size_class)),
  );
  const ucSizeTotal = $derived(ucShownSizes.reduce((a, s) => a + num(s.n), 0));
  const ucReach = $derived(reachTally(ucShown));
  const ucTotalN = $derived(ucShown.reduce((a, r) => a + num(r.n), 0));
  const ucTotalU = $derived(ucShown.reduce((a, r) => a + num(r.units), 0));
  // Sends whose buffer lived in host memory. On a GPU-aware MPI these are the
  // small control messages; on a non-GPU-aware one they are everything, and
  // the tab's GPU then says only which device the rank drives.
  const ucHostN = $derived(
    ucShown.filter((r) => num(r.buf_device) === 65535).reduce((a, r) => a + num(r.n), 0),
  );
  const ucReachParts = $derived(
    REACH.filter((k) => ucReach.get(k).n > 0)
      .map((k) => ({ k, n: ucReach.get(k).n, phrase: REACH_PHRASE[k] })),
  );
  const ucActorLabel = $derived.by(() => {
    const a = ucActors.find((x) => x.k === ucActor);
    return actorLabel(a?.node, a?.device, a?.devices);
  });

  const multiDevice = $derived(
    (view === 'ucx' ? ucActors : actors).some((a) => (a.devices ?? 1) > 1),
  );

  // The opening source is decided ONCE, after both queries have settled, and it
  // prefers nvlink. Letting whichever query returned first set it made a
  // capture with both providers open on a different view run to run — the same
  // instability the GPU tabs avoid by picking the lowest device rather than the
  // busiest row. nvlink wins the tie because it is the stronger claim: an
  // observed instruction beats an attributed one.
  $effect(() => {
    if (view === null && !loading)
      view = sources[0] ?? null;
  });

  const barColor = $derived(ramp().at(-1));

  /** Reach, said as a sentence rather than as a bare class name. */
  const REACH_PHRASE = {
    'same rank': 'to itself',
    'same GPU': 'to its own memory',
    'same host': 'elsewhere on the same host',
    'other host': 'to another host',
    'unresolved': 'to a peer the capture could not name',
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
          : view === 'ucx'
            ? `what each GPU's rank sent through UCX · ${ucActors.length} ${
                ucActors.every((a) => a.devices === 0) ? 'host-only ranks' : 'devices'} on ${
                new Set(ucActors.map((a) => a.node)).size} hosts sent anything`
            : '',
  );
</script>

<Card title="Devices" subtitle={subtitle}>
  {#if loading}
    <p class="text-[13px] m-0" style="color: var(--text-2)">loading…</p>
  {:else if !sources.length}
    <p class="text-[13px] m-0" style="color: var(--text-2)">
      This capture records no GPU accesses, no object-resolved sends and no UCX
      sends. The device view needs the gpumem provider, an ibverbs capture
      whose sends resolve to registered objects, or the ucx provider.
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
          different measurements — never add one tab to another
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
          >{actorLabel(a.node, a.device, a.devices)}</button>
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
                      : r.source === 'devx'
                        ? 'GPU-initiated (IBGDA / NCCL GIN), address named by a WQE the GPU built'
                        : 'RDMA write, address named by the sender'}
                >{r.source === 'verbs_recv' ? 'send' : r.source === 'devx' ? 'gpu put' : 'write'}</td>
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

    {:else if view === 'ucx'}
      <div class="flex flex-wrap gap-1 mb-3">
        {#each ucActors as a}
          <button
            onclick={() => (ucActor = a.k)}
            class="text-xs rounded px-2.5 py-1 border cursor-pointer whitespace-nowrap"
            style="border-color: {ucActor === a.k ? 'var(--text-3)' : 'var(--border)'};
                   background: {ucActor === a.k ? 'var(--surface-1)' : 'transparent'};
                   color: {ucActor === a.k ? 'var(--text-1)' : 'var(--text-2)'}"
          >{actorLabel(a.node, a.device, a.devices)}</button>
        {/each}
      </div>

      <p class="text-[13px] mt-0 mb-3">
        <b>{ucActorLabel}</b> sent {count(ucTotalN)} messages
        ({count(ucTotalU)} units) —
        {#each ucReachParts as p, i}<b>{count(p.n)}</b> {p.phrase}{i <
          ucReachParts.length - 1 ? ', ' : ''}{/each}
        {#if ucHostN}
          <span style="color: var(--text-3)">
            · {count(ucHostN)} from host memory
          </span>
        {/if}
      </p>

      <div class="overflow-x-auto max-h-[42vh] mb-5">
        <table class="w-full text-xs border-collapse">
          <thead>
            <tr>
              {#each ['peer', 'peer rank', 'from rank', 'buffer on', 'reach', 'via', 'messages', 'units'] as h}
                <th class="px-2.5 py-1 font-medium border-b {['messages', 'units'].includes(h) ? 'text-right' : 'text-left'}"
                    style="color: var(--text-2); border-color: var(--border)">{h}</th>
              {/each}
            </tr>
          </thead>
          <tbody>
            {#each ucShown as r}
              {@const c = reach(r)}
              {@const hostBuf = num(r.buf_device) === 65535}
              <tr>
                <td class="px-2.5 py-1 border-b whitespace-nowrap" style="border-color: var(--border)">
                  {c === 'unresolved' ? '—' : actorLabel(num(r.owner_node), num(r.owner_device), num(r.owner_devices))}
                </td>
                <td class="px-2.5 py-1 border-b" style="border-color: var(--border)">{r.owner_rank ?? '?'}</td>
                <td class="px-2.5 py-1 border-b" style="border-color: var(--border)">{r.actor_rank}</td>
                <td class="px-2.5 py-1 border-b whitespace-nowrap" style="border-color: var(--border);
                    color: {hostBuf ? 'var(--text-3)' : 'var(--text-1)'}">
                  {hostBuf ? 'host memory' : dev(r.buf_device)}
                </td>
                <td class="px-2.5 py-1 border-b whitespace-nowrap text-[10px] uppercase tracking-wide"
                    style="border-color: var(--border); color: {c === 'other host' ? 'var(--text-1)' : 'var(--text-3)'}">{c}</td>
                <td class="px-2.5 py-1 border-b whitespace-nowrap text-[10px] uppercase tracking-wide"
                    style="border-color: var(--border); color: var(--text-3)"
                    title={r.via === 'send' ? 'ucp_tag_send — two-sided, matched by tag'
                      : r.via === 'am' ? 'ucp_am_send — active message, payload only'
                      : r.via === 'put' ? 'ucp_put — one-sided write into the peer'
                      : 'ucp_get — one-sided read; the units travelled from the peer to this rank'}
                >{r.via}</td>
                <td class="px-2.5 py-1 text-right border-b" style="border-color: var(--border)">{count(num(r.n))}</td>
                <td class="px-2.5 py-1 text-right border-b whitespace-nowrap" style="border-color: var(--border)">{count(num(r.units))}</td>
              </tr>
            {/each}
          </tbody>
        </table>
      </div>

      <h3 class="text-[13px] font-medium mt-0 mb-2">Message sizes</h3>
      <table class="w-full text-xs border-collapse">
        <thead>
          <tr>
            {#each ['units per message', 'share', 'messages', 'units'] as h}
              <th class="px-2.5 py-1 font-medium border-b {['messages', 'units'].includes(h) ? 'text-right' : 'text-left'}"
                  style="color: var(--text-2); border-color: var(--border)">{h}</th>
            {/each}
          </tr>
        </thead>
        <tbody>
          {#each ucShownSizes as s}
            <tr>
              <td class="px-2.5 py-1 border-b" style="border-color: var(--border)">{s.size_class}</td>
              <td class="px-2.5 py-1 border-b w-1/2" style="border-color: var(--border)">
                <div class="flex items-center gap-2">
                  <div class="h-2 rounded-sm"
                       style="width: {ucSizeTotal ? (num(s.n) / ucSizeTotal) * 100 : 0}%;
                              min-width: 2px; background: {barColor}"></div>
                  <span style="color: var(--text-3)">
                    {ucSizeTotal ? Math.round((num(s.n) / ucSizeTotal) * 100) : 0}%
                  </span>
                </div>
              </td>
              <td class="px-2.5 py-1 text-right border-b" style="border-color: var(--border)">{count(num(s.n))}</td>
              <td class="px-2.5 py-1 text-right border-b whitespace-nowrap" style="border-color: var(--border)">{count(num(s.units))}</td>
            </tr>
          {/each}
        </tbody>
      </table>

      {#if multiDevice}
        <p class="text-xs mt-3 m-0" style="color: var(--critical)">
          At least one rank's sends came from buffers on more than one GPU, so
          its device cannot be named — those rows are tabbed as <b>GPU ?</b>
          rather than assigned to a guess.
        </p>
      {/if}

      <p class="text-xs mt-3 m-0" style="color: var(--text-3)">
        Tabs are <b>(host, GPU)</b>, the GPU each sending rank drives; the
        <b>peer</b> is the rank at the far end of the UCP endpoint, named the
        same way. Both are <i>attributed</i>, not observed — nothing here saw a
        kernel run. What is observed is <b>buffer on</b>: where each message's
        memory lived when UCX was handed it, which is how a GPU-aware MPI shows
        itself (device buffers) and how its control traffic does (host memory).
        <b>units</b> are UCP datatype units: bytes when the caller handed UCX a
        byte datatype, which the record cannot say — <code>transport_mix</code>'s
        received bytes are the check on a given capture. Compare them within
        this tab, never with the verbs tab's bytes. Which
        transport UCX actually used (shared memory, CUDA IPC, the NIC) is in
        <code>transport_mix</code>; <b>reach</b> here is where the peer is, not
        how the bytes got there.
      </p>
    {/if}
  {/if}
</Card>
