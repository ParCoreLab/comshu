<script>
  // One tooltip element for the whole page, positioned on demand. Per-mark
  // hover is the default for cell/bar/dot charts, so every panel needs this and
  // none should grow its own.
  let { tip = null } = $props();

  let el = $state(null);
  let pos = $state({ x: 0, y: 0 });

  export function move(ev) {
    const pad = 14;
    const w = el?.offsetWidth ?? 160;
    const h = el?.offsetHeight ?? 48;
    pos = {
      x: Math.min(ev.clientX + pad, innerWidth - w - 8),
      y: Math.min(ev.clientY + pad, innerHeight - h - 8),
    };
  }
</script>

<div
  bind:this={el}
  class="fixed z-10 rounded-md border px-2.5 py-1.5 text-xs pointer-events-none
         transition-opacity duration-75 shadow-lg"
  style="left: {pos.x}px; top: {pos.y}px; opacity: {tip ? 1 : 0};
         background: var(--surface-1); border-color: var(--border); color: var(--text-1)"
>
  {#if tip}
    <div class="font-semibold">{tip.title}</div>
    <div style="color: var(--text-2)">{tip.body}</div>
  {/if}
</div>
