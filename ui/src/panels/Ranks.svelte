<script>
  import { num } from '../lib/query.js';
  import { bytes, count } from '../lib/viz.js';
  import Panel from '../lib/Panel.svelte';

  // The GPU/rank view, from the same comm_edges the matrix reads — no second
  // endpoint, no server-side rollup. If this needed a table the matrix does not
  // have, that table would belong in `pcomm-read --link`, not here.
  //
  // **Grouped by transport, never summed across it.** This panel briefly did
  // sum, and the result was a rank "sending 790,372 bytes" that was really
  // 196,608 NVLink 4-byte stores plus ~91 InfiniBand messages added together —
  // a number with no meaning, presented as a total. One section per transport,
  // each with its own bar scale, is the small-multiples answer: an NVLink ring
  // measured in 4 MB would otherwise be an invisible sliver beside a GB of IB.
  const SQL = `
    WITH out AS (
      SELECT src_rank AS rank, transport, unit,
             sum(bytes)::BIGINT AS sent,
             sum(sends)::BIGINT AS ops,
             count(DISTINCT dst_rank) FILTER (dst_rank >= 0) AS peers
      FROM comm_edges GROUP BY 1, 2, 3
    ), inn AS (
      SELECT dst_rank AS rank, transport, sum(bytes)::BIGINT AS recv
      FROM comm_edges WHERE dst_rank >= 0 GROUP BY 1, 2
    )
    SELECT o.rank, o.transport, o.unit, o.sent, o.ops, o.peers,
           coalesce(i.recv, 0)::BIGINT AS recv
    FROM out o
    LEFT JOIN inn i ON i.rank = o.rank AND i.transport = o.transport
    ORDER BY o.transport, o.rank`;

  const BAR = '#2a78d6';

  // Grouped in the client rather than by a second query: it is the same rows,
  // and a panel that issued one query per transport would scan the capture once
  // per transport to fill one screen.
  function groups(rows) {
    const by = new Map();
    for (const r of rows) {
      if (!by.has(r.transport)) by.set(r.transport, []);
      by.get(r.transport).push(r);
    }
    return [...by.entries()];
  }
</script>

<Panel
  title="Per-rank traffic"
  subtitle={(rows) =>
    `${new Set(rows.map((r) => r.rank)).size} ranks · ` +
    `${[...new Set(rows.map((r) => r.transport))].join(', ')}`}
  sql={SQL}
  needs="comm_edges — run pcomm-read --link"
>
  {#snippet body(rows)}
    {#each groups(rows) as [transport, rs]}
      {@const max = Math.max(1, ...rs.map((r) => num(r.sent)))}
      {@const unit = rs[0]?.unit ?? 'messages'}
      <div class="mb-5">
        <div class="flex items-baseline gap-2 mb-1.5">
          <h3 class="text-[13px] font-semibold m-0">{transport}</h3>
          <span class="text-xs" style="color: var(--text-3)">
            measured in {unit} — not comparable with the other sections
          </span>
        </div>
        <div class="overflow-x-auto max-h-[50vh]">
          <table class="w-full text-xs border-collapse">
            <thead>
              <tr>
                {#each ['rank', 'sent', '', 'received', unit, 'peers'] as h}
                  <th
                    class="px-2.5 py-1 font-medium border-b {h === 'rank' || h === '' ? 'text-left' : 'text-right'}"
                    style="color: var(--text-2); border-color: var(--border)">{h}</th>
                {/each}
              </tr>
            </thead>
            <tbody>
              {#each rs as r}
                {@const sent = num(r.sent)}
                <tr>
                  <td class="px-2.5 py-1 border-b" style="border-color: var(--border)">{r.rank}</td>
                  <td class="px-2.5 py-1 text-right border-b whitespace-nowrap"
                      style="border-color: var(--border)">{bytes(sent)}</td>
                  <td class="px-2.5 py-1 border-b w-1/3" style="border-color: var(--border)">
                    <div class="h-2 rounded-[4px]"
                         style="width: {(sent / max) * 100}%; background: {BAR}"></div>
                  </td>
                  <td class="px-2.5 py-1 text-right border-b whitespace-nowrap"
                      style="border-color: var(--border)">{bytes(num(r.recv))}</td>
                  <td class="px-2.5 py-1 text-right border-b"
                      style="border-color: var(--border)">{count(num(r.ops))}</td>
                  <td class="px-2.5 py-1 text-right border-b"
                      style="border-color: var(--border)">{count(num(r.peers))}</td>
                </tr>
              {/each}
            </tbody>
          </table>
        </div>
      </div>
    {/each}
  {/snippet}
</Panel>
