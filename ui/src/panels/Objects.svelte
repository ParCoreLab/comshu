<script>
  import { num } from '../lib/query.js';
  import { bytes, count, logScale } from '../lib/viz.js';
  import Panel from '../lib/Panel.svelte';

  // Memory objects: what was allocated, how big, and which device claims it.
  //
  // `device = 65535` is kUnknownDevice — "we could not attribute this", which is
  // a different statement from "host memory", and the two must not be merged.
  // It is shown as `unknown` rather than dropped or folded into device 0.
  const SQL = `
    SELECT rank, device,
           count(*)::BIGINT                AS objects,
           sum(range_size)::BIGINT         AS total_bytes,
           max(range_size)::BIGINT         AS largest,
           count(DISTINCT range_base)::BIGINT AS distinct_bases
    FROM mem_object
    GROUP BY rank, device
    ORDER BY total_bytes DESC`;

  const dev = (d) => (num(d) === 65535 ? 'unknown' : `GPU ${d}`);
</script>

<Panel
  title="Memory objects"
  subtitle={(rows) =>
    `${count(rows.reduce((a, r) => a + num(r.objects), 0))} objects across ` +
    `${new Set(rows.map((r) => r.rank)).size} ranks and ` +
    `${new Set(rows.map((r) => r.device)).size} device ids`}
  sql={SQL}
  needs="mem_object — emitted by any provider that sees an allocation"
>
  {#snippet body(rows)}
    {@const color = logScale(rows.map((r) => num(r.total_bytes)))}
    {@const max = Math.max(...rows.map((r) => num(r.total_bytes)))}
    <div class="overflow-x-auto max-h-[70vh]">
      <table class="w-full text-xs border-collapse">
        <thead>
          <tr>
            {#each ['rank', 'device', 'objects', 'total', '', 'largest'] as h}
              <th class="px-2.5 py-1 font-medium border-b {h === 'rank' || h === 'device' || h === '' ? 'text-left' : 'text-right'}"
                  style="color: var(--text-2); border-color: var(--border)">{h}</th>
            {/each}
          </tr>
        </thead>
        <tbody>
          {#each rows as r}
            {@const tb = num(r.total_bytes)}
            <tr>
              <td class="px-2.5 py-1 border-b" style="border-color: var(--border)">{r.rank}</td>
              <td class="px-2.5 py-1 border-b whitespace-nowrap" style="border-color: var(--border)">
                <span style="color: {num(r.device) === 65535 ? 'var(--text-3)' : 'var(--text-1)'}">{dev(r.device)}</span>
              </td>
              <td class="px-2.5 py-1 text-right border-b" style="border-color: var(--border)">{count(num(r.objects))}</td>
              <td class="px-2.5 py-1 text-right border-b whitespace-nowrap" style="border-color: var(--border)">{bytes(tb)}</td>
              <td class="px-2.5 py-1 border-b w-1/3" style="border-color: var(--border)">
                <div class="h-2 rounded-[4px]" style="width: {(tb / max) * 100}%; background: {color(tb)}"></div>
              </td>
              <td class="px-2.5 py-1 text-right border-b whitespace-nowrap" style="border-color: var(--border)">{bytes(num(r.largest))}</td>
            </tr>
          {/each}
        </tbody>
      </table>
    </div>
    <p class="text-xs mt-2 m-0" style="color: var(--text-3)">
      <code>unknown</code> is device id 65535 — attribution failed, which is not
      the same as host memory and is deliberately not merged with GPU 0.
    </p>
  {/snippet}
</Panel>
