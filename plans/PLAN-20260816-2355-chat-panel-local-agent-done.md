# PLAN — the chat panel: a local agent that answers with SQL and a spec, never with data

- **Created:** 2026-08-16 23:55 +03
- **Status:** DONE (2026-08-17) — steps 0–4 built and measured; step 5
  (container packaging, a `python` tool) deliberately not started, and the
  reasoning for deferring it is unchanged. What the build actually taught is at
  the bottom of this file; the plan body above it is left as written.
- **Supersedes:** step 5 of
  [`PLAN-20260807-1718-ui-backbone-done.md`](PLAN-20260807-1718-ui-backbone-done.md)
  ("the chat panel"), which is four bullets. Its constraints are inherited
  verbatim and are still non-negotiable; this plan is how they get honoured.
- **Depends on:** the derived layer as it exists today (`comm_edges`,
  `object_access`, `endpoint`, `timeline_span`, `wr_flow`, `link_health` in
  `pcomm-read/link_sql.hpp`) and `POST /query` in `pcomm-ui/main.cpp`. Neither
  needs to change for v1.

## The shape of the thing

A side pane in `pcomm ui`. You type "which rank pair moved the most bytes, and
was it lopsided?" and you get prose, the SQL that produced it, and — when a
picture is the answer — a chart.

The agent is [`pi`](https://github.com/earendil-works/pi-mono) (already
installed on simurgh-base, v0.84.1, `~/.local/bin/pi`) running headless in RPC
mode against the local llama.cpp router. It gets three tools of ours and none
of its own. `LOCAL_LLM_ENDPOINT=http://simurgh-base:10000/v1` is already in the
environment, `pi`'s default provider/model is already `simurgh` /
`muse-glimmer-30b` (`~/.pi/agent/settings.json`), and that 30B is the model this
is designed for — big enough to hold a schema and drive tools, small enough that
everything below assumes it will get things wrong and must be caught.

## The one decision that shapes everything else

**The agent never returns rows to the browser.** A chart it produces is
`{ sql, spec }` — a Vega-Lite spec with an empty `data`, plus the query that
fills it. The page runs that SQL through `query()` in `ui/src/lib/query.js`,
exactly like every curated panel does, and binds the rows into the spec itself.

This is not fussiness. Three things fall out of it:

1. **The derived layer does not fork.** `ui/README.md`'s central rule is that
   every panel's numbers come from one path. An agent that computes an aggregate
   in a sidecar and posts the answer as text is a ninth panel with a private
   pipeline — the exact failure the UI was designed against, arriving through a
   door nobody guarded.
2. **The WASM seam survives.** `{sql, spec}` is transport-independent. In the
   static/DuckDB-WASM build the same pair renders against the in-page database
   with no change to the agent at all.
3. **Every answer is checkable.** The SQL is shown next to the chart with an
   "open in Ask" button that drops it into the existing Ask panel. A wrong
   answer from a 30B model is not a crisis if the user can see the query that
   produced it in one click; an unfalsifiable paragraph of prose is.

The corollary is that the chat pane is **not** a data path, and `ui/src/lib/agent.js`
is the one file allowed to talk to the agent — the same rule as `query.js`, for
the same reason.

## Architecture

```
browser (page from :8777)
  ├── query.js ──POST /query──────────────► pcomm ui  (C++, DuckDB, unchanged)
  └── agent.js ──POST /ask, GET /events───► bridge    (node, :8778)
                                              │  stdin/stdout JSONL
                                              ▼
                                           pi --mode rpc  -e pcomm-agent.ts
                                              │  sql / schema / chart tools
                                              ├──POST /query──► pcomm ui
                                              └──OpenAI API───► llama.cpp :10000
```

Four properties worth naming:

- **The C++ binary does not change in v1.** No `/chat` endpoint, no node
  embedded, no rebuild to iterate on prompts. If the bridge isn't running the
  chat pane says so and every other panel is unaffected.
