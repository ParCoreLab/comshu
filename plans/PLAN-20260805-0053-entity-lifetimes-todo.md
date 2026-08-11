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

## 2026-08-11: the QP close is now engine-owned — mlx5dv-only runs account their queue pairs

`entity_close_accounted` fired "entity opened and never closed (opener 210)" on
the IBGDA cases (09/10/12, `-p mlx5dv`) and nowhere else. A `Mlx5QueuePair` is
opened by the mlx5dv provider but destroyed through `ibv_destroy_qp`, whose close
lived in the **ibverbs** provider — not loaded on an mlx5dv-only run, so nothing
recorded the close. The mirror of the CQ deficit below, from the destroy side.

The close door had to move to a component present in every provider selection,
and since ibverbs and mlx5dv coexist by design, no provider can own
`ibv_destroy_qp` alongside the other. The **engine** owns it now (like `dlsym`,
the object registry, and the `MemObject` close from `nvshmem_free`). Four defects
surfaced on the way, each found by measurement, not reasoning — worth keeping so
they are not re-derived:

1. **A `-p p2p` run has no QP opens**, so an unconditional engine close named
   entities nothing created (cases 04/05/06 went red). Gated on a QP-open
   provider being present: `type_is_declared(102) || type_is_declared(210)`. This
   is `pcomm_close_object`'s registry check in another form.
2. **NVSHMEM/NCCL reach `ibv_destroy_qp` entirely through `dlsym`/`dlvsym`**
   (measured on du04: `[pcomm-entry] dlsym/dlvsym ibv_destroy_qp`), so the ELF
   interposer never saw it. Registered in `smart_dlsym` like
   `dmabuf_registry`'s `cuGetProcAddress`.
3. **Forwarding to the real symbol is version-sensitive.** `RTLD_NEXT` lands on a
   compat implementation and faults (a SIGSEGV inside `ibv_destroy_qp` during
   `nvshmemid_hostlib_finalize`); `RTLD_DEFAULT` finds the engine's own exported
   symbol and recurses. The dlsym path forwards to the chain's `next` (the exact
   versioned symbol the caller resolved); the PLT path (perftest, pingpong)
   resolves from libibverbs' own handle via `dlopen(NOLOAD)` — what
   `PreloadHookManager` already does.

Validated on both clusters: MN5 01–08 green (job 44506331), du04 09–12 green,
`entity_close_accounted` passing on all. Commits `b63be06`, `a894345`, `170624f`,
`58792c6`. The opener recorded on the close is `102` regardless of the creation
door (the closing hook cannot know 102 vs 210), which is harmless — the assertion
joins on subject alone.

**Was open, one latent edge:** ibverbs and mlx5dv do not co-run in any case
today; when they do, the gate lets the engine emit one close per destroy, which
is correct, but a QP created via `ibv_create_qp` on an mlx5dv-only run (none
observed) would be closed without an open. The gate was coarse (per-run, not
per-QP); a true fix registers each opened QP handle, the same registry
`PLAN-20260806-1329` builds for objects. Done, 2026-08-11 — below.

## 2026-08-11: the gate is per-QP now, and a close names the door that opened it

`pcomm_engine_note_qp(qp, opener_type_id)` is a new engine-ABI call. Each
creation door calls it right after recording the open — ibverbs from
`create_qp` and `create_qp_ex` with 102, mlx5dv from `mlx5dv_create_qp` with 210
— and the engine keeps the handles in a `QpRegistry`, the per-QP counterpart of
`ObjectRegistry`. `pcomm_close_qp` then has two paths instead of one rule:

- **handle registered** → close it, reporting the opener the creating provider
  registered. So an mlx5dv-created queue pair closes as 210, and the "opener is
  102 regardless of the creation door" wart above is gone. The assertion still
  joins on subject alone, so old captures (every close 102) still check out;
  `asserts_fire.sh` carries both shapes as quiet cases.
- **handle not registered** → the coarse per-run rule, unchanged, for this path
  only. Kept deliberately: an `ibv_create_qp` queue pair on a `-p mlx5dv` run and
  an `mlx5dv_create_qp` one this provider missed are the same `void*` with the
  same absent open, so suppressing the row would suppress the missing-door signal
  the assertion exists for. What the registry removes is every case where the
  engine *can* know; what is left is a case where nothing at the destroy site can.

`QpRegistry` **erases** on close where `ObjectRegistry` deliberately does not.
Nothing resolves an address to a queue pair, so there is no window to keep, and a
destroyed `ibv_qp*` address goes straight back to the next `ibv_create_qp` — a
retained entry would let the next queue pair at that address, created through an
unhooked door, close as though its open had been recorded. Erasing also drops a
double destroy of one pointer onto the unregistered path, where the assertion can
still see it.

**No ABI bump**, deliberately. `PCOMM_ENGINE_ABI_VERSION` is compared for
equality at declare, so bumping would reject providers this addition does not
break: a provider built before the call simply never makes it and the engine
falls back to the coarse rule — the previous behaviour exactly. The reverse
direction cannot be silent either, since providers carry a DT_NEEDED on the
engine and link `-Wl,--no-undefined` (`undefined symbol: pcomm_engine_note_qp`).

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
