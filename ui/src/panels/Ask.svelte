<script>
  import { query } from '../lib/query.js';
  import { count } from '../lib/viz.js';
  import Card from '../lib/Card.svelte';

  // The query panel — the honest half of the "ask a question" panel.
  //
  // The natural-language layer is not here yet, and stubbing a fake one would
  // be worse than none. What that layer will need already exists, though, and
  // this panel is it: the model's job will be to emit SQL, and this is where
  // that SQL runs. The constraints stay the same either way — read-only, row
  // capped, results shown as a table the user can check.
  //
  // The schema list is the other half: a model cannot write a query against
  // tables it has not been shown, and neither can a person.
  // The chat pane hands a query over two ways, and both are needed: the
  // sessionStorage key for when this panel has not mounted yet, and the event
  // for when it is already open — in which case nothing re-runs and, before the
  // event existed, every hand-off after the first silently did nothing.
  //
  // One-shot on read: this panel is where a query becomes yours to edit, so a
  // later reload must not overwrite what you have typed.
  const handed = sessionStorage.getItem('comshu.ask.sql');
  if (handed) sessionStorage.removeItem('comshu.ask.sql');

  let sql = $state(
    handed ?? 'SELECT src_rank, dst_rank, sends, bytes\nFROM comm_edges\nORDER BY bytes DESC\nLIMIT 10',
  );
  let rows = $state(null);
  let cols = $state([]);
  let error = $state(null);
  let busy = $state(false);
  let tables = $state([]);

  $effect(() => {
    const take = (e) => {
      sql = e.detail;
      rows = null;
      error = null;
    };
    addEventListener('comshu:ask', take);
    return () => removeEventListener('comshu:ask', take);
  });

  $effect(() => {
    // Views (parquet-backed) and tables (the empty stand-ins mounted for
    // streams this capture lacks) — both are queryable, so both are listed.
    query(`SELECT t FROM (
             SELECT view_name AS t FROM duckdb_views() WHERE NOT internal
             UNION ALL
             SELECT table_name FROM duckdb_tables()
           ) ORDER BY 1`)
      .then((r) => (tables = r.map((x) => x.t)))
      .catch(() => (tables = []));
  });

  async function run() {
    busy = true; error = null;
    try {
      const r = await query(sql);
      rows = r;
      cols = r.length ? Object.keys(r[0]) : [];
    } catch (e) {
      error = e.message; rows = null;
    } finally {
      busy = false;
    }
  }
</script>

<Card title="Ask" subtitle="Query the capture directly. Read-only; results are capped.">
  <textarea
    bind:value={sql}
    spellcheck="false"
    class="w-full h-28 text-xs font-mono rounded-md border p-2.5 resize-y"
    style="background: var(--surface-0); border-color: var(--border); color: var(--text-1)"
  ></textarea>

  <div class="flex items-center gap-3 mt-2">
    <button
      onclick={run} disabled={busy}
      class="text-[13px] rounded-md px-3 py-1.5 border cursor-pointer disabled:opacity-50"
      style="background: var(--surface-0); border-color: var(--border); color: var(--text-1)"
    >{busy ? 'running…' : 'Run'}</button>
    {#if rows}
      <span class="text-xs" style="color: var(--text-2)">{count(rows.length)} rows</span>
    {/if}
  </div>

  {#if error}
    <pre class="text-xs mt-3 whitespace-pre-wrap m-0" style="color: var(--critical)">{error}</pre>
  {/if}

  {#if rows && rows.length}
    <div class="overflow-auto max-h-96 mt-3">
      <table class="w-full text-xs border-collapse">
        <thead>
          <tr>
            {#each cols as c}
              <th class="text-left px-2.5 py-1 font-medium border-b sticky top-0"
                  style="color: var(--text-2); border-color: var(--border); background: var(--surface-1)">{c}</th>
            {/each}
          </tr>
        </thead>
        <tbody>
          {#each rows as r}
            <tr>
              {#each cols as c}
                <td class="px-2.5 py-1 border-b whitespace-nowrap"
                    style="border-color: var(--border)">{r[c]}</td>
              {/each}
            </tr>
          {/each}
        </tbody>
      </table>
    </div>
  {:else if rows}
    <p class="text-[13px] mt-3 m-0" style="color: var(--text-2)">No rows.</p>
  {/if}

  {#if tables.length}
    <details class="mt-4">
      <summary class="cursor-pointer text-[13px]" style="color: var(--text-2)">
        {tables.length} tables in this capture
      </summary>
      <div class="flex flex-wrap gap-1.5 mt-2">
        {#each tables as t}
          <button
            onclick={() => (sql = `SELECT * FROM ${t} LIMIT 20`)}
            class="text-xs rounded px-2 py-0.5 border cursor-pointer font-mono"
            style="background: var(--surface-0); border-color: var(--border); color: var(--text-2)"
          >{t}</button>
        {/each}
      </div>
    </details>
  {/if}
</Card>