- **The agent runs where the human is, not where the capture is.** On MN5 the
  capture and `pcomm ui` are on a login node behind a forward; the bridge, `pi`
  and llama.cpp run on simurgh-base where the GPU is. The tools reach the
  capture through the same forwarded `/query` the browser uses. Nothing large
  moves.
- **Two loopback ports, so CORS is real.** The page is `127.0.0.1:8777`, the
  bridge is `127.0.0.1:8778`. The bridge sends
  `Access-Control-Allow-Origin: http://127.0.0.1:<ui-port>` and binds loopback
  only, with the same trust argument `pcomm-ui/main.cpp` already writes down.
  In `pnpm dev`, vite proxies `/agent/*` alongside `/query`.
- **The sidecar is optional and separately versioned.** It lives in a new
  top-level `agent/`, is node + `pi`, and is not in the cmake build.

### Rejected: pcomm ui spawns the agent

Tempting (one port, one process to start) and wrong for the cluster case: it
would put the model call on the login node, which has no GPU and, on MN5, no
route out. Keeping the agent on the workstation is what makes the remote-capture
case work at all. A convenience `pcomm ui --agent <url>` that only tells the
page where to look is fine later; spawning is not.

### Rejected for v1: the docker container with python + duckdb

This was the starting proposal and it is the right *eventual* shape for one
specific thing — a `python` tool for the analysis SQL cannot do (clustering,
fits, distribution tests). It is wrong as the v1 architecture:

- **MN5 has no docker.** If the container is where the agent lives, the agent
  does not exist on the machine that matters. (Running it on the workstation
  instead is fine — which is exactly the architecture above, and then the
  container is packaging, not design.)
- **A python tool with its own duckdb connection is a second analysis path**,
  and it will disagree with the UI's numbers the first time it rounds a
  nanosecond. See the 256 ns ulp note in `ui/README.md`: that bug shipped twice,
  in SQL and in the panel, from one shared misconception. A third copy of the
  data path is a third place for it to come back.
- **Nothing in the question list needs it yet.** Every worked example in the
  catalog below is one SELECT.

So: `agent/Dockerfile` ships in step 5 as a way to *run* the sidecar
reproducibly, and the python tool is the thing that justifies it, once a
question exists that SQL genuinely cannot answer. Not before.

## The tools the model gets

`pi` starts with `--no-builtin-tools` (no bash, no read, no write, no edit —
a chat pane must not be able to touch the filesystem) and three registered by
`agent/pcomm-agent.ts`:

| tool | in | out | notes |
|---|---|---|---|
| `schema` | optional table name | column names + types + the catalog's prose for that table | no args = the table list with one line each, non-empty tables first |
| `sql` | a SELECT | up to 50 rows, plus the true row count | wrapped in `SELECT * FROM (…) LIMIT 51`; rejects non-SELECT before it leaves the sidecar; on a DuckDB error the message goes back verbatim so the model can fix its own column name |
| `chart` | `{ title, sql, spec }` | ok / validation errors | validated, then streamed to the page as a chart message |

`sql` capping at 50 rows is the "the model never sees raw rows" constraint made
operational: it sees aggregates it asked for, never a capture dump. It is also
the context budget — 131 K sounds like a lot until a `gpu_access` scan lands in
it.

**Retry on error, three times, then stop.** A hallucinated column is the single
most likely failure and it is self-correcting if the DuckDB error text comes
back; a loop that never gives up is how a local model burns ten minutes on a
typo.

### Chart validation

The spec is Vega-Lite (`reference/vega-lite-6.4.3/` is already vendored — read
its `src/` before deciding a figure is unreachable, per the UI plan's own
warning that the first such conclusion was wrong). Validated in the bridge
before the page sees it:

- `data` must be absent or `{"name": "rows"}` — **`data.url` and inline `values`
  are rejected**, which is the rule that keeps the {sql, spec} split honest and
  stops a spec from fetching anything.
- mark in a whitelist (`bar line point rect area tick rule text arc`);
  encodings in a whitelist; `transform` limited to `aggregate/bin/filter/calculate`.
