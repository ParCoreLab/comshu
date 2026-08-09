<script>
  import { num } from '../lib/query.js';
  import { count } from '../lib/viz.js';
  import Panel from '../lib/Panel.svelte';

  // Source attribution. Empty on every capture in the corpus today: `source_line`
  // is produced by the gpumem/NVBit provider, and no suite case currently runs
  // with PC->line resolution on. The panel is written against the real table so
  // it lights up when one does, rather than being sketched against a fixture
  // that would never be checked.
  //
  // `file` is an interned handle, not a path — the strings live in
  // intern-blob.bin, which the Parquet export does not carry. Resolving them
  // means an `intern` table with the text dereferenced, which belongs in
  // pcomm-read (step 2's remaining work), not in a browser.
  const SQL = `
    SELECT file, line,
           count(*)::BIGINT              AS rows,
           count(DISTINCT pc)::BIGINT    AS pcs,
           count(DISTINCT rank)::BIGINT  AS ranks
    FROM source_line
    GROUP BY file, line
    ORDER BY rows DESC
    LIMIT 500`;
</script>

<Panel
  title="Source attribution"
  subtitle={(rows) => `${count(rows.length)} source locations`}
  sql={SQL}
  needs="source_line — the gpumem/NVBit provider with PC→line resolution enabled"
>
  {#snippet body(rows)}
    <div class="overflow-x-auto max-h-[70vh]">
      <table class="w-full text-xs border-collapse">
        <thead>
          <tr>
            {#each ['file (handle)', 'line', 'rows', 'pcs', 'ranks'] as h}
              <th class="px-2.5 py-1 font-medium border-b {h.startsWith('file') ? 'text-left' : 'text-right'}"
                  style="color: var(--text-2); border-color: var(--border)">{h}</th>
            {/each}
          </tr>
        </thead>
        <tbody>
          {#each rows as r}
            <tr>
              <td class="px-2.5 py-1 border-b" style="border-color: var(--border)">{r.file}</td>
              <td class="px-2.5 py-1 text-right border-b" style="border-color: var(--border)">{r.line}</td>
              <td class="px-2.5 py-1 text-right border-b" style="border-color: var(--border)">{count(num(r.rows))}</td>
              <td class="px-2.5 py-1 text-right border-b" style="border-color: var(--border)">{count(num(r.pcs))}</td>
              <td class="px-2.5 py-1 text-right border-b" style="border-color: var(--border)">{count(num(r.ranks))}</td>
            </tr>
          {/each}
        </tbody>
      </table>
    </div>
    <p class="text-xs mt-2 m-0" style="color: var(--text-3)">
      <code>file</code> is an interned handle. The strings live in
      <code>intern-blob.bin</code>, which the Parquet export does not carry —
      resolving them needs an <code>intern</code> table from
      <code>pcomm-read</code>, not a lookup in the browser.
    </p>
  {/snippet}
</Panel>
