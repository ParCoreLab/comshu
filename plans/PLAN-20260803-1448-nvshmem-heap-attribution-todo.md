# PLAN — ask NVSHMEM who owns an address, instead of guessing from the VA layout

- **Created:** 2026-08-03 14:48 +03
- **Status:** DONE — `21f3b74`, `ebaaf3c`, `f64a3fb`. Step 1's premise held
  (evidence below); Design A was taken. Four departures from the plan as
  written, all recorded under "What actually happened"; one open decision on
  case 05 that is not the implementer's to make.
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

## What actually happened

### Step 1: the premise held, in four configurations

A standalone NVSHMEM probe (jobs **44135828**, **44136107**; NVSHMEM 3.2.5) ran
4 PEs on one node, 8 PEs over two nodes under IBRC **and** under IBGDA, and 4
PEs with `NVSHMEM_DISABLE_P2P`. In every one of them:

| question | answer |
|---|---|
| `nvshmem_ptr(a, my_pe) == a` for local symmetric `a` | **holds** — allocation bases, interiors, and a 64 MiB object's last byte |
| `nvshmem_ptr(x, my_pe)` for non-symmetric `x` | **NULL** — `cudaMalloc` pointers, host addresses, junk VAs |
| `nvshmem_ptr(q, my_pe)` for a *peer's* mapping `q` | **NULL** |
| non-`NULL` exactly for P2P-reachable PEs | **yes** — 0-3 within a node, NULL for the four PEs across the IB link, self only under `DISABLE_P2P` |
| `delta_pe = nvshmem_ptr(a,pe) - a` constant over the heap | **yes** (not relied on; see below) |
| callable off the main thread | **yes** |
| resolvable by `dlsym(RTLD_DEFAULT, ...)` | **yes**, all three symbols |

The third row is the one that mattered most and the plan did not ask for it: a
peer's mapping answering `NULL` to `my_pe` is what makes "non-`NULL` for
`my_pe`" a *sound* test for "this region is my heap" rather than a test that
claims every mapping in the process. IBGDA changed nothing.

### Departure 1: probe with the `cuMemMap` region bases, not with an observed access

The plan's Design A takes the first *observed access address* satisfying
`nvshmem_ptr(a, my_pe) == a` as the anchor. Using the `cuMemMap` region bases
instead is strictly better and needs no extra machinery: they are already
collected, they come with exact sizes, and each one is independently classified
by the same API predicate. That removes the anchor, removes any reliance on the
constant `delta`, and — the point — removes the last thing that could depend on
*which* region was seen first. `cuMemMap` is now purely a source of extents; it
contributes no ordering, no index, and no count.

### Departure 2: `mem_device` is a device, resolved by CUDA, not a PE