- no `signals`, no `params` with `bind`, no `expr` outside the allowed
  transforms. Vega's expression language is not JavaScript, but it is a language
  and this page has no reason to run one.
- colour comes from the page, not the model: the spec's `scale.range` is
  overwritten with the tokens in `ui/src/lib/viz.js`, and sequential ramps are
  reversed for dark mode by the same code every other panel uses. A model that
  picks `"viridis"` should not be able to produce the dark-mode inversion bug
  the matrix already survived once.

If the 30B turns out to be unreliable at free-form specs — measure it in step 4,
do not assume either way — the fallback is a `kind` shorthand
(`bar | line | heatmap | scatter` + which column is which) that the bridge
expands into a spec. Vega-Lite stays the wire format either way, so this is a
narrowing of the model's freedom, not a rewrite.

## What the model is told

`agent/catalog.md`, version-controlled, is most of the system prompt. It is the
half of this project that decides whether it works, and it is the half that
needs no model to write.

- **Per table:** what a row *means* in the domain, not just its columns. That
  `comm_edges` is one directed rank→rank aggregate; that `object_access` is
  Snoopie's per-object binning; that `timeline_span` is HUGEINT wall-ns.
- **The gotchas, stated as rules:** wide integers arrive as strings and must be
  compared as integers, not cast to DOUBLE (the 256 ns ulp, with the one-line
  demonstration from `ui/README.md`); `dst_rank = -1` means *unresolved peer*
  and must be surfaced, never silently dropped; underscore-prefixed views are
  internal plumbing and are not for answering questions; tables can exist and be
  empty because the capture lacked that provider.
- **A capture preamble computed at session start** — providers present, rank and
  node count, wall duration, which tables have rows. Five cheap queries, and the
  difference between a model that knows `source_line` is empty in this capture
  and one that writes a beautiful query against nothing.
- **Eight to twelve worked examples**, question → SQL, drawn from the queries the
  curated panels already run. The panels are the best prompt material in the
  repo: they are known-correct SQL against these exact views.

## Steps

**Step 0 — the catalog.** `agent/catalog.md` and the preamble queries. No agent
involved; it is documentation, and the Ask panel should link to it regardless of
whether any of the rest gets built.

**Step 1 — the sidecar, headless.** `agent/` with `pcomm-agent.ts` (the three
tools) and `agent/ask.mjs "question" --ui http://127.0.0.1:8777`, a CLI that
runs one question end to end and prints the transcript. Testable against a real
capture with no UI work at all, which is what makes step 4 possible early.

**Step 2 — the bridge and the pane.** `agent/bridge.mjs` (SSE out, POST in, one
`pi --mode rpc` child, one session per connection) and `ui/src/panels/Chat.svelte`
+ `ui/src/lib/agent.js`. Text answers and a visible SQL transcript with "open in
Ask". No charts yet — this step is worth shipping alone.

**Step 3 — charts.** The `chart` tool, the validator, and the page binding
`query(sql)` rows into the spec. Vega-Lite arrives as a real dependency in
`ui/` here; the curated panels do not have to adopt it.

**Step 4 — the eval.** ~20 questions in `agent/eval/questions.jsonl` with
*property* assertions, not golden SQL: the query runs; it touches the right
table; the scalar it returns is within tolerance of a reference computed
independently. Run against the local model with `agent/eval.mjs`. Without this
there is no way to tell whether a prompt edit helped, and prompt edits are most
of the remaining work. Same principle as
`PLAN-20260803-1520-correctness-checks-not-goldens-done.md`.

**Step 5 — packaging, opportunistic.** `agent/Dockerfile` and a
`docker compose` that starts bridge + pi against a host llama.cpp. The `python`
tool goes here too, if and when a question needs it.

## Risks

