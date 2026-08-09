<script>
  let { title = '', subtitle = '', children } = $props();
</script>

<section
  class="rounded-[10px] border p-5 mb-4"
  style="background: var(--surface-1); border-color: var(--border)"
>
  {#if title}
    <h2 class="text-[15px] font-semibold m-0">{title}</h2>
  {/if}
  {#if subtitle}
    <p class="text-[13px] m-0 mb-3" style="color: var(--text-2)">{subtitle}</p>
  {/if}
  {@render children?.()}
</section>
