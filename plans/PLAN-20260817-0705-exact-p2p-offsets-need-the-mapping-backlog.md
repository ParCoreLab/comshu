# Exact p2p object offsets: record the mapping, not the library

Status: **BLOCKED 2026-08-17**, on evidence rather than effort. Both routes to
peer-memory object identity were probed and both are shut on this stack: the
driver API cannot tell an application buffer from allocator backing (reverted,
measured), and the POSIX-fd shareable handle carries no allocation identity to
join on (probed, job 44705498). The sections from *The general fact* onward were
written before those two results and are kept for their reasoning, not as a plan
— read the two blocker sections first.
Created: 2026-08-17 07:05
Touches: `gpu-backends/nvbit/nvbit_backend.cu`,
`libcomshu/include/comshu/core/records.hpp`, `providers/gpumem/engine.hpp`,
`comshu-read/link_sql.hpp`

## What is already fixed, and what this is not

Two defects in `object_access`'s p2p branch were fixed on 2026-08-17 and are
**not** what this plan is about:

- `owner_rank` was the rank that *wrote*, not the rank that *owns* — the opposite
  of what the verbs branch means by the same column.
- the offset window partitioned on `(rank, object)`, gluing one actor's mappings
  of the same symmetric object id on several peers into one window (measured: a
  2048-byte object reporting offsets to 549,755,815,936).

A third was fixed the same day: `object` was carried across ranks unchanged,
though it is an intern handle minted per rank — on `nvshmem_coexist-44696991` id
701 names four different buffers, and 139 ids have ranks disagreeing about their
extent. It is now translated through the owner's `mem_object` **by range**, which
is the rule `records.hpp` already states for the reader's own merge.

What remains is the fourth thing, which no SQL change can reach: a p2p
`obj_offset` is anchored to the **lowest address observed**, so it is only the
true offset when the workload happened to touch byte 0. `exact = FALSE` marks it
and the Objects panel renders "(approx.)" — this is honest today, just not right.

**The second thing it costs is object attribution for every non-NVSHMEM
allocator, and that one is NOT a join problem.** It is worth being exact about
where the gap is, because it looks like a join problem and is not.

`AllocRegistry::add_range` (`providers/gpumem/engine.hpp:66`) records a mapped
peer region as **device-only, with `kNullHandle` for the object**, and does so
deliberately: "that would mint an object handle for an entity nobody recorded,
leaving GpuAccess.object pointing at a MemObject row that does not exist." Object
handles come from the NVSHMEM pass, which knows a region is symmetric memory.
NCCL has no such pass, so on `nccl_p2p-44688653` **every one of the 1,179,948
accesses carries `object = 0`**, and the capture holds exactly one `mem_object`
per rank — 1 MiB — which is not the buffer being written.

So neither side recorded the buffer. No range match, size match or uniqueness
heuristic can recover it, because there is nothing on either end to match
against. Those rows previously sat in `object_access` pointing at a
non-existent object and are now dropped and counted in
`link_health.unattributed_p2p_accesses` — the same information, labelled
honestly.

This is the strongest argument for recording the mapping, and it is a *capture*
argument rather than a query one.

**And the first step is smaller than it looks, because the gap is one unhooked
door.** `nvbit_backend.cu` intercepts `cuMemAlloc`, `cuMemAlloc_v2` and
`cuMemAllocManaged` — which mint objects — and `cuMemMap`, which cannot.
**`cuMemCreate` is not hooked at all.** So a VMM allocation is observed only at
the moment it is *mapped*, by which point the thing being mapped has no recorded
identity, and `add_range` is right to refuse to invent one.

Verified on `nccl_p2p-44688653`, which is a Ring/Simple allreduce over NVLink
(`NCCL_P2P_LEVEL=NVL`, one channel, host-initiated — not GIN):

- The workload's own `cudaMalloc` **is** recorded: one 1 MiB object per rank,
  because `cudaMalloc` routes to `cuMemAlloc_v2`.