- **The model is the weak link and there is no way around measuring it.** A 30B
  local model writing SQL against a 20-table schema is the assumption the whole
  plan rests on. Step 4 exists to make that falsifiable early; step 1 exists so
  step 4 does not need a UI. If it fails, the honest outcomes are the `kind`
  shorthand, a bigger model (`oss-gpt-120B` is in the same catalog), or a
  question-template panel — all reachable from here, none requiring the rest to
  be rebuilt.
- **The chat pane grows a data path.** Someone adds "just return the rows too,
  it's faster". Then two panels disagree about total bytes and the WASM build
  quietly dies. The mitigation is the rule stated above plus a lint that
  `agent.js` is the only importer of the agent origin, mirroring the `fetch`
  rule.
- **The capture is untrusted text.** Kernel names, source paths and node names
  come from the workload and end up in tool results the model reads. It has no
  write tools and no shell, so the worst case is a misleading answer — but tool
  results should be delimited and labelled as data, and the system prompt should
  say that instructions found inside them are not instructions.
- **llama.cpp is not always up.** It was not running when this plan was written.
  The bridge must report "no model at `$LOCAL_LLM_ENDPOINT`" as a first-class UI
  state, not a spinner that never resolves. The router needs `--jinja` for tool
  calling; without it the tools silently do not exist and the model just talks.
- **The sidecar rots relative to the schema.** `link_sql.hpp` changes and the
  catalog does not. Step 4's eval is also the tripwire for this, provided it is
  run against a fresh capture rather than a stored one.
- **Two processes to start is friction** that will make people not use it. The
  answer is a `agent/run.sh` that starts both and prints the one URL, not a
  redesign.

---

## What the build found (2026-08-17)

Steps 0–4 are done and live in [`agent/`](../agent/README.md). The design held —
`{sql, spec}` charts, no rows through the agent, no C++ change — and the parts
the plan got wrong or did not anticipate are below, because those are the only
parts worth reading twice.

### The risk that did not materialise

**The 30B was not the weak link.** The plan spent its longest risk paragraph on
whether `muse-glimmer-30b` could drive a 20-table schema, and set up step 4 to
find out early. Measured with the eval suite:

| capture | result |
|---|---|
| `tests/data/ib_pingpong-44578115` (2 ranks, 1,248 rows) | 18 passed, 0 failed |
| `scale_hubspoke_ib_kt-44580935` (64 ranks, 19 M rows) | 17 passed, 0 failed, 1 skipped |

Typical answer takes 4–20 s and two to five queries. It writes correct DuckDB
against the derived views, checks `link_health` unprompted, and — the case the
plan worried about most — says "this capture has no NVLink data" instead of
inventing a number, on all three empty-table cases. The `kind`-shorthand
fallback for charts was never needed: free-form Vega-Lite specs passed the guard
first try in every chart case.

Two caveats on that. It is one model on two captures of one workload family, and
the model **narrates**: it restates its SQL and dumps result tables into prose
that the instructions tell it not to. That is cosmetic and is prompt work, but
it is the visible weakness, not correctness.

### The eval's first failure was the eval's fault

`median-latency` failed once on the 64-rank capture, and the cause was the
question: on a capture that is 99.6 % `RDMA_WRITE`, "median send latency"
legitimately means either every work request or only `opcode = 2`. The model
computed both and reported the subset. The question was sharpened to name
`wr_flow` explicitly; three re-runs then passed.

Worth stating because it is the standing hazard of this kind of eval: an
ambiguous question measures the model's guess at what you meant, and fixing it
by loosening the assertion would have hidden a real class of error.

### Four things the plan did not predict

- **`POST /query` 400s on a query beginning with a comment.** Found by running
  every worked example in the catalog: the first pass failed all thirteen,
  because each began with a `--` line explaining it. The agent path is immune
  (comments are stripped, and the row-cap wrapper puts `SELECT` first), so this
  is only the human Ask panel. Recorded in `things-to-circle-back-to.md` rather
  than fixed, since this work changed no C++.

