import { defineConfig } from 'vite';
import { svelte } from '@sveltejs/vite-plugin-svelte';
import tailwindcss from '@tailwindcss/vite';

// Built to ui/dist, which `pcomm-ui --dir ui/dist` serves. During development
// vite serves the page itself and proxies /query to a running pcomm-ui, so the
// app talks to a real capture rather than a mock — the data path is the thing
// most worth exercising, and a fixture would be the one part never tested.
export default defineConfig({
  plugins: [svelte(), tailwindcss()],
  build: { outDir: 'dist', emptyOutDir: true },
  server: {
    port: 5180,
    proxy: { '/query': 'http://127.0.0.1:8777' },
  },
});
