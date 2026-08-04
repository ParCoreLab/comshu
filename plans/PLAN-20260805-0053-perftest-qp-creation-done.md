# PLAN — find the door perftest creates its queue pair with

- **Created:** 2026-08-05 00:53 +03
- **Status:** DONE 2026-08-05. The door is `verbs_context::create_qp_ex`, a
  function pointer rather than a symbol. Hooked as an object-ops slot; MN5 suite
  job 44234365 is 8 PASS + 1 SKIP with case 08 green on 8 assertions, and du04
  case 09 still passes on 9. The assertion was never weakened.
- **Blocks:** case 08's `qp_creation_accounted` assertion, currently
  `expect_fail` with the evidence attached.
- **Related:** `PLAN-20260803-2253-ibgda-device-initiated-capture` (the mlx5dv
  provider that was expected to close this and did not).

## The contradiction

Case 08 captures a queue pair being *driven* and never being *created*:

- `ib_modify_qp` has 5 rows across 2 ranks (qp_num 1452 and 1275)
- `ib_queue_pair` is empty
- `mlx5_queue_pair` is empty
- the ib provider is otherwise healthy in the same process — 1000 `ib_post_send`,
  1010 `ib_poll_cq`, 2 `ib_mem_region`

So a transition refers to a queue pair no record created. That is the invariant
`qp_creation_accounted.sql` states, and it is genuinely violated.

## What has been eliminated

All by measurement (probe job 44205459, `LD_DEBUG=bindings`, plus static
`readelf`), not by argument:

| hypothesis | how it died |
|---|---|
| perftest uses `mlx5dv_create_qp`, we just don't hook it | we hook it and emit `mlx5_queue_pair`; captured nothing |
| symbol versioning defeats unversioned interposers | `ibv_create_qp@IBVERBS_1.1` binds to `libibverbs_trace.so` from `ib_send_bw`, `libibverbs` **and** `librdmacm` |
| RDMA CM bypasses us | `librdmacm` imports only `ibv_create_qp@IBVERBS_1.1`, which resolves to our provider |
| perftest builds the QP with DEVX | zero `mlx5_devx_cmd` records; that hook demonstrably works on du04 |
| the mlx5dv provider never loaded | launcher reported `2 providers → 1 pass · ib+devx`, and `find_so` exits(1) when a provider is missing |
| id-range conflict made the second provider bail | 100-199 and 200-299 are disjoint, `declare_provider` permits it, no engine message |

## Why the instrument ran out

`LD_DEBUG=bindings` shows symbol **resolution**, not **invocation**. Both
`rdma_create_qp` and `rdma_create_qp_ex` bound in the same run and perftest
cannot be calling both. So bindings can prove *"if this is called, it reaches
us"* — which it did, for every door — and cannot prove which was called.

That distinction is the whole reason this plan exists rather than a fix: three
successive explanations were asserted from resolution-level evidence and all
three were wrong.

## Step 1 — call-level evidence

A temporary build with an entry print (env-gated, `PCOMM_TRACE_HOOKS=1`) in
`ibv_create_qp`, `mlx5dv_create_qp` and `mlx5dv_devx_obj_create`, run on case
08's workload. One job. This answers directly which interposer is entered and
how often, and it cannot be confused with a resolution.

Expected outcomes and what each means:

- **One of them fires.** Then the hook is entered and the record is not written
  — a provider bug, and the smallest possible one to find from there.
- **None fires.** Then the queue pair is created by something not in our hook
  set at all, and the print set widens: `ibv_create_qp_ex`,
  `ibv_create_qp_ex2`, and whatever `ibv_devinfo`-adjacent path perftest uses
  when it selects a device. Note `ib_send_bw` does *not* import
  `ibv_create_qp_ex`, so this outcome would itself be surprising and worth
  pausing on rather than patching past.

## The answer (job 44233994)

Step 1 ran as an engine-level entry trace (`PCOMM_TRACE_HOOKS=1`) rather than
three hand-placed prints, which made it cover every hooked symbol in both
providers at once. Both ranks reported identically:

```
ENTER ibv_open_device    (preload,    listening=yes)
ENTER ibv_reg_mr_iova2   (preload,    listening=yes)
ENTER ibv_modify_qp      (preload,    listening=yes)
ENTER ibv_qp_to_qp_ex    (preload,    listening=yes)
ENTER ibv_poll_cq        (object-ops, listening=yes)
ENTER ibv_alloc_pd / ibv_dereg_mr / ibv_destroy_qp  (listening=no)
```

**No create door at all** — and the positive control (8 traced symbols per rank)
proves the instrument was live. A queue pair is modified, converted to an
extended QP, and driven, having been created by nothing we can see.

The reason is that it was never a symbol:

```
$ readelf --dyn-syms /lib64/libibverbs.so.1 | grep create_qp
  ibv_create_qp@@IBVERBS_1.1
  ibv_create_qp@IBVERBS_1.0
```

There is no exported `ibv_create_qp_ex`. `verbs.h:3071` makes it a `static
inline` that dispatches through `verbs_get_ctx_op(context, create_qp_ex)` →
`vctx->create_qp_ex(...)`. `librdmacm` likewise imports only
`ibv_create_qp@IBVERBS_1.1`, so `rdma_create_qp_ex` reaches the same inline.

### Why six hypotheses died and the seventh was not reachable

Every earlier instrument — `nm`, `readelf`, `LD_DEBUG=bindings` — answers
questions about **symbols**. A `static inline` has no symbol, so all three were
constitutionally incapable of naming this door, and each failure looked like
"wrong guess, try another symbol" rather than "wrong category of thing". The
guesses were not unlucky; they were drawn from a set that could not contain the
answer.

The fix was to measure a different thing entirely: not what resolves, but what
is **entered**. That distinction is the transferable lesson, and it is why the
trace was added to the engine choke point rather than to three functions.

## Step 2 — done: hooked as an object-ops slot

`DEFINE_OBJECT_OPS_HOOK(ibv, create_qp_ex, ...)`, with `get_op` returning
`verbs_get_ctx(ctx)->create_qp_ex`. Two guards reproduce what
`verbs_get_ctx_op` checks before dispatching there, and the second is
load-bearing:

- **`sz`** — `verbs_context` "grows up", so an older libibverbs may not have the
  field and writing there lands past the struct.
- **a NULL slot** means the driver did not implement extended creation, and
  `ibv_create_qp_ex` falls back to `ibv_create_qp` — which is already hooked.
  Patching a NULL slot would make `verbs_get_ctx_op` report the capability as
  present, push the application down a path it had declined, and call through a
  trampoline whose saved original is NULL. That is the `devx_probe` mistake
  exactly: **a hook must never manufacture a capability, or a failure, that the
  application did not have.**

Verified job 44234284: `ibv_create_qp_ex` entered on both ranks, `102.pcs`
(`ib_queue_pair`) written where the stream had never existed.

## What this cost, and the transferable part

Six hypotheses died to symbol-level instruments before the seventh was even
reachable, because the answer was not a symbol. Worth carrying forward:

1. **`nm`, `readelf` and `LD_DEBUG` all answer questions about symbols.** When
   the thing you are looking for might be a function pointer, an inline, or a
   vtable slot, they cannot find it and their silence looks like "wrong guess".
2. **Measure entry, not resolution.** The trace went into the engine choke point
   rather than three chosen functions, so it covered every hook in both
   providers at once — and the *absence* across all of them was the evidence.
3. **A probe needs a positive control.** The job prints a count of traced
   symbols precisely so "no create door fired" cannot be confused with "the
   trace printed nothing". The same discipline caught an earlier all-zeros grep.

## Step 2 (original text) — only after step 1 names the door

Hook it in whichever provider owns the range, emit the entity, drop the
`expect_fail` marker, and let `qp_creation_accounted` guard it.

## Constraints

- **Do not weaken the assertion.** It states a true invariant — a transition
  cannot refer to a queue pair nothing created — and the gap is real. Making it
  green by narrowing its scope would trade the only honest signal on this case
  for a colour, which is the failure this whole suite has spent a week undoing.
- **Do not add a hook on suspicion.** Three explanations have already been wrong;
  a hook added without call-level evidence would most likely capture nothing and
  read as "still broken" rather than "wrong guess".
- The entry print is a probe, not a feature. It comes out when the door is named.
