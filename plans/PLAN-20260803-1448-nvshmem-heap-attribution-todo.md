# PLAN — ask NVSHMEM who owns an address, instead of guessing from the VA layout

- **Created:** 2026-08-03 14:48 +03
- **Status:** TODO
- **Blocks:** `PLAN-20260803-1021-gpumem-window-timestamps-todo.md` (the window
  change is parked because it perturbs the race this plan removes).
- **Findings:** `things-to-circle-back-to.md` — "NVSHMEM heap attribution
  depends on when a record is drained" and "goldens that encode run-specific
  identifiers".

## What is broken

`mem_device` — *which GPU the memory an access touched lives on* — is wrong for
NVSHMEM symmetric-heap addresses. It is not a small error: measured
`mem_dev = 64,65,66,67` where the answer is `1,2,3`.

It matters beyond one column, because `RemoteFilter` is
`run_device != mem_device`. A wrong owner turns a *local* access into "remote
traffic", so the corruption inflates the capture rather than just mislabelling
it: case 07 doubled (196608 → 393216 accesses per rank) and the two
node-crossing ranks went from 0 NVLink accesses to 196608.

## This is already known, and already worked around

Not a new discovery — the tree has been compensating for it. `case 04`'s
`peer_fanout.sql`:

> NB: NVSHMEM 3.7's fragmented heap breaks positional device attribution, so the
> mem_device *ids* are unreliable and run-varying; the fan-out count is the
> stable invariant.

and in the same file, *"the ring put has each PE store to one neighbor"* — while
its golden says:

```
run_dev,peers
0,2
1,2
2,2
3,2
```

A one-neighbour ring reporting a fan-out of **2**. That is the corruption
showing through a test that **passes**. Contrast case 03, which uses the plain
alloc-registry path and asserts the exact ring:

```
run_dev,mem_dev,n
0,1,16384
1,2,16384
2,3,16384
3,0,16384
```

That difference — exact peers where we control attribution, "count them, don't
name them" where NVSHMEM does — is the whole problem in two goldens.

## Why the current approach cannot be repaired in place

Both paths reverse-engineer NVSHMEM's *private* virtual-address layout.

**3.x** (`find_nvshmem_heap_owner`):

```cpp
return (mype + r.region_index) % npes;   // npes = snap->size()
```

with `region_index` assigned by `publish_heap_snapshot()`, which sorts the
regions discovered *so far* and re-indexes them 0..n-1. Three separate defects:

1. **`npes` is inferred from the region count.** That is only valid if NVSHMEM
   maps exactly one region per PE. It maps ~76 for 8 PEs, so the modulus is
   wrong and every index derived from it is wrong.
2. **The index is positional**, so it changes as regions are discovered — the
   answer depends on how many existed when a record happened to be drained.
   This is the race; it is a *consequence* of (1), not the root.
3. **Ownership is assumed contiguous and rotational** (`region 0 is me, region i
   is PE (mype+i)%npes`). Nothing obliges NVSHMEM to lay the heap out that way,
   and with a fragmented heap it does not.

**2.x** (`find_nvshmem_dev_of_ptr_legacy`) is worse by construction: hardcoded
`start = 0x000012020000000`, a constant stride, `size = 15`, and literal version
strings `"2.8"` / `"2.9"` selecting between behaviours.

No amount of care fixes a scheme whose input is a layout the vendor does not
document and is free to change. The user's instinct — *"I am almost sure this is
not working on all versions of NVSHMEM"* — is exactly right, and it is not
fixable by tuning the guess.

## The fix: NVSHMEM will tell us

`nvshmem_ptr(ptr, pe)` returns the **local address at which PE `pe`'s copy of a
symmetric object is mapped**, or `NULL` when that PE is not directly accessible.
That is precisely the map we are reconstructing, from the public API.

Verified on the MN5 install (NVSHMEM **3.2.5**,
`comm_libs/12.9/nvshmem/lib/libnvshmem_host.so.3.2.5`) — all exported, version
tagged:

