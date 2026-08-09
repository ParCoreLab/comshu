<script>
  import { num } from '../lib/query.js';
  import { cluster, leavesOf } from '../lib/cluster.js';
  import { count } from '../lib/viz.js';
  import Panel from '../lib/Panel.svelte';

  // Clusters ranks by *who they talk to*, from the same comm_edges the matrix
  // draws. On a ring this has a known answer — neighbours merge first — which
  // is what makes the panel checkable rather than decorative.
  const SQL = `SELECT src_rank, dst_rank, bytes
               FROM comm_edges WHERE dst_rank >= 0 ORDER BY src_rank, dst_rank`;

  const LINE = '#2a78d6';

  function build(rows) {
    const ranks = [...new Set(rows.flatMap((r) => [num(r.src_rank), num(r.dst_rank)]))].sort((a, b) => a - b);
    const idx = new Map(ranks.map((r, i) => [r, i]));
    const vecs = ranks.map(() => new Array(ranks.length).fill(0));
    for (const r of rows) {
      // log1p, so a ring's two very different edge weights (1.3 GB vs 158 KB)
      // both count as "talks to", instead of the payload edge drowning the ack.
      vecs[idx.get(num(r.src_rank))][idx.get(num(r.dst_rank))] = Math.log1p(num(r.bytes));
    }
    const root = cluster(vecs, ranks.map(String));
    return { root, leaves: leavesOf(root) };
  }

  // Positions every node: leaves evenly spaced down the y axis, internal nodes
  // at the mean of their children, x by merge height.
  function layout(root, leaves, w, rowH) {
    const pos = new Map();
    leaves.forEach((l, i) => pos.set(l, { x: w, y: i * rowH + rowH / 2 }));
    const maxH = root.height || 1;
    (function place(n) {
      if (!n.children) return pos.get(n);
      const kids = n.children.map(place);
      const p = {
        x: w - (n.height / maxH) * (w - 8),
        y: kids.reduce((a, k) => a + k.y, 0) / kids.length,
      };
      pos.set(n, p);
      return p;
    })(root);
    return pos;
  }

  // Elbow: down the parent's x to the child's y, then out to the child.
  function links(root, pos, out = []) {
    if (!root.children) return out;
    const p = pos.get(root);
    for (const c of root.children) {
      const q = pos.get(c);
      out.push(`M${p.x},${p.y} L${p.x},${q.y} L${q.x},${q.y}`);
      links(c, pos, out);
    }
    return out;
  }
</script>

<Panel
  title="Rank clustering"
  subtitle={(rows) => `${new Set(rows.map((r) => r.src_rank)).size} ranks, by communication pattern (cosine distance)`}
  sql={SQL}
  needs="comm_edges with at least two ranks — run pcomm-read --link"
>
  {#snippet body(rows)}
    {@const { root, leaves } = build(rows)}
    {@const rowH = leaves.length > 40 ? 12 : leaves.length > 20 ? 16 : 22}
    {@const W = 320}
    {@const H = leaves.length * rowH}
    {@const pos = layout(root, leaves, W, rowH)}
    <div class="overflow-x-auto">
      <svg width={W + 60} height={H + 8} role="img"
           aria-label="dendrogram clustering ranks by communication pattern">
        {#each links(root, pos) as d}
          <path {d} fill="none" stroke={LINE} stroke-width="2" stroke-linejoin="round" />
        {/each}
        {#each leaves as l}
          {@const p = pos.get(l)}
          <text x={W + 6} y={p.y + 3} font-size="10" fill="var(--text-2)">{l.label}</text>
        {/each}
      </svg>
    </div>
    <p class="text-xs mt-3 m-0" style="color: var(--text-3)">
      {count(leaves.length)} leaves. Merge height is cosine distance between
      rank vectors of bytes-per-peer, so this groups ranks that <em>share
      peers</em> — not ranks that talk to each other.
    </p>
    <p class="text-xs mt-1.5 m-0" style="color: var(--text-3)">
      The distinction is easy to miss and a ring makes it stark: rank 0 sends to
      {'{'}1, N−1{'}'} and rank 1 to {'{'}0, 2{'}'}, so adjacent ranks share no
      peer at all and sit maximally far apart, while 0 and 2 both talk to 1 and
      merge immediately. An even ring therefore splits cleanly into even and odd
      — a real property of the topology under this metric, not a clustering
      artefact, and not evidence of two communities. On a ring there are none.
    </p>
  {/snippet}
</Panel>
