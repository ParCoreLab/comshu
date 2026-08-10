# ui/ — the pcomm frontend

Svelte 5 + Vite + Tailwind. Built to `dist/`, which `pcomm-ui` serves.

```bash
pnpm install
pnpm build                                   # -> ui/dist

# serve a real capture through the built app
pcomm-read <trace-dir> --parquet /tmp/x
pcomm-read --link /tmp/x
pcomm-ui /tmp/x --dir ui/dist                # http://127.0.0.1:8777

pnpm dev                                     # vite on :5180, /query proxied to :8777
```

`pnpm dev` proxies `/query` to a running `pcomm-ui` rather than mocking it. The
data path is the part most worth exercising, and a fixture would be the one
thing never tested against a real capture.

## Panels

| view | source | state |
|---|---|---|
| Matrix | `comm_edges` | rank×rank heatmap, log scale, unresolved peers in their own column |
| Ranks | `comm_edges` | per-rank sent/received/peers |
| Clustering | `comm_edges` | dendrogram, cosine distance over bytes-per-peer |
| Flow | `ib_post_send` + `mlx5_device_send` | sankey: transport → opcode → size class |
| Objects | `mem_object` + `gpu_access` | per-object 1D access heatmap, after Snoopie's object view |
| Source | `source_line` | **empty on every current capture** — needs gpumem with PC→line |
| Timeline | event counts | deliberately not a timeline; hands off to Perfetto |
| Ask | any | SQL console; the NL layer will emit into this same path |

The view lives in the URL fragment (`#flow`), so a view is linkable and survives
a reload. One panel renders at a time — eight panels is eight queries, and
rendering all of them scans the capture eight times to show one.

## The rules this app is built on

**Every panel gets its data from `src/lib/query.js` and nowhere else.** No
component imports `fetch`. Two reasons: a panel with its own server-side
endpoint forks the derived layer, and two panels then disagree about total bytes
with no single place to fix it; and the static / DuckDB-WASM build swaps the
transport in that one file instead of hunting fetches through a grown UI.

**If a panel needs a table that does not exist, it belongs in
`pcomm-read --link`, not in the panel.** That is what keeps every view a
projection of one model rather than eight private pipelines.

**Colour is tokens; Tailwind does layout.** The palette in `src/app.css` is the
validated set from the data-viz reference palette, and its light/dark pairs have
to swap as a set — `src/lib/viz.js` reads the same steps for SVG fills.

**A sequential ramp is defined against its surface.** `viz.js` reverses it for
dark mode, and that is not a preference: the first matrix mapped the largest
values to the darkest step, so on a dark background a 1.3 GB diagonal read
*quieter* than the 158 KB cells beside it. `?theme=light|dark` exists so both
modes stay checkable — a dark-only render is how that bug survived its first
review.

**Numbers arrive as strings.** A JSON number cannot hold a UBIGINT address
without dropping the low bits, and those addresses are join keys. Cast what you
need with `num()`; most wide columns should never be cast at all.