```
T nvshmem_ptr@@NVSHMEM      void *nvshmem_ptr(const void *ptr, int pe)   [host + device]
T nvshmem_my_pe@@NVSHMEM
T nvshmem_n_pes@@NVSHMEM
T nvshmem_malloc@@NVSHMEM   nvshmem_calloc / nvshmem_align / nvshmem_free also present
```

Three things fall out:

- **`npes` becomes real** — `nvshmem_n_pes()` instead of counting regions. That
  alone kills defect (1), and (2) with it.
- **Nothing is positional**, so drain timing stops mattering.
- **The cross-node zeros become principled.** `nvshmem_ptr` returns `NULL` for a
  PE that is not P2P-reachable, which is *exactly* why ranks 3 and 7 legitimately
  record 0 NVLink accesses. Today that correct answer is a by-product of a
  layout guess; it would become NVSHMEM's own answer.

### Where it plugs in

`AllocRegistry` (`providers/gpumem/engine.hpp`) already is the right structure —
`(base, size) → device`, range lookup on the drain path:

```cpp
const Alloc* find(std::uint64_t addr) const;   // linear scan, allocation counts are small
```

So the fix **adds entries to a registry that already exists** and deletes both
guessing paths. `Engine::process` needs no change: addresses that resolve get a
device, the rest fall back to `run_device` and are filtered out, as now.

### Getting a symmetric address to probe with — two designs

`nvshmem_ptr` needs one valid *local symmetric* address; from it the whole map
follows, because the heap is symmetric and the offset is identical on every PE.

**Design A — probe with an address we already observe (preferred).** The recv
thread already has access addresses. A local symmetric address is
self-identifying:

```c
nvshmem_ptr(a, nvshmem_my_pe()) == a     /* iff a is local symmetric memory */
```

Take the first observed access that satisfies this, then for `pe` in
`[0, nvshmem_n_pes())` call `nvshmem_ptr(a, pe)`; every non-`NULL` result names
the base of that PE's mapping. Combine with the region sizes we already collect
from `cuMemMap` to get exact ranges. Build once, cache, done.

*Why preferred:* no interposition at all, so no possibility of perturbing the
application's allocation path — and this codebase has already been bitten there
(hooking both `ibv_reg_mr` and `ibv_reg_mr_iova2` double-counted every region).

*Risk:* rests on `nvshmem_ptr(a, my_pe) == a` holding for any local symmetric
address. **Validate this first** (step 1) — the whole design depends on it.

**Design B — interpose `nvshmem_malloc` (fallback).** Hook it with the existing
engine (`DEFINE_PRELOAD_HOOK`; `gpumem_trace` already links `pcomm::engine`,
which is the same object as `pcomm::hook`). Each allocation yields a known-good
symmetric address plus its exact size, so per-allocation ranges are exact rather
than inferred from `cuMemMap`.

*Cost:* an interposer in the application's allocation path, and `nvshmem_calloc`
/ `nvshmem_align` / `nvshmem_realloc` must be hooked too or allocations are
missed silently.

Both designs resolve symbols with `dlsym` at **runtime**, not link time. The
provider gains no build dependency on NVSHMEM; when the symbols are absent the
application is not using NVSHMEM and the path is skipped. Lazy resolution is
required either way — NVSHMEM is not loaded when the provider's constructor
runs — and `pcomm::smart_dlsym` already does exactly this for the ibverbs
provider.

## Steps

1. **Validate the premise before building on it.** A ~30-line standalone
   NVSHMEM program on MN5: allocate, print `nvshmem_ptr(p, pe)` for every `pe`,
   and check (a) `nvshmem_ptr(p, my_pe) == p`, (b) non-`NULL` exactly for
   P2P-reachable PEs, (c) that the returned bases fall inside the `cuMemMap`
   regions we observe. If (a) fails, Design A is out and B is the plan. **Do not
   skip this** — the previous scheme failed precisely by assuming a layout
   property nobody checked.
