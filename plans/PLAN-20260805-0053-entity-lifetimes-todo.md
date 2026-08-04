# PLAN — entities have no close, so every capture counts calls, not lifetimes

- **Created:** 2026-08-05 00:53 +03
- **Status:** TODO — the engine no longer blocks it; the design decision is open.
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
