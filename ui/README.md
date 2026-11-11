# ui/ — the comshu frontend

Svelte 5 + Vite + Tailwind. Built to `dist/`, which is **compiled into**
`comshu ui` — the server has no other page, and a copy of the binary on a login
node carries the whole app with it.

```bash
npm ci                                       # exactly what package-lock.json pins
npm run build                                # -> ui/dist, embedded at cmake build time

comshu ui <trace-dir>                         # http://127.0.0.1:8777 — that is the whole flow

npm run dev                                  # vite on :5180, /query proxied to :8777
comshu ui <trace-dir> --dir ui/dist           # serve from disk instead of the embedded copy
```

Neither of the first two is something you have to remember: a build with
`-DCOMSHU_UI=ON` runs `tools/build_ui.sh`, which installs when the lockfile
changed and then builds. `ui/dist` is a build artifact and is not committed, so
the build produces it. No export step either, and no `comshu export`/`comshu link`
first, since `comshu ui` loads a capture directly.

**npm, not pnpm**, which this used to use. A spack `+ui` build installs these
packages itself, and spack has an npm package but no pnpm one at all. Measured
before switching: three of the four output chunks come out byte-identical, and
only the app chunk differs, by 725 bytes. There is one lockfile,
`package-lock.json`, because two lockfiles over one `package.json` is how the
build and the developer end up installing different things.

`npm run dev` proxies `/query` to a running `comshu ui` rather than mocking it. The
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
| Timeline | `timeline_span` | canvas, pan + zoom; density bins zoomed out, real spans zoomed in |
| Ask | any | SQL console; the chat pane hands its queries here to be edited and re-run |

Plus one thing that is not a panel: the **Chat** pane (`src/panels/Chat.svelte`),
a side pane rather than a tenth view because it is not a view of the capture —
it is a way of getting to the views, and it stays open while you read them. It
needs `agent/bridge.mjs` running beside this server and says so when it is not.
See [`../agent/README.md`](../agent/README.md).

The view lives in the URL fragment (`#flow`), so a view is linkable and survives
a reload. One panel renders at a time — eight panels is eight queries, and
rendering all of them scans the capture eight times to show one.

## The rules this app is built on

**Every panel gets its data from `src/lib/query.js` and nowhere else.** No
component imports `fetch`. Two reasons: a panel with its own server-side
endpoint forks the derived layer, and two panels then disagree about total bytes
with no single place to fix it; and the static / DuckDB-WASM build swaps the
transport in that one file instead of hunting fetches through a grown UI.

**The chat pane may not return data either.** `src/lib/agent.js` is the only
file that talks to the agent bridge, and what comes back is prose plus
`{ sql, spec }` — never rows. A chart's data is fetched by the panel through
`query()` like everything else, so the model's numbers come down the same pipe
as the panels' numbers and can be checked against them. An agent that computed
its own aggregates would be a ninth panel with a private pipeline, which is the
failure this file exists to prevent, arriving through a door nobody guarded.

**If a panel needs a table that does not exist, it belongs in
`comshu link`, not in the panel.** That is what keeps every view a
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

**A wall-clock nanosecond is one of those wide columns.** It is ~1.79e18, and a
float64 — which is what both a JS number and a SQL `DOUBLE` are — has a **256 ns
ulp** at that magnitude. The timeline's median span is 122 ns, so the first
version of it rounded every duration in the capture to zero width, in SQL and
again in the panel. Both halves are fixed the same way: keep the absolute time
in integers (`HUGEINT` in `timeline_span`, an un-`Number()`ed string in the
panel) and let the float touch only small deltas. `(1786575955340045312::DOUBLE
+ 122)::BIGINT` returns its input unchanged — that is the whole bug in one
expression, and it is worth running once before writing anything time-shaped.
