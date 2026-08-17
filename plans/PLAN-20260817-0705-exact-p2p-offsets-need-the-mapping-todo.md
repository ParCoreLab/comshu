# Exact p2p object offsets: record the mapping, not the library

Status: **todo**
Created: 2026-08-17 07:05
Touches: `gpu-backends/nvbit/nvbit_backend.cu`,
`libpcomm/include/pcomm/core/records.hpp`, `providers/gpumem/engine.hpp`,
`pcomm-read/link_sql.hpp`

## What is already fixed, and what this is not

Two defects in `object_access`'s p2p branch were fixed on 2026-08-17 and are
**not** what this plan is about:

- `owner_rank` was the rank that *wrote*, not the rank that *owns* — the opposite
  of what the verbs branch means by the same column.
- the offset window partitioned on `(rank, object)`, gluing one actor's mappings
  of the same symmetric object id on several peers into one window (measured: a
  2048-byte object reporting offsets to 549,755,815,936).

What remains is the third thing, which no SQL change can reach: a p2p
`obj_offset` is anchored to the **lowest address observed**, so it is only the
true offset when the workload happened to touch byte 0. `exact = FALSE` marks it
and the Objects panel renders "(approx.)" — this is honest today, just not right.

## Why not the NVSHMEM route

The tempting shortcut is the symmetric heap: `nvshmem_ptr(a, pe) = peer_base[pe]
+ (a - my_heap_base)`, so one delta per (actor, peer) fixes every object. It
works, and it is the wrong layer.

`gpu-backends/nvbit/nvbit_backend.cu:184-194` is the scar tissue from the last
time ownership was derived from NVSHMEM's private VA layout: it produced
`mem_dev = 64,65,66,67` where the truth was 1,2,3, and `RemoteFilter` turned that
into phantom traffic — case 07 doubled, 196,608 → 393,216 accesses per rank. The
fix was to stop inferring and ask CUDA. `:288` states the general principle
outright: *"cuMemMap is not NVSHMEM's alone — NCCL registers its peer buffers
through the same APIs."*

So NVSHMEM stays where it already is: a **supplement** that knows a region is
symmetric memory at all (`refresh_mapped_regions` step 2), not the mechanism.

## The general fact

The delta is a property of the **mapping**, not of the library that made it, and
mappings enter a process through a small enumerable set of CUDA doors:

| door | what it gives | delta |
|---|---|---|
| VMM: `cuMemCreate` → `cuMemExportToShareableHandle` → `cuMemImportFromShareableHandle` → `cuMemMap(ptr, size, **offset**, handle, flags)` | local VA, size, **offset into the physical allocation**, allocation handle | exact, no subtraction needed — `offset` *is* the answer |
| legacy IPC: `cuIpcGetMemHandle(h, dptr)` on the owner, `cuIpcOpenMemHandle(&ptr, h)` on the importer | owner base and importer base against a 64-byte handle | `ptr - dptr`, exact |
| same-process peer access (UVA + `cudaDeviceEnablePeerAccess`) | nothing to record | zero — the pointer already *is* the owner's |

The join key is the handle, which is process-independent by construction: that is
its entire purpose. Any library that shares device memory across processes goes
through one of these — NVSHMEM, NCCL, UCX, MPI, or hand-written code — so
recording them covers all of it and knows about none of it.

**We already hook the hard one.** `nvbit_backend.cu:985` intercepts
`API_CUDA_cuMemMap` and keeps `mapped_regions` as `(base, size)`. The two fields
that make the offset exact — `offset` and `handle` from the same
`cuMemMap_params` — are read and discarded. That is the smallest possible
starting point.

## Design

**1. A `PeerMapping` entity record.** Local VA base, size, offset into the
allocation, an interned handle identity, and the owning device. Emitted from the
backend at the door, next to `mapped_regions`.

**2. Interning the handle.** A shareable handle is an fd (process-local — the
*number* is meaningless across ranks) or a fabric handle (globally meaningful).
For the fd case the join key must be the **content** the exporter published, not
the descriptor: intern the 64-byte `CUipcMemHandle` / the fabric handle bytes in
a new `InternDomain`, exactly as `MemObject` interns base addresses so two
providers mint the same id (`records.hpp:94-99`). This is the part most likely to
be got subtly wrong and deserves its own test.

**3. The owner side must record too.** A mapping is only resolvable if the
exporting rank recorded `cuMemCreate` / `cuIpcGetMemHandle` against the same
handle identity. Both halves or neither — a capture with importers and no
exporters resolves nothing, and that must surface as a `link_health` count rather
than as silently-approximate offsets.

**4. `object_access` becomes exact.** The p2p branch replaces the `min()` window
with a join to the mapping:

```
obj_offset = g.address - pm.local_base + pm.alloc_offset
           - (owner_object.range_base - owner_alloc.base)
```

and `exact` becomes TRUE for rows that resolve. Rows that do not resolve keep the
current `min()` behaviour and stay `exact = FALSE` — degrading per row, the way
the corpus degrades per stream, not all at once.

## The oracle

This plan has something most do not: **the right answer is already measured.**
`hubspoke_ib-44578395` takes the verbs branch, which is exact today, and reports
the hub's 64 KiB buffer as three disjoint bands:

```
owner  object  actor  lo      hi      bytes
0      707     1      16384   32768   49152
0      707     2      32768   49152   49152
0      707     3      49152   65536   49152
```

`hubspoke-44465477` is the same workload on NVLink and currently reports all
three actors at `[0, 16384)`. **Success is the p2p capture reproducing
16384/32768/49152.** No new fixture, no golden to bless, and the two transports
cross-check each other — which is exactly the pairing the hub/pipe/skew cases
were built for.

## Steps

1. Log `offset` and `handle` at the existing `cuMemMap` hook under
   `PCOMM_VERBOSE` and confirm on MN5 that NVSHMEM's heap arrives with a
   non-trivial `offset` and a stable handle. **Do this before designing the
   record** — if the offsets are all zero the join key carries everything and
   the record shrinks.
2. Add the `cuIpcGetMemHandle` / `cuIpcOpenMemHandle` hooks; check whether
   NCCL's p2p path (case 05) uses IPC or VMM. They may need different arms.
3. `PeerMapping` record + intern domain + both-sides emission.
4. `link_sql.hpp`: the exact join, `exact = TRUE`, `link_health` finding for
   mappings with no matching export.
5. Re-run cases 13-15 on MN5 (**`--per-case`** — see
   [[pcomm-mn5-combined-alloc-breaks-multinode-cases]]) and assert the bands
   against the ib twin.

## The assertion gap this sits on top of

Worth its own decision, because it is why the two fixed defects shipped at all:
**no suite assertion can reach `object_access`.** `pcomm export --csv` does not
emit the derived views — it re-derives `comm_edges` in C++ (`CsvAgg`,
`pcomm-read/main.cpp:1526`), and that reimplementation is the only derived table
the asserts can see. Case 13's own `hub_buffer_shared.sql` counts writers
straight off `gpu_access.run_dev`, so it passed throughout while the view it was
meant to protect reported three phantom objects.

Adding `object_access` to the CSV path would mean reimplementing the whole join
in C++ — a second forked derived layer, which is the failure `ui/README.md` and
`link_sql.hpp` are both written to prevent. The alternative is to let the suite
assert against `--link` Parquet (or a `pcomm ui` instance) instead of CSV, for
derived tables only. That is a change to how the suite reads a capture, not to
what it checks, and it should be decided before more assertions are written
against a table the UI does not use.