- **Two captures on one machine will silently cross-wire.** The page defaults to
  the bridge on a fixed port, so a second `pcomm ui` would have got confident
  answers about the *first* capture — every number right, every number about the
  wrong run. Two guards now: the bridge accepts only the origin it was started
  with (CORS keyed on `--ui`), and the pane compares the bridge's reported
  capture against its own origin and refuses on a mismatch. The CORS one fires
  first in practice, which makes the pane's "no agent" message misleading unless
  it also mentions origin refusal — it does.

- **Vega-Lite `structuredClone`s the spec**, which throws on a Svelte `$state`
  proxy — and the spec always arrives inside one. Flattened in `chart.js`, at
  the library boundary rather than the call site.

- **Vega is 800 KB of bundle for a feature most sessions never open.** Loaded on
  first chart via dynamic import: the base page stayed at 133 KB, the embedder
  handles the extra chunks (5 assets, 950 KB), and the lazy chunk serves out of
  the binary correctly.

### Where the numbers stand

Rendering was checked in both themes — a dark-only check is how the inverted
ramp survived its first review — and the categorical palette added to `viz.js`
was validated against both surfaces (worst adjacent CVD ΔE 9.1 light / 8.4 dark;
three light-mode slots sit under 3:1, which the always-present legend and the
query under every chart cover).

### Step 5, still deferred

Nothing measured here argues for the container or a `python` tool. Every
question in the eval, and every question asked by hand, was one SELECT. Revisit
when a real question needs statistics SQL cannot express — that is what would
justify the second analysis path, and nothing else does.

---

## Follow-up (2026-08-17, after review of real answers)

Three things came out of reading what the agent actually said, rather than
whether it passed. Each produced a fix and an eval case, because a failure with
no test is a failure that comes back.

### It summed overlapping spans and called the result "runtime"

Asked "what dominates the runtime?", it answered `ib_wr_flight`, 97.6 %. The
arithmetic was right and the conclusion was backwards. Summing `t1 - t0` over
`timeline_span` adds intervals that ran *concurrently*: the total came to
43,548 s of a job whose 64 ranks only have 5,276 s of wall clock between them —
**8.25× more time than exists**. Merging the intervals per rank inverts the
ranking completely:

| stream | summed | merged occupancy | share of rank-wall |
|---|---|---|---|
| `gpu_kernel` | 1,069 s (reported as 2.4 %) | 1,068.7 s | **20.3 %** |
| `ib_wr_flight` | 43,548 s (reported as 97.6 %) | 26.1 s | **0.5 %** |

`ib_wr_flight` collapses by 1,670×, which *is* the finding: ~1,670 work requests
are outstanding at any moment. The network is deeply pipelined, not slow. And
`gpu_kernel`'s sum survives merging untouched — kernels never overlap within a
rank, so they are the serialised resource. Spans of any kind cover only 20.3 %
of rank-wall, so ~80 % of the runtime is accounted for by nothing in the
capture, which the answer should say.

Catalog rule 7 now states that spans overlap, that the streams are *nested*
(a flight span contains its own post and completion), gives the merged-occupancy
query, and requires the sanity check that no stream can exceed 100 %. The
`dominates` eval case asserts the top stream by merged occupancy — computed
independently by the harness — and fails any answer claiming a percentage above
100.

Worth keeping in mind for the rest of the derived layer: **`wr_flow.latency_ns`
is exposed to the same misreading**, and any future panel that sums it will
reproduce this bug in SVG instead of prose.

### The agent could not name a host or a GPU, and that was our fault

"Which host/GPU pair did the most remote accesses?" was unanswerable: the label
`n14 · GPU 0` was not in any table. `Devices.svelte` built it inline from
`_node` and `_rank_device` — internal views the agent is told never to read —
and `Graph.svelte` ordered its arcs by the same private `dense_rank`. Three
consumers, three derivations, one of them impossible.

