# PLAN — entities have no close, so every capture counts calls, not lifetimes

- **Created:** 2026-08-05 00:53 +03
- **Status:** STEPS 1-3 SHIPPED. The decision was made (A) and implemented; this
  header was stale. Reviewed 2026-08-10 — what remains is below.

## Where it actually stands, 2026-08-10

**Step 1 — done.** `EntityClose` is type 522, Event, internal range, in the
schema, the generated bindings, `schema_conformance` and the reader.

**Step 2 — done and then some.** Emitted from ibverbs (six doors), mlx5dv (two),
and now gpumem's `nvshmem_free`. The table in "What is broken" below is
historical: every row of it now records a close.

**Step 3 — done, and stronger than the plan asked.**
`tests/hpc/suite/asserts/entity_close_accounted.sql` carries all three clauses:
a close naming an entity nothing opened, more closes than opens for a subject,
and an entity opened and never closed. The `ib_device` exemption on the third is
measured, not assumed.

**Step 4 — largely subsumed, and worth saying so rather than doing it.** The
plan wanted "live regions" to become computable so case 06's count stops
tracking allocator churn. It is computable — measured on
`nvshmem_coexist-44400747`: 179 opens, 179 closes, **0 live**, identically on
all four ranks. But clause 3 of the assertion already requires every opened
entity to be closed, so "live at end == 0" is implied by an assertion that
already runs everywhere, and a case-06 golden asserting 0 would add a number
without adding a check. Case 06's per-rank symmetry assertion stays as it is.

## Step 3 did its job, 2026-08-10: the CQ deficit was a third door

`entity_close_accounted` went red on every ibverbs case that runs NVSHMEM
(13-18) and on nothing else. It was right, in both of the two ways it can be.

On `hubspoke_ib-44459283`, every rank: 104 `create_cq` opens + 60 `create_cq_ex`
opens, 194 closes. A deficit of exactly 30, identical on all four ranks, split 15
closes naming a handle nothing opened and 15 naming a handle opened once and
closed twice. Those are not two bugs. A handle is an interned pointer, so a CQ
created through an unhooked door lands on a *reused* address half the time and on
a fresh one the other half; the first shape shows up as a second close, the
second as an orphan. Ordering the closes by tick shows it directly — the orphans
are a contiguous block of handles first interned at teardown, each one adjacent
to a re-close, four hooked `create_cq_ex` closes between every pair.

The door is `mlx5dv_create_cq`, and `IbCqOrigin`'s own comment had already named
the verdict before the evidence arrived: "neither means a third door". It is an
exported symbol (`mlx5dv_create_cq@@MLX5_1.1`), unlike `ibv_create_cq_ex`, and
`libuct_ib_mlx5.so.0` is the only object under UCX, OpenMPI or NVSHMEM on MN5
that names it — UCX's mlx5 transport creates its tx and rx CQs there and destroys
them through `ibv_destroy_cq`, which is why the close arrived alone.

Hooked in the **ibverbs** provider despite being a libmlx5 symbol, because
`ibv_destroy_cq` is there: an entity whose open and close come from two different
providers is only accounted when the user selects both, and `pcomm -p ib` would
have gone red on a correct capture. `mlx5dv_create_qp` took the other choice and
left exactly that hole — its `mlx5_queue_pair` open was missing from the
assertion's opens list entirely, so a perftest-shaped teardown was a guaranteed
false positive. Added, with `tests/hpc/suite/asserts_fire.sh` as the mutation
test that says the clause still fires without it.

## What is actually left

**The reader does not use the window, and address reuse is real.** Measured on
the same capture:

```
ib_mem_region   716 opens, 560 distinct handles,  63 reused
mem_object      600 opens, 522 distinct handles,  28 reused  (one opened 6x)
```

A handle is a pointer value, so a freed address gets reused and the same handle
names a *different* buffer. `mem_owner` merges objects by `(base, size)` and
`gpu_access.object` joins by handle, so both conflate incarnations that
`EntityClose` now makes separable. Resolving an access against the `[open,
close)` window it falls in is the work this plan was ultimately for, and it is
now unblocked — the records exist, the timestamps are per-rank TSC and every
join here is already rank-scoped.

