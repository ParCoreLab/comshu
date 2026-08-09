<script>
  import { query, num } from './lib/query.js';
  import Matrix from './panels/Matrix.svelte';
  import Ranks from './panels/Ranks.svelte';
  import Dendrogram from './panels/Dendrogram.svelte';
  import Sankey from './panels/Sankey.svelte';
  import Objects from './panels/Objects.svelte';
  import SourceLines from './panels/SourceLines.svelte';
  import Timeline from './panels/Timeline.svelte';
  import Ask from './panels/Ask.svelte';

  // ?theme=light|dark stamps the root the way a toggle would. It exists because
  // a dark-only render is how the inverted sequential ramp survived its first
  // review — both modes have to stay reachable without touching an OS setting.
  const t = new URLSearchParams(location.search).get('theme');
  if (t === 'light' || t === 'dark') document.documentElement.dataset.theme = t;

  // One panel at a time, because every panel is a query and rendering eight at
  // once means eight scans of the same capture to show one of them.
  const VIEWS = [
    { id: 'matrix',  label: 'Matrix',     c: Matrix },
    { id: 'ranks',   label: 'Ranks',      c: Ranks },
    { id: 'cluster', label: 'Clustering', c: Dendrogram },
    { id: 'flow',    label: 'Flow',       c: Sankey },
    { id: 'objects', label: 'Objects',    c: Objects },
    { id: 'source',  label: 'Source',     c: SourceLines },
    { id: 'time',    label: 'Timeline',   c: Timeline },
    { id: 'ask',     label: 'Ask',        c: Ask },
  ];

  // The view lives in the URL fragment, so a view is linkable and survives a
  // reload — the cheapest thing that makes a tool shareable ("look at #flow").
  const fromHash = () => VIEWS.find((v) => v.id === location.hash.slice(1))?.id ?? 'matrix';
  let active = $state(fromHash());
  $effect(() => {
    const on = () => (active = fromHash());
    addEventListener('hashchange', on);
    return () => removeEventListener('hashchange', on);
  });
  const Current = $derived(VIEWS.find((v) => v.id === active).c);

  let health = $state([]);
  $effect(() => {
    query('SELECT finding, n, note FROM link_health WHERE n > 0')
      .then((r) => (health = r))
      .catch(() => (health = []));
  });
</script>

<main class="max-w-[1100px] mx-auto p-6">
  <h1 class="text-lg font-semibold m-0">pcomm</h1>
  <p class="text-[13px] mt-0.5 mb-4" style="color: var(--text-2)">
    multi-GPU communication profile
  </p>

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
