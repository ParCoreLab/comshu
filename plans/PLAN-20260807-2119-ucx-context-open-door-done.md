# PLAN — find the door UCX opens its ibv_contexts with

> **Closed 2026-11-09 (backlog triage).** The door was mlx5dv_open_device, interposed in 197f633; the expect_fail markers came off in 82803a2. The status below is historical.

- **Created:** 2026-08-07 21:19 +03
- **Status:** TODO. Diagnosed to a hypothesis with two instruments already in
  the tree; the door itself is not yet named.
- **Blocks:** `entity_close_accounted` on cases 02, 06 and 07, currently
  `expect_fail` with the full measurement in each `meta.json`.
- **Spawned by:** `PLAN-20260807-1746-receive-path-not-captured` — stage 1 added
  `ib_completion_queue`, and the moment CQs became visible this fell out.
- **Prior art, and it is the same bug twice already:**
  `PLAN-20260805-0053-perftest-qp-creation` (the door was
  `verbs_context::create_qp_ex`, a function pointer with no symbol) and
  `PLAN-20260806-2320-dlvsym-interposer-not-observed` (the door was not a
  registration door at all — it was the *context*, and every unmatched close
  carried `installed=NO`).

## What is broken

**30 completion queues per rank are destroyed that nothing recorded creating.**

```
case 02 (NCCL, 8 ranks)      164 opens / 194 closes per rank   1312 / 1552 per run
case 07 (NVSHMEM, 8 ranks)   164 opens / 194 closes per rank   1312 / 1552 per run
```

Identical on every rank of two unrelated workloads. Cases 01 and 08 are clean
(2/2 and 4/4), so it is not the close hook and not the record.

## The hypothesis, and the evidence for it

`ibv_create_cq` is a **preload** interposer: live from process start, so a CQ
created through it can never be missed. `verbs_context::create_cq_ex` is an
**object-ops slot**: it exists only after `ensure_installed` has patched that
context. So a CQ created via the extended door *before* its context is
instrumented is invisible, while `ibv_destroy_cq` — preload again — still sees
every close. That asymmetry is exactly the observed shape.

Measured on MN5 job 44400676:

```
[pcomm-ctxinstall] door=create_cq      contexts=48
[pcomm-ctxinstall] door=open_device    contexts=25
[pcomm-ctxinstall] total=73 late=48

[pcomm-unmatched] destroy_cq cqe=8191 ctx=... installed=yes
                  <- uct_ib_verbs_destroy_cq in .../ucx/mt/lib/ucx/libuct_ib.so.0
```

**48 of 73 contexts per rank never come through `ibv_open_device`.** They are
opened somewhere this provider does not interpose, and their first hooked call
is `ibv_create_cq`. Anything they did before that — including `create_cq_ex`
calls — happened with an unpatched slot.

`IbCompletionQueue.origin` (added for this hunt) confirms the extended door is
real and worth having: of 1312 recorded CQs, **480 came through `create_cq_ex`**
(60/rank). Hooking it captured those; it could not capture the ones that
predate installation.

### One reading to NOT repeat

`trace_unmatched_destroy_cq` prints `installed=yes`, and that is **not** evidence
against the install-window hypothesis. The flag is sampled at **destroy** time,
by which point the context has long since installed at `create_cq`. I read it
the wrong way round once already and wrote it into a status report before
catching it. **Sampling installed-ness at CREATE time is the first improvement
this hunt needs** — until then the flag says nothing either way.

## What to do

### 1. Make the instrument answer the question it was built for

Record, per context, whether it was installed at the moment each CQ was created
— not at destroy. Cheapest form: stamp `ensure_installed` with a monotonically
increasing install epoch, and have `trace_unmatched_destroy_cq` report whether
the context had *any* install at all before the CQ's address was first seen. If
every unmatched CQ belongs to a context whose install came later, the window is
proven and step 2 is the whole fix.

### 2. Find the opening door

`libuct_ib.so.0` is the destroyer. The candidate nobody has examined is
**`mlx5dv_open_device`**, which this provider does not interpose and which the
source comments in `ibverbs_provider.cpp` already flag as a possible gap
(`g_installed_ctxs`' comment names it explicitly).

**Do not settle this with `nm`, `readelf` or `LD_DEBUG=bindings.** All three
answer questions about *symbols*, and that is precisely how case 08 lost a week
to a `static inline`. Trace hook **entry** instead (`PCOMM_TRACE_HOOKS=1`), or
follow the *object* rather than the symbol — which is what finally cracked the
`reg_mr` hunt after six hypotheses died.

Note `mlx5dv_open_device` IS an exported symbol, unlike `create_qp_ex`, so if
that is the door a plain preload hook reaches it.

### 3. Close the loop

Drop the `expect_fail_asserts.entity_close_accounted` marker from cases 02, 06
and 07 and let the assertion guard the fix. It must go green **without being
weakened** — that is the standard `qp_creation_accounted` set on case 08, and
the standard this repo has held twice.

## Constraints

- **The window is a floor, not a bug in itself.** Every object-ops hook in the
  tree inherits it. Fixing the CQ symptom by making `ibv_destroy_cq` suppress
  closes it cannot match would delete the only signal that finds the next door —
  the exact thing `entity_close_accounted`'s own comment forbids.
- **Installation must stay keyed on "is this context already patched" and
  nothing else.** Re-installing saves our own trampoline as the original and
  recurses; and any new opening door must call `forget_context` on the matching
  close, or a recycled context pointer is mistaken for one already patched. That
  omission silently cost case 02 two of its eight ranks once already.
- **This is not only about CQs.** If contexts are opened through an unhooked
  door, *every* object-ops hook is blind on them for that window — `post_send`,
  `post_recv`, `poll_cq` included. CQs are merely where it became visible,
  because they are the one object created from a context rather than a PD. The
  prize is bigger than 30 rows per rank.

## Wider gap worth folding in, or splitting out

An application polling through the **extended `ibv_cq_ex` API**
(`start_poll`/`next_poll`/`read_*`) never touches `ctx->ops.poll_cq` — the exact
mirror of the `ibv_wr_*` blind spot that made perftest's sends invisible until
`qp_to_qp_ex` pointer-patching. Then `ib_poll_cq` is empty and
`recv_completion_accounted` goes *quiet* rather than red. pingpong and perftest
both use the classic path so it is not biting today, but the fix shape is known
and it is the same family of hole as everything above.