2. Resolve `nvshmem_ptr` / `nvshmem_my_pe` / `nvshmem_n_pes` lazily via
   `smart_dlsym`, behind a "NVSHMEM present?" predicate. No symbols → skip, and
   say so once at startup rather than silently.
3. Build the PE map on first eligible access (Design A) and cache it. It is
   immutable once built: the symmetric heap's PE assignment does not change,
   which is the property the positional scheme lacked.
4. Feed `AllocRegistry` with `(peer_base, size, pe)` entries so attribution is
   one range lookup and `find_nvshmem_heap_owner` disappears from the drain path.
5. **Delete both guessing paths** — `find_nvshmem_heap_owner`,
   `find_nvshmem_dev_of_ptr_legacy`, `NvshmemHeapRegion`,
   `publish_heap_snapshot`, `heap_snapshot`, and the `nvshmem_version` /
   `nvshmem_ngpus` knobs. This should be a net deletion.
6. **Strengthen the goldens the bug forced to be weak.** Case 04's
   `peer_fanout` becomes an exact peer assertion in the shape of case 03's
   `peer_topology`, and its comment about unreliable ids goes with it.
7. Keep a guard the old scheme could not have: assert every attributed owner is
   in `[0, nvshmem_n_pes())`. `mem_dev = 64` with 8 PEs would have failed
   immediately and loudly.

## Acceptance criteria

Falsifiable, and mostly already written down:

| check | now | after |
|---|---|---|
| case 04 `peer_fanout` | `peers = 2` (a 1-neighbour ring) | `peers = 1`, or exact peer ids |
| case 04 comment | "mem_device ids are unreliable and run-varying" | deleted |
| case 03 `peer_topology` | exact ring, passing | **unchanged** — regression guard |
| case 07 `cross_transport` | timing-dependent | stable across a deliberate timing change |
| owner range | unchecked | asserted `< n_pes` |

The decisive test for case 07: **re-land `2bfb376` (the window change) and
confirm it stays green.** That change is a known, reproducible timing
perturbation — it is the best available probe for whether attribution still
depends on drain timing, and it is the thing this plan unblocks.

## Open questions

- Case 04's comment says **NVSHMEM 3.7**; MN5 has **3.2.5**. Either the comment
  is wrong or it was written against another environment. Worth resolving,
  because "which version fragments the heap" is load-bearing for the *old*
  scheme and irrelevant to the new one — which is itself an argument for the new
  one.
- IBGDA / proxy-mapped regions: `nvshmem_ptr` behaviour there is unverified.
  Step 1 should cover a config with `NVSHMEM_IB_ENABLE_IBGDA` set, as case 07
  does.
- Team-scoped allocations (`nvshmemx_mc_ptr`, `nvshmem_team_*`) are out of scope;
  no case uses them today. Note it rather than build for it.
- PE vs device: the current code casts a PE straight to `DeviceId`, so PE and
  device are already conflated. That is fine single-node but is a latent
  wrongness worth deciding on deliberately rather than inheriting.

## Rejected

- **Regenerate case 07's golden.** It encodes one arbitrary interleaving; a new
  one would encode another and still be wrong.
- **Keep the positional scheme, fix only `npes`** (use `nvshmem_n_pes()` but
  keep `(mype + region_index)`). Fixes the modulus and leaves the rotational
  layout assumption and the ordering dependence. Half a fix that would look like
  a whole one.
- **Resolve ownership device-side** in the injected function — `nvshmem_ptr` is
  `__host__ __device__`, so it is possible. Rejected for the same reason as
  reading `%globaltimer` per access: it puts work on the hot path of the thing
  being measured, and case 07 has already shown how little perturbation that
  path tolerates.
- **Parse NVSHMEM's internal state** (heap metadata structures). Trades one
  version-fragile guess for a more fragile one.