- A Ring/Simple allreduce never peer-writes into the user buffer; it stages
  through NCCL's internal channel buffers on the peer. Those are VMM memory —
  the job log reports "9/9 new mapped regions attributed by CUDA" per rank.
- Result: rank 1's own buffer is at 140629078179840 while its peer writes start
  at 140629092859904, ~14 MB higher, spanning ~8.3 MB, and **zero of the
  1,179,948 accesses fall inside any recorded `mem_object`**.

## Tried and reverted: naming VMM memory from the driver API alone

**Do not re-attempt this shape.** Implemented and measured on MN5 2026-08-17
(jobs 44698761, 44699012, 44699694), then reverted in full.

The idea was that `cuMemCreate` is simply an unhooked door: hook it, remember the
handles minted in this process, and a `cuMemMap` of one of them is as nameable as
a `cuMemAlloc`'d buffer, so `add_range` could become `add`. It compiles, it runs,
and it is wrong for a reason no care at the call site fixes.

**CUDA's own pooled allocator uses the same three doors, in granularity chunks.**
On case 04 the object count per rank went from 38 to **818** - 780 of them
exactly 2 MiB, at `0x7fc9...`-`0x7fe0...`, which is pool backing rather than
anything the program allocated.

Two narrowings were tried and both failed:

1. **Anchor on the reservation, not the chunk** (`cuMemAddressReserve`), on the
   theory that a pooled allocator reserves one range and maps many chunks into
   it. Still 818: the pool reserves *per chunk* as well.
2. **Require an observed reservation**, falling back to `add_range` otherwise.
   Still 818, for the same reason - the reservations exist, they are just 2 MiB
   each.

**The conclusion is about the API, not the implementation.** `cuMemCreate`,
`cuMemAddressReserve` and `cuMemMap` carry no signal distinguishing an
application buffer from an allocator's internal backing, because for the driver
there is no difference - CUDA's pool is just another client of the same
interface. "We created it in this process" is therefore not evidence of an
application object, and `AllocRegistry::add_range`'s refusal to name what it only
located is right for a deeper reason than the one its comment gives.

That is why the NVSHMEM pass works: it asks NVSHMEM, at the library boundary,
which regions are symmetric memory. Naming a buffer needs a source that knows
what a buffer *is*, and the driver is below that line.

So the remaining routes are:

1. **The library boundary**, as NVSHMEM already does - for NCCL that means
   interposing its buffer registration, a much larger commitment than a CUDA hook
   and what `add_range`'s comment is really waiting for.
2. **The cross-process handle pair** (`cuMemExportToShareableHandle` /
   `cuMemImportFromShareableHandle`), which does not identify what a buffer is but
   *does* identify which allocation a mapping is a window onto. That is enough for
   the offset translation this plan is actually about, and it never has to decide
   whether the thing is an application buffer - only which side owns it.

Route 2 remains the plan. Route 1 is the only thing that would give NCCL captures
object *identity*, and it should not be started expecting the driver API to
shortcut it.

## Route 2 is blocked too: the fd carries no allocation identity

Probed on MN5 2026-08-17, job 44705498 (`tests/hpc/probes/vmmdoors/`), BEFORE
building anything — which is the whole reason this cost one job instead of three.

The premise held in part. Both workloads do share peer memory through the VMM
export/import pair, and neither touches the legacy `cuIpc*` doors at all:

| door | calls |
|---|---|
| `cuMemCreate` | 206 |
| `cuMemMap` | 230 |
| `cuMemExportToShareableHandle` | 16 |
| `cuMemImportFromShareableHandle` | 32 |
| `cuIpcGetMemHandle` / `cuIpcOpenMemHandle` | **0** |

So there is a door, and it is the VMM one. What there is not is a **key**.

Every export and every import reports `type=1` —
`CU_MEM_HANDLE_TYPE_POSIX_FILE_DESCRIPTOR` — and fstat on both sides gives the
**same `dev=6 ino=1729` for all 48 of them**. The fd is a handle onto the NVIDIA
driver node, not a per-allocation object: the allocation's identity lives inside
the driver and never reaches the filesystem. So the two candidate keys are both
gone — the fd *number* is process-local by construction, and the inode is a
constant.