Fixed where `ui/README.md` already said it belonged: **`rank_identity`** in
`link_sql.hpp` (`rank, node_ix, node_hash, device, devices, label`), and
`Devices.svelte` now reads it instead of deriving it. This is the first C++
change of this work, and it is the case that earns one — the alternative was
teaching the model a recipe over unstable internals.

What it still cannot do, by construction: name the actual machine. `node_hash`
is FNV-1a of gethostname, one-way, so `n14` is an ordinal and the catalog says
so rather than letting the model invent a hostname.

*If a hostname is ever wanted, it has to be recorded — that is a capture-format
change, not a query.*

### Two UI bugs the eval could never have caught

- **The second "open in Ask" silently did nothing.** The Ask panel read the
  handed query once at mount, so it worked when switching to a panel that was
  not open and failed every time after. Now a `pcomm:ask` event as well, which
  covers the already-mounted case; sessionStorage still covers the other.
- **Questions and answers read as one wall of text.** The question now gets a
  raised surface and a left rule — differing in shape, not only colour.

Both were found by a person using it, which is the standing limitation of the
eval: it checks whether answers are true, and nothing about whether the pane is
usable.

### A second reading, and a rule that was written too narrowly (2026-08-17)

Asked "when does the first kernel run? and what is consuming most of the
timeline?", the agent got the first half right (61.80 s after capture start,
rank 53 — verified) and the second half wrong again, in a new way and for an
instructive reason.

**The rule did not fire because of its wording.** Rule 7 said "to answer *what
dominates the runtime*, merge the intervals". The question said "consuming most
of the timeline", the model did not connect the two, and it went back to summing
overlaps. A rule keyed to one phrasing of a question is a rule that only works
when the question is asked that way; it now names the whole family (dominates,
consuming, bottleneck, slow, where the run spends itself).

**And it invented a second wrong metric.** It computed
`max(t1_ns) - min(t0_ns)` per stream and called it *"wall-clock coverage"*. That
is the stream's **extent**, not its coverage, and the two diverge hardest
exactly where it matters: `ib_post_recv` has the widest extent in this capture
(81.4 s, the full run) and an occupancy of **0.1 s across all 64 ranks** — the
emptiest stream wearing the widest span. On that mislabel it concluded "the
timeline is IB dominated", which is backwards on every honest measure. Rule 7
now says extent is not coverage and warns that setup traffic at both ends of a
run gives the idlest streams the widest extents.

**What the capture actually says**, and neither answer found it:

| phase | share of wall | what is in it |
|---|---|---|
| 0 → 61.8 s | 75 % | almost nothing — 0.1 s of `ib_post_recv` across 64 ranks |
| 61.8 → 82.4 s | 25 % | `gpu_kernel` **80.9 %** busy per rank, `ib_wr_flight` 2.0 % |

The last kernel ends exactly at the capture's final nanosecond, so the run is
three-quarters idle setup followed by a 20.6 s burst in which the GPUs are
saturated. "GPU kernels are 20 % of the run" is true and hides this, so the
catalog now asks for *when* alongside *how much*: compare each stream's extent
to the capture's and report the phase when they differ.

Two eval cases added — `consuming` (the paraphrase that slipped through) and
`first-kernel` — and the chart guidance now calls for a log scale when values
span orders of magnitude, because three answers in a row drew a linear bar chart
with one visible bar and five invisible ones.

**The pattern worth carrying forward:** both bad answers came from a *measure*
being wrong while every number in them was arithmetically correct. That is the
failure mode this agent has, and the eval only catches it where a reference is
computed independently — so any new question about time needs one.

### Model comparison: muse-glimmer-30b vs gemma4-31b-qat (2026-08-17)

Run on the 64-rank capture, same catalog, same tools. Two questions with two
trials each, plus the full 23-case suite per model.

**The suite does not separate them: 22 passed / 0 failed / 1 skipped, both.**
That number is only meaningful after three eval bugs were fixed, and all three
were the harness scoring *surface form* rather than correctness — each one had
failed gemma for an answer that was right:

- `failures` demanded the numeral `0`; gemma wrote "No work requests completed
  with an error status".
