<script>
  // A model-authored chart: the spec came from the agent, the rows come from
  // `query()`. Never from the agent — that split is the whole design, and it is
  // why the SQL is printed under the plot rather than hidden behind it.
  import { query } from './query.js';
  import { render } from './chart.js';

  let { chart, onask } = $props();

  let el = $state(null);
  let error = $state(null);
  let rows = $state(null);

  $effect(() => {
    if (!el) return;
    let view = null;
    let dead = false;
    query(chart.sql)
      .then(async (r) => {
        if (dead) return;
        rows = r;
        if (!r.length) return;
        view = await render(el, chart.spec, r);
      })
      .catch((e) => {
        if (!dead) error = e.message;
      });
    return () => {
      dead = true;
      view?.finalize();
    };
  });
</script>

<figure class="m-0 mt-2 rounded-md border p-3" style="border-color: var(--border); background: var(--surface-1)">
  <figcaption class="text-[13px] font-medium mb-2" style="color: var(--text-1)">{chart.title}</figcaption>

  {#if error}
    <pre class="text-xs whitespace-pre-wrap m-0" style="color: var(--critical)">{error}</pre>
  {:else if rows && !rows.length}
    <p class="text-xs m-0" style="color: var(--text-2)">The query returned no rows.</p>
  {/if}

  <div bind:this={el} class="w-full overflow-x-auto"></div>

  <details class="mt-2">
    <summary class="cursor-pointer text-xs" style="color: var(--text-3)">
      {rows ? `${rows.length} rows` : 'loading'} — show the query
    </summary>
    <pre class="text-[11px] font-mono whitespace-pre-wrap mt-1.5 mb-0" style="color: var(--text-2)">{chart.sql}</pre>
    <button
      onclick={() => onask?.(chart.sql)}
      class="text-[11px] rounded px-2 py-0.5 border cursor-pointer mt-1.5"
      style="background: var(--surface-0); border-color: var(--border); color: var(--text-2)"
    >open in Ask</button>
  </details>
</figure>
