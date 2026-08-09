<script>
  import { query } from './query.js';
  import Card from './Card.svelte';

  /**
   * A panel that runs one query and renders it, with the four states every
   * panel has: loading, failed, empty, and data.
   *
   * `empty` is a first-class state and not an oversight. Several tables are
   * legitimately absent — `source_line` needs the gpumem provider, the `ib_*`
   * family is empty under IBGDA — and a panel that renders a blank chart for
   * "this capture has no such data" is indistinguishable from one that is
   * broken. Say which.
   */
  let {
    title,
    subtitle = '',       // string, or (rows) => string
    sql,
    needs = '',          // what a capture must contain for this panel to work
    body,
  } = $props();

  let rows = $state(null);
  let error = $state(null);

  $effect(() => {
    let alive = true;
    rows = null;
    error = null;
    query(sql)
      .then((r) => alive && (rows = r))
      .catch((e) => alive && (error = e.message));
    return () => (alive = false);
  });

  const sub = $derived(
    rows && typeof subtitle === 'function' ? subtitle(rows) : typeof subtitle === 'string' ? subtitle : '',
  );
</script>

<Card {title} subtitle={error ? '' : sub}>
  {#if error}
    <p class="text-[13px] m-0" style="color: var(--critical)">{error}</p>
    {#if needs}
      <p class="text-xs mt-1 m-0" style="color: var(--text-2)">Needs: {needs}</p>
    {/if}
  {:else if rows === null}
    <p class="text-[13px] m-0" style="color: var(--text-2)">loading…</p>
  {:else if rows.length === 0}
    <p class="text-[13px] m-0" style="color: var(--text-2)">
      Nothing to show — this capture has no such data.
    </p>
    {#if needs}
      <p class="text-xs mt-1 m-0" style="color: var(--text-3)">Needs: {needs}</p>
    {/if}
  {:else}
    {@render body(rows)}
  {/if}
</Card>
