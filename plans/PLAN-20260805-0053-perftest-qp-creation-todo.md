# PLAN — find the door perftest creates its queue pair with

- **Created:** 2026-08-05 00:53 +03
- **Status:** TODO — narrowed by measurement, not closed. Four hypotheses dead,
  one contradiction standing.
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

## Step 2 — only after step 1 names the door

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