**gpumem hooks `nvshmem_free` but not `cudaFree`**, and the symmetric heap
region is never freed at all. `entity_close_accounted` exempts opener 520 from
its never-closed clause for exactly that reason; the exemption is a marker for
the missing door and should be deleted when it is hooked.
- **Related:** case 06's `ib_entities` counts `reg_mr` *calls* for exactly this
  reason; `PLAN-20260803-2253-ibgda-device-initiated-capture` (the void-hook
  work that removed the mechanical obstacle).

## What is broken

pcomm records entities with a lifetime — memory regions, queue pairs, UARs,
umems — and never records the end of one. Every destroy/dereg/free path is
hooked and none emits anything:

| entity | open | close hooked? | close recorded? |
|---|---|---|---|
| `ib_mem_region` | `ibv_reg_mr_iova2` | `ibv_dereg_mr` | no |
| `ib_queue_pair` | `ibv_create_qp` | `ibv_destroy_qp` | no |
| `mlx5_umem` | `devx_umem_reg[_ex]` | `devx_umem_dereg` | no |
| `mlx5_uar` | `devx_alloc_uar` | `devx_free_uar` | no |

So a capture cannot answer "how many regions were live at time T" — only "how
many registration calls happened". Case 06's own SQL says so:

> it counts `reg_mr` *calls*, not live regions. Freed pointers get re-registered
> and both are recorded, because dereg emits no close record — so the number
> tracks NVSHMEM's allocator churn.

That is why case 06's absolute region count varied 115 vs 114 across racks at the
same commit, and why the assertion had to retreat to per-rank symmetry. The
retreat was correct; the underlying blindness is still there.

## Why it was not just an oversight

Until 2026-08-04 the hook engine could not express a void-returning hook at all
(`HookTrait` built `bool (*)(void, Args...)`; `invoke_preload` did `auto ret =
real(...)`). `mlx5dv_devx_free_uar` returns void, so it could not even be
hooked. That obstacle is gone — `HookSigs` has a void specialisation and
`invoke_preload` an `is_void_v` branch — which is what makes this plan
actionable rather than blocked.

## The decision to make first

Two shapes, and they are not equivalent:

**A. A second record per close.** `IbMemRegionClose { Handle mr; }` and friends,
or one generic `EntityClose { Handle subject; }` in the internal range.
- Fits the append-only stream model exactly: records are a packed array, written
  once, never revised.
- Costs a record type (or one shared type) and a join for every "live at T"
  query.
- Handles the awkward cases honestly: a close with no matching open (the entity
  was created before we attached) is just a row.

**B. Back-fill the entity's `Temporal` window.** The record model already has
`Temporal` with a `Window` kind carrying start and end.
- Reads better: one row per entity, lifetime on the row.
- **Requires rewriting a record already written to a stream**, which the writer
  cannot do — it is an `fwrite` of a packed array with no index. It would mean
  buffering every open entity until its close, unbounded, and reordering the
  stream. That is a different writer, not a change to this one.

**Recommendation: A**, and specifically one shared `EntityClose` in the internal
range (500-599) rather than a close type per entity. The subject is already an
interned `Handle`, which is unique across providers, so one type serves all of
them and no provider needs a new id. B is only viable if entity streams stop
being append-only, which is a much larger change than the problem justifies.

## Steps

1. Add `EntityClose { Handle subject; }` (Event, internal range) — schema,
   generated bindings, conformance, reader table, hash. `schema_check` enforces
   all five, as it did for the mlx5dv records.
2. Emit it from the four close hooks above. The handle is the same interned
   pointer the open record carries, so the join is exact.
3. **Assertions, not goldens.** The derived invariants:
   - every close names a subject some open record created (the mirror of
     `qp_creation_accounted`, and it will find the same class of missing hook);
   - closes never exceed opens for a subject;
   - a workload that tears down cleanly ends with opens == closes — which is a
     *derived* expectation from the workload, not a recorded number.
4. Only then revisit case 06's `ib_entities`: with closes, "live regions" becomes
   computable and the count stops tracking allocator churn. Do not touch that
   case before step 3 — its current per-rank-symmetry assertion is correct and
   should be *added to*, not replaced.

## Risk worth naming

A close hook that fires for an entity we never saw opened is not a bug in the
hook — it is the same blindness as case 08's unaccounted queue pair, arriving
from the other side. Step 3's first assertion will surface those, and the
temptation will be to suppress them. They should be counted and reported
instead: an unmatched close is evidence of a creation door we do not hook, which
is information we currently have no way to obtain at all.