A fabric handle (`CU_MEM_HANDLE_TYPE_FABRIC`) would be globally meaningful, but
nothing here requests one, and configuring the workload to use one so the tracer
can read it would be measuring a run nobody else does.

**What this says about the general problem.** NVSHMEM attribution works because
`nvshmem_ptr(addr, pe)` is exactly the cross-process correspondence — a function
from *my* symmetric address to *where PE pe's copy lives in my process*. That is
the fact peer attribution needs, and NVSHMEM hands it over. CUDA's VMM API
exposes no equivalent: it will map a peer's allocation for you and tell you
nothing about which allocation it was.

So peer-memory OBJECT attribution needs a source that knows the correspondence,
and only two exist:

1. **The library that made the mapping.** For NCCL that means its registration
   path, the same shape as the NVSHMEM pass. This is the only route left for
   NCCL and it is not small.
2. **A fabric-handle stack**, where the shareable handle is globally meaningful
   and the join is trivial — worth re-probing on any machine whose NCCL uses one,
   since that turns route 2 back on for free.

Neither is a query change, and no heuristic over `mem_object` substitutes for
them: the correspondence is absent from the capture, not merely hard to compute.

## Both remaining routes, probed (job 44709082)

`tests/hpc/probes/ncclsym/` ran NCCL 2.29.3 bare and under the tracer.

**Route 1 — NCCL's symmetric window does not give VA symmetry.**
`ncclCommWindowRegister(..., NCCL_WIN_COLL_SYMMETRIC)` succeeds on all four
ranks and returns the *same* window handle (`0x25e000d048`), but the buffer
`ncclMemAlloc` returned is at a **different address on every rank**:

```
rank 0  0x7f4055200000     rank 2  0x7f01b1200000
rank 1  0x7efa9f200000     rank 3  0x7f151b200000
```

So the window is symmetric in NCCL's bookkeeping, not in the VA space. The
property `object_access` exploits — same `(base, size)` on every rank, which is
what makes NVSHMEM translate — is absent, and hooking `ncclMemAlloc` /
`ncclCommWindowRegister` would name the owner's buffer without providing the
correspondence that peer attribution actually needs.

**Route 2 — fabric handles are supported here, but NCCL will not ask for one.**
`CU_DEVICE_ATTRIBUTE_HANDLE_TYPE_FABRIC_SUPPORTED` is **1** on all four H100s,
so the join key route 2 needs is not a hardware limitation. But NCCL selects
`CU_MEM_HANDLE_TYPE_FABRIC` only for **MNNVL** topologies — the library's own
strings are `MNNVL busId %lx fabric UUID %lx.%lx cliqueId %x` — and it exposes no
env knob for the handle type (`NCCL_CUMEM_ENABLE`, `NCCL_WIN_ENABLE`,
`NCCL_SYM_NOWIN_ENABLE` exist; nothing selects the handle type). On MN5, which is
NVLink islands within a node rather than an NVSwitch fabric across nodes, NCCL
uses POSIX fds — the handles already proved identity-free.

**So the fabric arm is implementable and testable only synthetically here.** A
two-process test that does `cuMemCreate` with `CU_MEM_HANDLE_TYPE_FABRIC`,
exports, imports and maps would validate the join end to end, and the capability
would then be real on any MNNVL machine (GB200 and similar). Nothing in the
current corpus would exercise it, and no existing capture gains anything.

**A flaw in that probe, recorded so it is not repeated:** the traced run's mpirun
output was piped through `grep -E "ncclsym|comshu-vmm"`, so when the run stopped
after the fabric print — NCCL under NVBit, the same instability that bus-errored
case 05 — the error message was filtered away and the failure looked like an
empty capture. The bare run is what answered both questions. Do not filter the
output of a probe whose failure mode is unknown.

## The fabric arm is built, and cannot be exercised on MN5

`PeerMapping` (record 526) is implemented end to end — backend hooks, engine
emission, reader table, CSV/Parquet, and a byte-identical layout assertion in
`schema_conformance`. It carries the interned `CUmemFabricHandle` hex, the local
`[base, size)`, `cuMemMap`'s own `alloc_offset`, the device, and which end of the
share the row is.

