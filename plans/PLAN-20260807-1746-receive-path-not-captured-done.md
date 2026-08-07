# PLAN — the receive path, which pcomm infers and never observes

- **Created:** 2026-08-07 17:46 +03
- **Status:** DONE 2026-08-07. All five stages implemented and measured on MN5;
  final board is job 44400747, SUITE GREEN — 8 PASS (3 carrying one documented
  xfail) + 3 SKIP. Six record types landed. See "What was measured" at the
  bottom: **three claims in this plan turned out to be wrong** and are corrected
  there rather than quietly worked around.
- **Spawned:** `PLAN-20260807-2119-ucx-context-open-door` — the missing CQ
  creation door this work exposed. It is the reason for the three xfails and is
  deliberately NOT part of this plan's scope.
- **Scope was:** "everything SigmaTrace's ibverbs module reaches, on the receive
  side, and the entities that make those rows joinable".
- **Prior art:** `../SigmaTrace` — `Ibtrace/IbverbsHooks.hpp` (the hook set),
  `Ibtrace/IbverbsEventSource.hpp:64-102` (slot mapping + `get_object_from_args`),
  `Profiler/Handlers/IBV/Ibverbs.cpp:179-209` (the only receive-side handler that
  actually records), `Profiler/Events/IbvEvents.hpp:236-247`.
- **Related:** `PLAN-20260807-0130-context-coverage-and-next` — `ibv_create_cq`
  is now hooked for coverage and carries no callback, which this plan spends.

## What is broken

pcomm hooks `ibv_post_send` and `ibv_poll_cq`. There is no receive hook of any
kind, so **every receive is inferred from its completion and never observed at
the point it was posted.**

Case 01 shows the asymmetry in the golden that is already committed:

```
rank,status,opcode,n
0,success,recv,20        <- 20 receives completed
0,success,send,20        <- 20 sends, each with an ib_post_send row
```

Twenty receive completions per rank, and nothing anywhere says which buffer each
one landed in. `ib_post_send` carries `wr_id`, `length` and the RDMA remote
address; the receive side has no counterpart, so:

- a receive buffer is never linked to the `ib_mem_region` that owns it;
- `wr_id` on a recv completion joins to nothing, while the same field on a send
  completion joins to `ib_post_send`;
- a lost or never-posted receive is indistinguishable from one that completed,
  because the only evidence of either is the completion that did or did not
  arrive.

Two-sided transports are exactly where this matters. NCCL's and NVSHMEM's proxy
paths post receives ahead of time; a capture that cannot see them cannot say
whether a stall was "no buffer posted" or "nothing sent".

## Second defect, same family: entities nothing creates

`IbPollCq.cq` is an interned `ibv_cq*` and **there is no `ib_completion_queue`
entity**. So the oldest event table in the capture references an entity no record
creates — the precise shape `entity_close_accounted.sql` exists to catch, sitting
in plain sight because that assertion checks closes against opens and nothing
checks an event's subject against an open.

The same is true of any SRQ and any AH the moment their hooks land. So the
entities come *with* the events here, not after them.

## How SigmaTrace does it, and where it stops

`IbverbsHooks.hpp` defines eleven doors this provider lacks. Read the handlers
before assuming they all produce data — **five of them record nothing**:

| door | SigmaTrace hook | SigmaTrace records | pcomm today |
|---|---|---|---|
| `ibv_post_recv` | object-ops | `IbvPostRecv` + `IbvPostRecvWr` | miss |
| `ibv_post_srq_recv` | object-ops | nothing | miss |
| `ibv_req_notify_cq` | object-ops | nothing (`IbvReqNotifyCq` is an enum with no struct) | miss |
| `ibv_create_ah` | preload | nothing | miss |
| `ibv_query_device` | preload | nothing | miss |
| `ibv_get_device_guid` | preload | nothing | miss |
| `ibv_create_cq` | preload | nothing | **hooked, no callback** |

So "parity with SigmaTrace" is a lower bar than it looks: on the receive side it
means `ibv_post_recv`, and the rest are doors held open for a record nobody
wrote. This plan takes the doors *and* writes the records, because a hook that
records nothing costs a dlsym chain and buys an empty table — which is the
failure mode this project has been paying down all week.

Note also that SigmaTrace's `post_recv` handler stores `num_sge` and no sge list,
so it never resolves the receive buffer either. Matching it exactly would leave
the central question — *which memory receives this* — unanswered. Don't.

## Stages

### 1. `ib_completion_queue`, and `ibv_destroy_cq`

`ibv_create_cq` is already hooked and already calls `ensure_installed`; add the
callback and the entity, then hook `ibv_destroy_cq` so the close lands in
`entity_close` like every other entity's does. `IbPollCq.cq` becomes joinable and
the suite-wide close assertion starts covering CQs for free.

`ibv_create_cq_ex` is a `verbs_context` slot, not a symbol — the `create_qp_ex`
shape exactly. Do it as an object-ops hook through `vctx_slot` **or** leave it
and measure how many CQs arrive uncounted; do not assume the exported symbol is
the only door, which is the mistake that cost case 08 a week.

