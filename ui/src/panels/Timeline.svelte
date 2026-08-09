<script>
  import { num } from '../lib/query.js';
  import { count } from '../lib/viz.js';
  import Panel from '../lib/Panel.svelte';

  // Deliberately not a timeline.
  //
  // Every other panel is a small aggregate of a huge input; the timeline is the
  // one view that genuinely needs the firehose — millions of rows — and Perfetto
  // already does it better than we would. `pcomm-read --pftrace` writes a native
  // trace. Owning a timeline engine would cost more than every other panel
  // combined.
  //
  // What this panel does instead: say how big the timeline would be, and give
  // the exact command. The future upgrade is embedding Perfetto's UI in an
  // iframe and handing it the trace over postMessage, so clicking a QP in the
  // matrix opens the timeline at that slice — with still no timeline code here.
  const SQL = `
    SELECT 'ib_post_send' AS stream, count(*)::BIGINT AS events FROM ib_post_send
    UNION ALL SELECT 'ib_poll_cq',     count(*)::BIGINT FROM ib_poll_cq
    UNION ALL SELECT 'mlx5_device_send', count(*)::BIGINT FROM mlx5_device_send
    UNION ALL SELECT 'gpu_access',     count(*)::BIGINT FROM gpu_access
    ORDER BY events DESC`;
</script>

<Panel
  title="Timeline"
  subtitle="Handed to Perfetto — this tool does not draw timelines"
  sql={SQL}
  needs="any event stream"
>
  {#snippet body(rows)}
    {@const total = rows.reduce((a, r) => a + num(r.events), 0)}
    <p class="text-[13px] mt-0 mb-3" style="color: var(--text-2)">
      {count(total)} timeline events in this capture. Export a native Perfetto
      trace and open it at
      <a href="https://ui.perfetto.dev" target="_blank" rel="noreferrer"
         style="color: var(--text-1)">ui.perfetto.dev</a>:
    </p>
    <pre class="text-xs rounded-md p-3 overflow-x-auto m-0"
         style="background: var(--surface-0); color: var(--text-1)">pcomm-read &lt;trace-dir&gt; --pftrace out.pftrace</pre>

    <table class="w-full text-xs border-collapse mt-4">
      <thead>
        <tr>
          <th class="text-left px-2.5 py-1 font-medium border-b"
              style="color: var(--text-2); border-color: var(--border)">stream</th>
          <th class="text-right px-2.5 py-1 font-medium border-b"
              style="color: var(--text-2); border-color: var(--border)">events</th>
        </tr>
      </thead>
      <tbody>
        {#each rows.filter((r) => num(r.events) > 0) as r}
          <tr>
            <td class="px-2.5 py-1 border-b" style="border-color: var(--border)">{r.stream}</td>
            <td class="px-2.5 py-1 text-right border-b" style="border-color: var(--border)">{count(num(r.events))}</td>
          </tr>
        {/each}
      </tbody>
    </table>
  {/snippet}
</Panel>