**It has never seen a real fabric handle, and on this machine it cannot.**
`tests/hpc/probes/fabricshare/` asks for one directly, and job 44709806 answers:

```
[fabric] rank=0 dev=0 fabric_supported=1
[fabric] rank=0 FAIL cuMemCreate(&mine, N, &prop, 0) -> CUDA_ERROR_NOT_PERMITTED
```

So the distinction that matters is one the earlier probe could not see:
`CU_DEVICE_ATTRIBUTE_HANDLE_TYPE_FABRIC_SUPPORTED = 1` says the **GPU** can
export a fabric handle; it does **not** say the system will let a process create
one. That needs the IMEX daemon / fabric manager configured, and MN5 does not
have it. Reading the device attribute as "fabric works here" would have been
wrong, and is exactly the kind of inference this probe exists to prevent.

**Status of the arm, stated plainly:**

| part | state |
|---|---|
| record layout | verified — `schema_conformance` asserts it byte-for-byte against the FlatBuffers struct |
| emission path | **unexercised** — no fabric handle has ever reached it |
| link-side join | **not written**, deliberately: building a join on top of an emission path nothing has run would compound unvalidated code |

The cost of keeping it is one schema-hash bump (0xA7144CDF1C9EF478 →
0xDECE2918D9647FAE); strides are unchanged, so captures decode with a warning
rather than a refusal. The benefit is that on MNNVL hardware — GB200 and
similar, where NCCL *does* choose fabric handles — the capture side is already
there and only the join remains.

**Do not write the join until something populates the table.** The first machine
with IMEX configured should run `tests/hpc/probes/fabricshare/` first; it prints
the allocation-id histogram, and a count of 2 on a single id is the whole
validation.

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

> **Superseded 2026-08-17.** What follows was the design before the two probes
> above. Its reasoning about *where* the delta lives is still correct; its
> assumption that a shareable handle is a usable join key is not.


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
   `COMSHU_VERBOSE` and confirm on MN5 that NVSHMEM's heap arrives with a
   non-trivial `offset` and a stable handle. **Do this before designing the
   record** — if the offsets are all zero the join key carries everything and
   the record shrinks.
2. Add the `cuIpcGetMemHandle` / `cuIpcOpenMemHandle` hooks. **Case 05 is
   answered: NCCL uses VMM**, nine mapped regions per rank and no IPC — so the
   VMM arm is the one that matters for it, and the IPC arm is for whatever else
   turns up. Note the negative result above before touching these: hooking the
   VMM doors tells you a mapping's *allocation*, never whether that allocation is
   an application buffer.
3. `PeerMapping` record + intern domain + both-sides emission.
4. `link_sql.hpp`: the exact join, `exact = TRUE`, `link_health` finding for
   mappings with no matching export.
5. Re-run cases 13-15 on MN5 (**`--per-case`** — see
   [[comshu-mn5-combined-alloc-breaks-multinode-cases]]) and assert the bands
   against the ib twin.

## The assertion gap this sits on top of

Worth its own decision, because it is why the two fixed defects shipped at all:
**no suite assertion can reach `object_access`.** `comshu export --csv` does not
emit the derived views — it re-derives `comm_edges` in C++ (`CsvAgg`,
`comshu-read/main.cpp:1526`), and that reimplementation is the only derived table
the asserts can see. Case 13's own `hub_buffer_shared.sql` counts writers
straight off `gpu_access.run_dev`, so it passed throughout while the view it was
meant to protect reported three phantom objects.

Adding `object_access` to the CSV path would mean reimplementing the whole join
in C++ — a second forked derived layer, which is the failure `ui/README.md` and
`link_sql.hpp` are both written to prevent. The alternative is to let the suite
assert against `--link` Parquet (or a `comshu ui` instance) instead of CSV, for
derived tables only. That is a change to how the suite reads a capture, not to
what it checks, and it should be decided before more assertions are written
against a table the UI does not use.
