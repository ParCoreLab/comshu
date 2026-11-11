# agent/ — the chat pane's sidecar

Ask a capture questions in English. The answer comes back as prose, the SQL that
produced it, and sometimes a chart.

```bash
comshu ui <capture>                                  # the capture, on :8777
node agent/bridge.mjs --ui http://127.0.0.1:8777    # the agent, on :8778
# then open http://127.0.0.1:8777 and press "Chat"
```

Or without a browser, which is how the prompt work is actually done:

```bash
node agent/ask.mjs "which rank pair moved the most bytes?"
node agent/ask.mjs --print-prompt | less           # what the model is told about this capture
node agent/eval.mjs                                # 18 questions, property-checked
```

## What it is

[`pi`](https://github.com/earendil-works/pi-mono) in RPC mode with three tools of
ours and none of its own (`--no-builtin-tools`: no shell, no filesystem, no way
to write anything anywhere). It talks to a llama.cpp router at
`$LOCAL_LLM_ENDPOINT`.

**The model is pinned to `muse-glimmer-30b`** (`lib/session.mjs`), not inherited
from pi's own default: the catalog is tuned against a specific model, and the
eval numbers below mean nothing if the model can change underneath them. Chosen
2026-08-17 over `gemma4-31b-qat`, which ties on the suite (22/23 each) and is
2–3× faster and terser, but was less reliable on questions whose trap is a wrong
*measure* with right arithmetic — this agent's characteristic failure. That
evidence is two trials of one question, and the plan says so. Override with
`--model`, or `$COMSHU_AGENT_MODEL` for every entry point at once.

```
browser (page from :8777)
  ├── query.js ──POST /query──────────────► comshu ui  (C++, DuckDB, unchanged)
  └── agent.js ──POST /ask (SSE)──────────► bridge.mjs (:8778)
                                              │ JSONL over stdio
                                              ▼
                                           pi --mode rpc -e comshu-agent.ts
                                              ├──POST /query──► comshu ui
                                              └──OpenAI API───► llama.cpp
```

The agent runs **where the person is, not where the capture is**. On a cluster
the capture and `comshu ui` sit on a login node behind an SSH forward while the
bridge, pi and the model run on the workstation with the GPU; the tools reach
the capture over the same forwarded `/query` the browser uses. Nothing large
moves, and `comshu ui` needs no change to support any of this.

## Reaching it from another machine

Both halves are loopback by default. To serve them on a private network instead
— a tailnet, so a laptop can open the page on the workstation that holds the
capture — give `run.sh` the address:

```bash
agent/run.sh <capture> 8777 --bind 100.64.0.3
```

One flag, because it sets three things that have to agree, and each one fails
differently when it does not:

| | wrong value | how it fails |
|---|---|---|
| `comshu ui --bind` | loopback | page unreachable — the obvious one |
| `bridge.mjs --bind` | loopback | Chat pane says "No agent at …" |
| `bridge.mjs --ui` | loopback while bound remotely | Chat pane says the agent is reading **a different capture** — a false mismatch, and the one that wastes an afternoon |

The bridge also refuses a non-loopback `--bind` unless you add `--allow-remote`,
and refuses the third row above outright with a message naming the fix. Neither
half has any authentication: anyone who can route to the address can read the
capture and drive the model. `comshu ui` turns DuckDB's external file access off
when bound off loopback; the query endpoint stays SELECT-only.

| file | what it is |
|---|---|
| `catalog.md` | what the tables mean, the gotchas, worked queries — most of the system prompt |
| `lib/prompt.mjs` | catalog + this capture's preamble + how to behave |
| `lib/preamble.mjs` | five queries at startup: ranks, providers, which tables have rows, `link_health` |
| `lib/query.mjs` | the `/query` client, the row cap, the read-only check |
| `lib/spec-guard.mjs` | what a chart is allowed to be |
| `comshu-agent.ts` | the pi extension: `schema`, `sql`, `chart` |
| `lib/session.mjs` | one pi child, one conversation |
| `bridge.mjs` | SSE for the browser |
| `ask.mjs` | one question, one answer, no browser |
| `eval.mjs`, `eval/` | does it actually answer correctly |

## The rule everything else follows

**The agent never returns rows to the browser.** A chart leaves the bridge as
`{ sql, spec }` — a Vega-Lite spec with no data — and the *page* runs that query
through `query.js`, the same path every curated panel uses, and binds the rows
in itself.

Three things fall out of it, and they are the reason it is not negotiable:

1. **The derived layer does not fork.** An agent that computes an aggregate in
   the sidecar and posts the answer as text is a ninth panel with a private
   pipeline — the exact thing `ui/README.md` is written to prevent, arriving
   through a door nobody guarded.
2. **The DuckDB-WASM build still works.** `{sql, spec}` is transport-independent.
3. **Every answer is checkable.** The SQL sits under the chart with a button
   that drops it into the Ask panel. With a 30B model that is not a nicety.

`spec-guard.mjs` enforces the first half by refusing `data.url`, inline `values`
and `datasets` anywhere in a spec, along with `params`, `signal`, `config` and
the other escape hatches. Colour is applied by the page from the same tokens
every other panel uses, so a model asking for `"viridis"` cannot reintroduce the
dark-mode ramp inversion the matrix already survived once.

## The tools the model gets

| tool | in | out |
|---|---|---|
| `schema` | optional table name | the table list with row counts, or one table's columns |
| `sql` | one SELECT | up to 50 rows plus the true row count; DuckDB's error verbatim when it fails |
| `chart` | `{ title, sql, spec }` | validated, field-checked against the query's columns, then streamed to the page |

The 50-row cap is "the model never sees raw rows" made operational: it sees
aggregates it asked for, never a capture dump, and one unguarded scan of
`gpu_access` cannot eat the context window.

## The eval

`node agent/eval.mjs [--ui <url>] [--only id,id] [--verbose]`

Property assertions, not golden SQL — there are many correct queries for "how
many bytes in total", and pinning one measures conformity instead of
correctness. Each case checks that a query ran, that it touched a table which
could hold the answer, that the number reported matches one the harness computes
independently, that a chart appeared when asked for, and — for the cases about
data the capture does not have — that the answer says so instead of inventing a
figure.

References are computed against the live capture, so the suite is not tied to a
fixture. Measured 2026-08-17 with `muse-glimmer-30b`, 21 cases:

| capture | result |
|---|---|
| `tests/data/ib_pingpong-44578115` (2 ranks) | 21 passed, 0 failed |
| `scale_hubspoke_ib_kt-44580935` (64 ranks, 19 M rows) | 20 passed, 0 failed, 1 skipped (no `gpu_kernel`) |

Two cases are there because the agent got them wrong first: `dominates` (it
summed overlapping spans and reported 97.6 % of a runtime that does not exist —
see rule 7 in the catalog) and `host-gpu` (it could not name a host/GPU pair
until `rank_identity` existed). Both now assert against a reference the harness
computes for itself.

Run it after any prompt or catalog edit. It is the only way to tell whether a
change helped, and prompt edits are most of the remaining work.

## Two servers, two ports, and getting them paired

The bridge accepts requests only from the origin it was started with (`--ui`),
so a bridge started for one capture will not answer a page serving another — it
is refused rather than allowed to answer confidently about the wrong run. With
two captures open at once, start a bridge per capture and point the page at the
right one:

```bash
comshu ui capture-a --port 8777 &  node agent/bridge.mjs --ui http://127.0.0.1:8777 --port 8778 &
comshu ui capture-b --port 8779 &  node agent/bridge.mjs --ui http://127.0.0.1:8779 --port 8780 &
# http://127.0.0.1:8779/?agent=http://127.0.0.1:8780
```

The pane also checks the bridge's reported capture against its own origin and
refuses to chat on a mismatch.

## Requirements

- `pi` on PATH (`npm i -g @earendil-works/pi-coding-agent`), node ≥ 20
- a llama.cpp router with `--jinja` — **without it tool calling silently does not
  exist** and the model just talks
- a running `comshu ui`

Nothing here is in the cmake build; the sidecar is optional and the page says so
when it is not running.
