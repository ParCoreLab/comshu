<script>
  import { query, num } from './lib/query.js';
  import Matrix from './panels/Matrix.svelte';
  import Graph from './panels/Graph.svelte';
  import Ranks from './panels/Ranks.svelte';
  import Dendrogram from './panels/Dendrogram.svelte';
  import Sankey from './panels/Sankey.svelte';
  import Objects from './panels/Objects.svelte';
  import Devices from './panels/Devices.svelte';
  import SourceLines from './panels/SourceLines.svelte';
  import Timeline from './panels/Timeline.svelte';
  import Ask from './panels/Ask.svelte';
  import Chat from './panels/Chat.svelte';

  // ?theme=light|dark stamps the root the way a toggle would. It exists because
  // a dark-only render is how the inverted sequential ramp survived its first
  // review — both modes have to stay reachable without touching an OS setting.
  const t = new URLSearchParams(location.search).get('theme');
  if (t === 'light' || t === 'dark') document.documentElement.dataset.theme = t;

  // One panel at a time, because every panel is a query and rendering eight at
  // once means eight scans of the same capture to show one of them.
  const VIEWS = [
    { id: 'matrix',  label: 'Matrix',     c: Matrix },
    // Next to the matrix on purpose: same rows, read as shape instead of as
    // numbers. See the header comment in Graph.svelte for why both exist.
    { id: 'graph',   label: 'Graph',      c: Graph },
    { id: 'ranks',   label: 'Ranks',      c: Ranks },
    { id: 'cluster', label: 'Clustering', c: Dendrogram },
    { id: 'flow',    label: 'Flow',       c: Sankey },
    { id: 'objects', label: 'Objects',    c: Objects },
    { id: 'devices', label: 'Devices',    c: Devices },
    { id: 'source',  label: 'Source',     c: SourceLines },
    { id: 'time',    label: 'Timeline',   c: Timeline },
    { id: 'ask',     label: 'Ask',        c: Ask },
  ];

  // The view lives in the URL fragment, so a view is linkable and survives a
  // reload — the cheapest thing that makes a tool shareable ("look at #flow").
  //
  // Split on ',' because a panel may append its own state after the id: the
  // timeline writes `#time,<lo>,<hi>` so a zoom is linkable too. Everything
  // after the first comma belongs to the panel and is opaque here.
  const fromHash = () =>
    VIEWS.find((v) => v.id === location.hash.slice(1).split(',')[0])?.id ?? 'matrix';
  let active = $state(fromHash());
  $effect(() => {
    const on = () => (active = fromHash());
    addEventListener('hashchange', on);
    return () => removeEventListener('hashchange', on);
  });
  const Current = $derived(VIEWS.find((v) => v.id === active).c);

  // The chat pane is not a view — it is a way of getting to the views, so it
  // sits beside them rather than replacing one, and it stays open across
  // navigation. It is remembered because a pane you must reopen on every reload
  // is a pane you stop using.
  let chat = $state(localStorage.getItem('pcomm.chat') === 'open');
  $effect(() => localStorage.setItem('pcomm.chat', chat ? 'open' : 'closed'));

  let health = $state([]);
  $effect(() => {
    query('SELECT finding, n, note FROM link_health WHERE n > 0')
      .then((r) => (health = r))
      .catch(() => (health = []));
  });
</script>

<div class="flex items-start">
<main class="grow min-w-0 max-w-[1100px] mx-auto p-6">
  <div class="flex items-start gap-3">
    <div class="grow">
      <h1 class="text-lg font-semibold m-0">pcomm</h1>
      <p class="text-[13px] mt-0.5 mb-4" style="color: var(--text-2)">
        multi-GPU communication profile
      </p>
    </div>
    {#if !chat}
      <button
        onclick={() => (chat = true)}
        class="text-[13px] rounded-md px-3 py-1.5 border cursor-pointer"
        style="border-color: var(--border); background: transparent; color: var(--text-2)"
      >Chat</button>
    {/if}
  </div>

  <nav class="flex flex-wrap gap-1 mb-5">
    {#each VIEWS as v}
      <a
        href="#{v.id}"
        aria-current={active === v.id ? 'page' : undefined}
        class="text-[13px] rounded-md px-3 py-1.5 border no-underline"
        style="border-color: {active === v.id ? 'var(--text-3)' : 'var(--border)'};
               background: {active === v.id ? 'var(--surface-1)' : 'transparent'};
               color: {active === v.id ? 'var(--text-1)' : 'var(--text-2)'}"
      >{v.label}</a>
    {/each}
  </nav>

  <!-- Surfaced on every view, never swallowed: "a wrong matrix that looks
       confident is worse than no matrix." -->
  {#each health as h}
    <div class="border-l-[3px] px-3 py-2 mb-4 text-[13px]"
         style="border-color: var(--critical); background: var(--surface-1)">
      <b style="color: var(--critical)">{h.finding} = {num(h.n)}</b> — {h.note}
    </div>
  {/each}

  <Current />
</main>

{#if chat}
  <Chat open={chat} onclose={() => (chat = false)} />
{/if}
</div>
