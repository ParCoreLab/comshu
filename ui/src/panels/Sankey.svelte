<script>
  import { num } from '../lib/query.js';
  import { sankey, ribbonPath } from '../lib/sankey.js';
  import { count, bytes } from '../lib/viz.js';
  import Panel from '../lib/Panel.svelte';
  import Tooltip from '../lib/Tooltip.svelte';

  // Transport -> operation -> message size. Both planes are unioned, so an
  // IBGDA run breaks down the same way a verbs run does.
  //
  // Weighted by SENDS, not bytes, and that is a data-driven choice: on the
  // 32-rank NCCL capture, opcode 1 (write-with-immediate) posts 79,360 work
  // requests carrying zero payload — real operations that a byte weighting
  // erases completely. Bytes are in the tooltip.
  //
  // NVLink is included, and its operations are named rather than numbered.
  // Both planes number their ops from 0, and they mean different things — a
  // verbs opcode 1 is write-with-immediate, a MemOp 1 is a Store. Rendering
  // both as "opcode 1" would merge two unrelated things into one node, which is
  // the same conflation the transport dimension exists to prevent.
  //
  // Verbs and DEVX number their operations in DIFFERENT spaces — ibv_wr_opcode
  // (0 = RDMA write) against the mlx5 WQE opcode (0x08 = RDMA write) — and both
  // were rendered "opcode N", so unrelated operations merged into one node
  // (CR-65). The well-known ones are decoded to one vocabulary, because a write
  // is a write whichever plane posted it; anything else keeps its number,
  // prefixed by the plane it is a number in.
  const SQL = `
    WITH s AS (
      SELECT 'verbs'  AS transport,
             CASE opcode WHEN 0 THEN 'write' WHEN 1 THEN 'write+imm' WHEN 2 THEN 'send'
                         WHEN 3 THEN 'send+imm' WHEN 4 THEN 'read' WHEN 5 THEN 'atomic cas'
                         WHEN 6 THEN 'atomic fadd'
                         ELSE 'verbs op ' || opcode END AS op,
             length FROM ib_post_send
      UNION ALL
      SELECT 'devx',
             CASE opcode WHEN 8 THEN 'write' WHEN 9 THEN 'write+imm' WHEN 10 THEN 'send'
                         WHEN 11 THEN 'send+imm' WHEN 16 THEN 'read' WHEN 17 THEN 'atomic cas'
                         WHEN 18 THEN 'atomic fadd'
                         ELSE 'mlx5 op ' || opcode END,
             length FROM mlx5_device_send
      UNION ALL
      SELECT 'nvlink',
             CASE op WHEN 0 THEN 'load' WHEN 1 THEN 'store'
                     WHEN 2 THEN 'atomic' WHEN 3 THEN 'prefetch'
                     ELSE 'op' || op END,
             size
      FROM gpu_access WHERE run_device <> mem_device
    )
    SELECT transport,
           op,
           CASE WHEN length = 0            THEN 'empty'
                WHEN length < 1024         THEN '<1 KB'
                WHEN length < 65536        THEN '1–64 KB'
                WHEN length < 1048576      THEN '64 KB–1 MB'
                ELSE '≥1 MB' END           AS size_class,
           count(*)::BIGINT       AS sends,
           sum(length)::BIGINT    AS bytes
    FROM s GROUP BY 1, 2, 3 ORDER BY sends DESC`;

  const ORDER = ['empty', '<1 KB', '1–64 KB', '64 KB–1 MB', '≥1 MB'];
  // Categorical slots, fixed order, never cycled.
  const HUES = ['#2a78d6', '#eb6834', '#1baf7a', '#eda100', '#e87ba4', '#008300', '#4a3aa7'];

  let tip = $state(null);
  let tipRef = $state(null);

  function model(rows) {
    const stages = [
      [...new Set(rows.map((r) => r.transport))],
      [...new Set(rows.map((r) => r.op))].sort(),
      ORDER.filter((s) => rows.some((r) => r.size_class === s)),
    ];
    const agg = new Map();
    const bump = (from, to, sends, b) => {
      const k = `${from}|${to}`;
      const cur = agg.get(k) ?? { from, to, value: 0, bytes: 0 };
      cur.value += sends; cur.bytes += b;
      agg.set(k, cur);
    };
    for (const r of rows) {
      const sends = num(r.sends), b = num(r.bytes);
      bump(`0:${r.transport}`, `1:${r.op}`, sends, b);
      bump(`1:${r.op}`, `2:${r.size_class}`, sends, b);
    }
    return { stages, links: [...agg.values()] };
  }

  const hueOf = (stages, key) => {
    const [si, name] = [Number(key.split(':')[0]), key.slice(key.indexOf(':') + 1)];
    return HUES[stages[si].indexOf(name) % HUES.length];
  };
</script>

<Panel
  title="Operation breakdown"
  subtitle={(rows) => `${count(rows.reduce((a, r) => a + num(r.sends), 0))} operations by transport, op and size`}
  sql={SQL}
  needs="ib_post_send or mlx5_device_send"
>
  {#snippet body(rows)}
    {@const { stages, links } = model(rows)}
    {@const W = 620}
    {@const H = 300}
    {@const g = sankey(links, stages, { width: W, height: H })}
    <div class="overflow-x-auto">
      <svg width={W + 130} height={H + 24} role="img"
           aria-label="flow of send operations from transport through opcode to message size">
        {#each g.ribbons as r}
          <path
            d={ribbonPath(r)}
            fill={hueOf(stages, r.from)}
            fill-opacity="0.34"
            onmousemove={(ev) => {
              tip = { title: `${r.from.split(':')[1]} → ${r.to.split(':')[1]}`,
                      body: `${count(r.value)} sends · ${bytes(r.bytes)}` };
              tipRef?.move(ev);
            }}
            onmouseleave={() => (tip = null)}
            role="presentation"
          />
        {/each}
        {#each g.nodes as n}
          <rect x={n.x} y={n.y} width={g.nodeWidth} height={n.h} rx="2" fill={hueOf(stages, n.key)} />
          <text
            x={n.stage === stages.length - 1 ? n.x + g.nodeWidth + 6 : n.x + g.nodeWidth + 6}
            y={n.y + n.h / 2 + 3} font-size="11" fill="var(--text-2)">{n.name}</text>
        {/each}
      </svg>
    </div>
    <p class="text-xs mt-2 m-0" style="color: var(--text-3)">
      Width is number of operations. Weighted by count rather than bytes so
      zero-length work requests stay visible; bytes are in the tooltip.
      An operation means something different per transport — a verbs message
      against an instrumented 4-byte GPU access — so the transport stage is the
      thing to read first, and widths across it are counts of different events.
      Verbs and DEVX number their opcodes in different spaces, so each is named
      under its own transport; an opcode with no name here shows as
      <code>verbs op N</code> or <code>mlx5 op N</code>.
    </p>
  {/snippet}
</Panel>

<Tooltip bind:this={tipRef} {tip} />