The plan's open question ("PE vs device: the current code casts a PE straight to
a `DeviceId`") has a clean answer. `cuPointerGetAttribute(...
DEVICE_ORDINAL, q)` on the peer address `nvshmem_ptr` returned names the GPU
that memory actually lives on — measured correct for every peer mapping in job
44136107. That is the same space `run_device` is in, so `RemoteFilter` compares
like with like. On one node it is numerically identical to the PE, so no golden
moves; at 8 PEs over 2 nodes it is the difference between a device id and a
rank id that merely looked like one.

### Departure 3: three things only running could find (`ebaaf3c`, `f64a3fb`)

**`nvshmem_ptr` is only safe inside NVSHMEM's init window.** It dereferences the
heap state, and outside `nvshmem_init` .. `nvshmem_finalize` that state does not
exist: null+0x20, all four ranks of case 04, in the recv thread. The recv
thread's *last* refresh naturally lands after the application has finalized, so
this is the normal path rather than an edge case. `nvshmem_my_pe` and
`nvshmem_n_pes` keep answering plausibly across the same boundary — the probe in
step 1 could not have caught this, because it only ever called them while
initialised — so they cannot be the liveness test. `nvshmemx_init_status` can,
and is now *required* to be resolvable rather than optional: without it there is
no safe window to check and the heap path stays off. The crash also took the
whole capture with it, since records buffer until `drain_thread`; an empty
capture is what a dead recv thread looks like.

**`cuMemMap` is not NVSHMEM's alone, and case 05 was living off the bug.** NCCL
registers its peer buffers through the same APIs, and the `cuMemAlloc`-fed
registry never sees those. Deleting the positional guess therefore left case 05
(NCCL, no NVSHMEM anywhere) with *no peer traffic at all* — because its peer
traffic had only ever existed as `find_nvshmem_heap_owner` fabricating owners
for NCCL's regions. It reported `mem_dev` up to 8 on a 4-GPU node, which is the
same `owner_in_range` violation as cases 04 and 06 wearing a different hat. The
fix is not NVSHMEM-shaped: every mapped region now gets the device
`cuPointerGetAttribute` says it lives on. That is the honest form of what case
05 was measuring, and it is what the plan's "adds entries to a registry that
already exists" was always going to have to cover.

**Ownership must be resolved at kernel launch, not on the drain path.** The plan
placed the work on the recv thread ("build the PE map on first eligible
access"), and that cannot work — for a reason that only shows up in a real
capture. The channel ring never fills at the default 1 GiB, so the *entire*
capture arrives in the single flush at context teardown, in the same buffer as
the terminate token. The one batch that needs the map is therefore the one batch
that cannot build it: NVSHMEM is already finalized, and the guard above
correctly refuses. Job 44137162: no crash, and every access filtered out, in
cases 04, 05, 06 and 07 at once.

Kernel launch has both properties the drain path lacks: NVSHMEM is initialised,
and every region the kernel can touch is already mapped — you cannot launch
against memory that is not. It is also the CUDA-callback thread, which already
holds `g_mutex`, so the region list stops being shared between threads at all.
That deleted the atomically published snapshot and the second mutex the
drain-path version needed. The drain path now contains nothing but a registry
lookup, which is what step 4 was after.

### Consequence: three goldens changed, and why that is not regeneration

Predicted from the workload *before* the run, not read off it. `put_to_next`
issues two global accesses per element — a load of the local `src[i]` and the
peer store — and `gpu_access` holds only what `RemoteFilter` kept. So a `load`
row in cases 04 and 06 meant a rank's own local memory had been attributed to
another GPU: the bug itself, 196608 phantom accesses per rank. Correct output is
stores only.

| golden | before | after |
|---|---|---|
| 04 `peer_fanout` → `peer_topology` | `peers = 2` | exact ring `0→1, 1→2, 2→3, 3→0`, 196608 each |
| 04 `traffic` | `load` + `store` rows | `store` rows only |
| 06 `traffic` | `load` + `store` rows | `store` rows only |
| 07 `cross_transport` | 196608 / 0 | **unchanged** — it already encoded the correct answer |
| 03 `peer_topology` | exact ring | **unchanged** — regression guard |

Case 07's golden needing no change is the strongest single check here: it was
captured from a run that happened to land on the right side of the race, and
the new scheme reproduces it by construction.

Every prediction above held exactly. Case 04's `peer_topology` came back
`0,1,196608 / 1,2,196608 / 2,3,196608 / 3,0,196608` — the ring, to the access.

### The board

Before (`44113361`, at `main`) and after (`44138023` + `44138108`, at `f64a3fb`):

| case | before | after | |
|---|---|---|---|
| 01 ib_pingpong | FAIL | PASS | pre-existing `qp_num` collision, flaky either way |
| 02 nccl_internode | FAIL | FAIL | pre-existing — `degree` 2 vs 1, same collision |
| 03 p2p_kernel | PASS | **PASS** | the regression guard, unchanged |
| 04 nvshmem | PASS | **PASS** | on an exact ring now, not on `peers = 2` |
| 05 nccl_p2p | PASS | FAIL | see below |
| 06 nvshmem_coexist | FAIL | FAIL | `traffic` went green; the two pre-existing IB checks (`ib_entities` 456/460, `regions_per_rank` 114/115) still fail, as they do at `5a28119` |
| 07 nvshmem_multinode | PASS | **PASS** | golden untouched |
| 08 ib_send_bw | PASS | PASS | |

### Case 05 needs a decision, and it is not mine to make

Case 05 is NCCL — no NVSHMEM anywhere — and it was passing on fabricated data:
`find_nvshmem_heap_owner` was being applied to NCCL's `cuMemMap` regions, which
is where `peer_traffic.sql`'s own note about "(3,0) vs (2,8)" comes from. On a
4-GPU node there is no device 8.

After the fix its peer traffic is real again — `peer_traffic` passes, 1,179,948
accesses where the previous run had 0 — and one check still differs:

```
ops:  golden has  {load, store} per rank;  measured  {store} per rank
```

`ops.sql` says the invariant is "every rank performs both peer loads and peer
stores". That claim was recorded from the fabricated attribution, and the `load`
rows are the same phantom-local-load artifact that cases 04 and 06 shed. Case 03
— NCCL, and on the alloc-registry path the bug never touched — has asserted
peer **stores only** all along, which corroborates it.

**Not changed here, deliberately.** For cases 04 and 06 the answer is derivable
from a three-line kernel anyone can read, so the goldens could be written before
the run. NCCL's access pattern is not: deciding that it performs no peer loads
would mean reading the answer off the run, which is the one thing this plan
rejects. It needs someone to say whether NCCL's ring allreduce does direct peer
reads at all, and the answer belongs in `ops.sql`'s comment either way.

## Still open

- **Re-landing `2bfb376`** (the window change) as the timing-perturbation probe
  for case 07 is the remaining acceptance item; it belongs to
  `PLAN-20260803-1021-gpumem-window-timestamps-todo.md`, which this unblocks.
- **The "NVSHMEM 3.7" in case 04's old comment** was never resolved — MN5 has
  3.2.5, and the comment is gone now. It no longer matters: the new scheme reads
  no version and asks for no layout, which was always the better argument.
- **Team-scoped allocations** (`nvshmemx_mc_ptr`, `nvshmem_team_*`) remain out of
  scope, as planned. Multicast addresses are not `nvshmem_ptr`-resolvable and
  would fall through to the alloc registry, i.e. be treated as local and
  filtered out.

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