### 2. `ibv_post_recv` → `IbPostRecv`

Object-ops hook on `ctx->ops.post_recv`, reached from `qp->context`.

One row per work request, like `IbPostSend`, not one per call: `qp`, `qp_num`,
`wr_id`, `num_sge`, total `length` summed over the sg_list, and the **first
sge's `addr` and `lkey`**. Those last two are what SigmaTrace does not keep and
are the whole point — they resolve the receive buffer to an `ib_mem_region`,
which makes "where did this message land" answerable for the first time.

Reuse `wr_builder.hpp` if the walk generalises; a receive WR chain is the same
linked-list shape with a smaller struct.

**Measure the volume before shipping the row.** `ib_post_send` is per-WR and
NCCL produces 26880 of them on case 02; receives are posted in batches ahead of
need and could be larger. If the count is uncomfortable, the predicate mechanism
(`DEFINE_OBJECT_OPS_HOOK_PRED`, as `poll_cq` uses to drop empty polls) is the
existing answer — but pick it from a number, not from a worry.

### 3. SRQ: `ibv_post_srq_recv`, `ibv_create_srq`, `ibv_destroy_srq`

`post_srq_recv` is an object-ops hook whose first argument is an `ibv_srq*`, so
`get_object_from_args` needs the SRQ branch SigmaTrace already worked out
(`IbverbsEventSource.hpp:97`).

An SRQ receive belongs to a queue *pair*-less object, so `IbPostRecv` needs a
subject that is either a QP or an SRQ. Use one record with an `origin`-style
discriminator (`IbRegionOrigin` is the precedent) rather than a second record
type: a reader asking "what receives were posted" should not have to know which
API posted them, exactly as it does not have to know whether a region came
through `reg_mr` or `reg_mr_iova2`.

`ibv_create_srq`/`ibv_destroy_srq` come with it, and not optionally: without them
every SRQ row names an entity nothing created, which is the defect this plan
opens by pointing at.

### 4. `ibv_req_notify_cq` → `IbReqNotifyCq`

Object-ops on `ctx->ops.req_notify_cq`, one row: `cq`, `solicited_only`. This is
the only evidence in the capture that an application is using *interrupt-driven*
completion rather than polling — which changes how a latency gap should be read,
and today looks identical to a quiet CQ.

Cheap, low volume (armed once per completion batch, not per completion).

### 5. `ibv_create_ah` → `ib_address_handle`, and device identity

`create_ah` is the UD path's addressing object; `ah_attr` carries `dlid`,
`sl`, `port_num` and the GRH. Recording it is what would let a UD workload
produce `comm_edges` at all — today `comm_edges` derives peers from RC's
`modify_qp` transitions, so a UD run is topologically silent.

`query_device` and `get_device_guid` are the device's stable identity. Worth one
`ib_device` entity emitted once per context: `node_guid`, `fw_ver`,
`vendor_id`/`part_id`, `phys_port_cnt`. This is the fact that would let two ranks
on different nodes be joined by *device* rather than by LID — and `src_lid`
already exists precisely because `qp_num` alone collides across HCAs.

## Verification

Cases 01 and 08 are the natural gates — `ib_rc_pingpong` and `perftest` both post
receives, and case 01's completions golden already pins 20 recv completions per
rank, so the expected `ib_post_recv` count is known *before* the code is written.
That is the good kind of golden: predicted, then measured.

The real prize is an assertion, not a golden:

> **every recv completion joins to a posted receive on the same rank and QP by
> `wr_id`, and no posted receive completes twice.**

That is the receive-side mirror of `entity_close_accounted`, it is
allocation-independent, and it fails loudly if a receive path is ever captured on
one side only. Put it in `tests/hpc/suite/asserts/`, not in a case.

Watch case 02 and 07 for volume regressions when the recv rows land — those are
the two boards with real proxy-thread traffic.

## Constraints

- **A hook that records nothing is not parity.** Five of SigmaTrace's receive
  doors are open and silent. Copying that shape produces empty tables, and this
  project has now spent a week learning that an absent provider and a broken one
  look identical downstream.
- **`ctx->ops` slots are per-context**, so every one of these object-ops hooks
  inherits the first-sight install machinery and its window. `create_cq` is
  already an install door; `create_srq` should become one too, since an SRQ can
  exist before any PD on the same reasoning that made `create_cq` necessary.
- **Do not add a record type per API entry point.** Origin discriminators, as
  `IbMemRegion.origin` does, keep a capture from leaking which entry point the
  caller happened to compile against.
- Record ids: 100-199 is the ibverbs range, `100 102 106 110 112` taken.


---

# What was measured (2026-08-07, MN5)

Six record types landed: `ib_completion_queue` (104), `ib_shared_recv_queue`
(108), `ib_post_recv` (114), `ib_req_notify_cq` (116), `ib_address_handle`
(118), `ib_device` (120).

## The headline claim held