- `chart-per-rank` counted only `sql` tool calls; gemma went straight to a
  correct chart, whose query the chart tool runs anyway.
- `dominates` / `consuming` matched the literal token `gpu_kernel`; gemma wrote
  "GPU kernels".

The checks still verify the substantive claim; what changed is that they no
longer require a particular way of writing it. **A property-based eval over
prose needs normalisation, and every un-normalised check is a hidden style
preference** — here it was silently favouring the model the catalog examples
were written against.

**Where they did differ** — the deliberately hard question: "rank 0 sends ~7.7×
more bytes than any spoke; does it pay for that in time, is it the bottleneck?"
The trap is that rank 0's median work-request latency is 30.4 ms against the
spokes' 1.8 ms (16.8×), while its GPU-kernel occupancy is 16.7 s — *identical*
to every spoke. The honest answer is "no": the latency is queueing, the run's
makespan is set by kernels, and the hub keeps up.

| | glimmer 30B | gemma 31B |
|---|---|---|
| hard question | 2/2 correct | 1/2 — one trial called it "the clear bottleneck" from latency alone, and again read a stream's extent as its occupancy |
| the previously-broken timeline question | 2/2 | 2/2 |
| queries per answer | 4–25 | 2–6 |
| seconds per answer | 32–76 | 10–47 |
| style | full working, tables, volunteers charts | ~350-char answers, charts only when asked |

Two observations worth more than the scores:

- **Glimmer found an anomaly nobody asked about.** It flagged rank 11 as a
  communication hot spot; verified, rank 11's p99 latency is **15.03 s** — more
  than 800 of its 82,304 work requests took ~15 seconds, while its median is
  1.8 ms like every other rank. That is the most interesting thing in this
  capture and no panel surfaces it.
- **Gemma follows "do not narrate" better.** Its timeline answer was 342
  characters and fully correct including the phase confinement; glimmer's was
  1,538 with tables and a chart. Over-narration is glimmer's standing weakness
  and gemma simply does not have it.

**Conclusion, with its limits stated:** on measured correctness they tie, and
gemma is 2–3× faster and much terser. The single signal favouring glimmer is
one trap question at n=2 — real but thin. Anyone choosing between them should
add trap-shaped cases (a wrong *measure* with right arithmetic) and run more
trials; that is the failure mode this agent has, and the current suite contains
only three such cases.

**Decision (2026-08-17): glimmer adopted**, and pinned in `lib/session.mjs`
rather than left to pi's global default — the catalog is tuned against one model
and an eval number attached to "whatever pi was configured for that day" is not
a measurement. `$PCOMM_AGENT_MODEL` or `--model` overrides it. Revisit when
there are more trap-shaped cases to decide on, or a new model to try.

### A stale bridge, found while pinning the model

Pinning the model surfaced something worse than the thing it was checking:
`/health` kept reporting the old value after a restart, because **the bridge
from the first run of the day had never died**. It held port 8778 for the whole
session; every later `agent/run.sh` started a bridge that failed to bind and
exited quietly while `run.sh` waited on the other child.

The visible symptom was a wrong field in `/health`. The real one was that the
bridge builds its system prompt at startup and cached it forever: `pcomm ui` had
since been restarted on a different capture behind the same port, so the model
was being told "2 ranks" about a 64-rank run. Its **queries still executed
against the real data, so the answers were right and the description it reasoned
from was false** — the worst shape for this kind of bug, because nothing looks
wrong.

Both halves fixed: the prompt is rebuilt per session (~1 s, and it means a
restarted capture is picked up), and the server now handles `EADDRINUSE` with a
message naming the likely cause instead of dying silently.

Worth remembering that the UI checks earlier in the day ran against that stale
bridge. They were checks of streaming, chart rendering and hand-off — all UI
mechanics, unaffected — and every catalog and eval result came through
`ask.mjs` / `eval.mjs`, which build a fresh prompt per run. But it is luck that
the overlap was harmless, not design.