Case 01, per rank: **500 posted receives, every one joining to the
`ib_mem_region` that owns its buffer** (base `0x556943d4e000`, 4096 B). "Which
memory receives this" is answerable for the first time, and it is answerable
*because* the first sge's `addr`/`lkey` are kept — the field SigmaTrace's
handler does not have. Confirmed against `../SigmaTrace`
(`Profiler/Handlers/IBV/Ibverbs.cpp:177-209`): it stores `num_sge` and no sge
list, and it does not check the return value either, so it records receives that
were never posted.

Volumes: 500/rank (case 01), 24,320 (case 02 NCCL), 147,088 (case 07 NVSHMEM —
15,360 QP + 131,728 SRQ). The plan asked for a volume measurement before
shipping the per-WR row. It is comfortably below `ib_post_send`'s firehose on
the same cases, so **no predicate is needed** and none was added.

## Three things this plan got wrong

**1. "case 01's golden pins 20 recv completions, so the expected `ib_post_recv`
count is known" — the count is 500, not 20.** `ibv_rc_pingpong` pre-posts
`rx_depth` (default 500) and only reposts when its outstanding count falls to 1,
which `-n 20` never triggers. The oracle is `rx_depth`, now a `params` column;
`asserts/receives.sql` derives from it. The prediction was still made before the
run and confirmed by it — just from the right parameter.

**2. "`create_srq` should become an install door, since an SRQ can exist before
any PD".** `ibv_create_srq` takes a **pd** and therefore cannot precede one. It
is `ibv_create_srq_ex` that takes a *context* — and like `create_cq_ex` and
`create_qp_ex` it is a `verbs_context` slot with no exported symbol. Both are
hooked.

**3. "every recv completion joins to a posted receive on the same rank and QP by
`wr_id`" — not for SRQ receives, and the first version of the assertion was
wrong because of it.** An SRQ receive is not bound to a QP when posted (the
hardware picks one when a message lands), so its post carries `qp_num = 0` while
its completion carries a real one. Scoping posts to `origin='qp'` without
scoping completions to match reported **656 false violations on case 07**. The
assertion now joins SRQ receives on `(rank, wr_id)`. Case 02 had passed only
because NCCL creates 120 SRQs and posts to none of them — a capture with no SRQ
traffic cannot tell you whether your SRQ handling is right.

## Stage 5 was implemented against the plan's intent, not its letter

The plan said hook `query_device` and `get_device_guid`. Hooking `query_device`
*for the record* would make `ib_device` appear only when the application happens
to ask — most do not. It is emitted from `ensure_installed` instead, the one
place that runs exactly once per context: 73 contexts per rank on case 02, every
one with a row. `get_device_guid` is not hooked at all — it takes an
`ibv_device*` so it is useless as an install door, and its guid is already
`node_guid` in the record. That is this plan's own "a hook that records nothing
is not parity" test, applied to the plan.

## Nothing shipped as an empty table

The plan's central worry was five doors that record nothing. On case 02 (NCCL):
162 address handles, 600 `req_notify_cq` rows, 120 SRQs. On case 07: 131,728 SRQ
receives. `ib_device` carries real identity (`fw_ver 28.39.2048`, distinct
`node_guid` per rank). `create_cq_ex` fires 480 times per run (60/rank), so
hooking it rather than merely measuring it was the right call — a plain
`ibv_create_cq` hook would have missed those silently.

## What this opened, and handed on

Stage 1 made completion queues visible, and visibility immediately found
something: **30 CQs per rank are destroyed that nothing recorded creating**,
identically on all 8 ranks of case 02 and case 07. That is the assertion working
as designed — it is the same defect class `entity_close_accounted` was written
to catch, and the same shape as the `create_qp_ex` hunt.

It is diagnosed, not fixed, and it is **not this plan's scope**: the fix is in
the context-opening path, not the receive path. Full evidence, the two
instruments built for it, and the next steps are in
`PLAN-20260807-2119-ucx-context-open-door`. `entity_close_accounted` is
`expect_fail` on cases 02/06/07 with the measurement attached, exactly as
`qp_creation_accounted` was on case 08 until its door was found — never
weakened, rows still printed.

`ib_device` contexts opened and never closed (73 opened, 67 closed, on every
rank of both cases) was the other new signal, and it **is** closed here: the
determinism ruled out the flush-timing reading, a device context's natural
lifetime is the process, and real MPI stacks exit without closing them all.
`entity_close_accounted`'s "never closed" clause now exempts opener 120 only,
with the measurement in the comment. The other two clauses still cover
`ib_device` in full — verified by mutation.

## Known remaining gap, unrelated to this plan

An application polling through the extended `ibv_cq_ex` API never touches
`ctx->ops.poll_cq`, the mirror of the `ibv_wr_*` blind spot on the send side.
`ib_poll_cq` would be empty and `recv_completion_accounted` would go *quiet*
rather than red. pingpong and perftest both use the classic path, so it is not
biting today. Carried in `PLAN-20260807-2119-ucx-context-open-door`, which is
the same family of hole.
