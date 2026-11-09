# PLAN — memory attribution belongs to the core, not to whichever provider happens to see the allocation

- **Created:** 2026-08-06 13:29 +03
- **Status:** SHIPPED. `device_of` is in `core/mem_attr.hpp` (RTLD_NOLOAD,
  no negative caching, `kAttrDeviceOrdinal = 9`); `MemObject` moved 720->520 into
  the internal range; ibverbs, mlx5dv and the engine all emit it, deduped by
  interning the base in `InternDomain::Pointer`; dmabuf/memoryless doors withhold
  it rather than fabricate a base-0 object. Steps 1-5 done. Step 6 (lifetime-
  scoped range resolution in the reader) is deferred with
  `PLAN-20260805-0053-entity-lifetimes`. The placement question this plan opened
  was settled by `PLAN-20260810-0505` (core registry, not a provider).
- **Depends on:** `PLAN-20260805-0053-entity-lifetimes-done.md` for the *time*
  half. Steps 1-5 land without it and are useful without it; step 6 is what it
  unblocks.
- **Related:** `PLAN-20260803-1448-nvshmem-heap-attribution-done.md` (the last
  time attribution was wrong and it inflated a whole capture);
  `PLAN-20260803-2253-ibgda-device-initiated-capture-done.md` (the
  `kAttrDeviceOrdinal` bug that motivated this).

## What is broken

`MemObject` is already the right abstraction and already lives in the core:

```cpp
// libpcomm/include/pcomm/core/records.hpp:31
PCOMM_RECORD(MemObject, RecordKind::Entity, 720, "mem_object") {
    Handle id; Handle name; MemRange range; DeviceId device;
};
```

Three things are wrong with how it is owned, and none of them is the concept:

1. **Its id, 720, sits inside `gpumem_lo=700, gpumem_hi=799`.** A core-declared
   record living in one provider's id range, so "any provider may emit a
   MemObject" is false by convention even though it is true by declaration.
2. **gpumem is its only producer**, and gpumem obtains it through NVBit. So
   answering "which GPU owns this buffer" currently costs instrumentation of
   every global memory instruction — for a fact obtainable with one driver call.
3. **`ib_mem_region` has no device field at all** (`mr, pd, range, lkey, rkey,
   access`). The oldest and largest part of the capture cannot say whether a
   registered buffer was GPU or host memory. Case 07 registers 216 regions of
   ~38 MB — NVSHMEM's symmetric heap, i.e. device memory moved by GPUDirect
   RDMA — and the capture records nothing that says so.

Meanwhile `mlx5dv_provider.cpp` carries a private `cuda::device_of()`. ibverbs
is about to need the same function. Extracting it *before* the second copy
exists is the whole reason this plan is being written now: the first copy had a
wrong constant (`kAttrDeviceOrdinal = 4`, which is
`CU_POINTER_ATTRIBUTE_HOST_POINTER`; the answer is 9) that survived for the
provider's entire life. Two copies means two places for that to happen.

## Design decision, and the one that shapes everything else

**Any provider may hook CUDA APIs itself.** This is deliberate and was decided
explicitly: a provider may need allocation information for its own *runtime*
decisions, not merely to record it, and routing that through another provider
would either not arrive in time or not arrive at all. There is no memory-tracking
monopolist.

This is a constraint on the design, not an aspiration. Any proposal that
requires a provider to ask another provider for memory information at runtime is
out of bounds, however tidy it looks.

**The engine does not currently permit it** (found 2026-08-06, not yet fixed).
`smart_dlsym::register_symbol` allows exactly one callback per symbol and
*throws* on a duplicate:

```cpp
if (RegisteredSymbols->contains(symbol_name))
    throw std::runtime_error("Symbol already registered");
```

Since NVSHMEM reaches every CUDA entry point through a single
`dlsym(handle, "cuGetProcAddress")` (see the IBGDA plan's 2026-08-06
refinement), any provider wanting CUDA interception must register exactly that
symbol — so the *first* one to do so claims it, and the second aborts the
application at load. That is the `devx_probe` failure mode again: a hook
breaking the process it was meant to observe passively.

Prerequisite, therefore, before any provider hooks CUDA: make the registry hold
a **chain** per symbol, each callback receiving what the previous returned, so
registration order composes instead of colliding. Order must not matter for
correctness — if it does, two providers are fighting over the same entry point
and that is a design error the chain should surface, not hide.

The direct consequence is that **two providers can observe the same allocation**,
and the failure mode is already documented in this repo: hooking both
`ibv_reg_mr` and `ibv_reg_mr_iova2` made case 01 record every region twice —
same mr, same base, same lkey — because rdma-core implements one on top of the
other and our interposers are global. The same shape will appear here the first
time two providers both attribute the same buffer.

So the design is: **shared vocabulary and shared helper, independent emitters,
dedup by construction.**

Dedup key: `MemObject.id` is a `Handle`, and handles are interned through the
shared engine. If every emitter interns the object's base address in the same
domain (`InternDomain::Pointer`), the same buffer yields the *same* id from any
provider, and duplicates become detectable — and mergeable — rather than silently
doubling a count. This uses machinery that already exists rather than adding a
registry.

## Steps

Each step is independently landable and independently verifiable. Do not batch
them; the schema change in step 2 is the only irreversible one.

### Step 1 — extract `pointer → device` into the core

New `libpcomm/include/pcomm/core/mem_attr.hpp` (header-only, no CUDA link):

- `pcomm::core::device_of(const void*) -> DeviceId`, returning `kUnknownDevice`
  when it cannot tell.
- Promote `kUnknownDevice` from `providers/mlx5dv/mlx5_records.hpp:30` to
  `core/vocab.hpp` beside `DeviceId`, which is already there. mlx5dv keeps a
  `using` so its records still read the same.

Non-negotiable properties, each of which is a bug already paid for:

- **`RTLD_NOLOAD`.** This must borrow libcuda from a process that already has
  it and must never *load* it into one that does not. A hook that manufactures
  a capability the application declined is the `devx_probe` failure.
- **Do not cache a negative.** Cache once resolved; retry while unresolved. A
  latched miss makes attribution permanently dead.
- **The `kAttr*` constants are hand-copied from `cuda.h` and nothing checks
  them.** Keep the comment saying so. `MEMORY_TYPE=2`, `DEVICE_ORDINAL=9`,
  `CU_MEMORYTYPE_DEVICE=2`.
- **It must compile and run with no CUDA present.** MN5's core configuration
  builds exactly that way, and the suite must stay green there.

Acceptance: mlx5dv uses the shared helper and cases 09/10/11 still pass on du04
with `device=0`/`device=1` on rank 0/1 (case 10) and `65535` (case 11). This is
a pure refactor — the values must not move.

### Step 2 — move `MemObject` out of gpumem's id range

720 → **520**, in the internal range. The range is currently sparse and all
three occupants are artifacts: `InternKey` 510, `InternString` 512,
`ClockAnchor` 590. There is no entity in the internal range yet, so 520 opens an
entity block and leaves 522+ adjacent for `EntityClose` when the lifetimes plan
lands — the two are joined at the hip and should read that way in the id map.

This is a schema change and touches, in order: the record declaration, the
FlatBuffers schema, the generated bindings (`tools/get_flatc.sh`,
`tools/check_generated.sh`), the conformance test, the reader table, and
`kSchemaHash`. `tools/schema_check.py` enforces all five and will name whichever
is missed — it did exactly that for the mlx5dv records.

Every golden that carries the hash changes. That is expected and mechanical;
regenerate rather than hand-edit.

Acceptance: `schema_check` clean, `ctest` green, gpumem still emits `mem_object`
identically on du04 (case 03 or 05), and the reader's `mem_object.csv` is
unchanged apart from provenance.

### Step 3 — emit `MemObject` from ibverbs

In the `reg_mr_iova2` post-callback, alongside the existing `IbMemRegion`:
attribute the address with the step-1 helper and emit a `MemObject` interned on
the base pointer.

Keep `IbMemRegion` as it is. Do **not** add a `device` column to it: the point
of this plan is that attribution lives in one record type, not smeared across
every provider's entity. The join is `ib_mem_region.base` → `mem_object.range`.

Acceptance, and it must be a derived assertion rather than a golden: on case 07
(NVSHMEM multinode), the ~38 MB symmetric-heap registrations resolve to a
`MemObject` with a real device ordinal; the small control buffers resolve to
host. Do not assert an absolute region count — case 06 already showed that
tracks allocator churn (115 vs 114 across racks).

### Step 4 — the same for mlx5dv, where it is *not* free

mlx5dv already has the device on `Mlx5Umem`. Emitting `MemObject` there is
straightforward for the `by_addr` binding and **impossible for `by_dmabuf`**:
the umem carries `addr=0x0` and a descriptor, so there is no pointer to
attribute. Emit for `by_addr`; for `by_dmabuf` emit nothing rather than an
object with a fabricated range.

This is the same gap recorded in `things-to-circle-back-to.md` as "`gpu_driven`
under dmabuf is still a heuristic", and it closes the same way: via
`fd → GPU VA` (`cuMemGetHandleForAddressRange` through `smart_dlsym`), which is
out of scope here.

### Step 5 — the reader join

`pcomm-read` resolves an address to a `MemObject` by range containment, scoped
to the rank. The precedent is `comm_edges`, which already resolves
`(dest_lid, dest_qp_num) → rank` — **and note it is keyed on the pair**, because
`qp_num` collides across ranks and a `qp_num`-keyed map resolved every colliding
peer to whichever rank was seen last. The same discipline applies here: scope
containment lookups by rank, never by address alone.

Dedup: when two providers emit a `MemObject` with the same interned id, merge
rather than double-count. If they disagree on `device`, that is a finding —
report it, do not silently pick one.

Acceptance: an assertion that no address in `ib_post_send`/`gpu_access` resolves
to more than one `MemObject` within a rank.

### Step 6 — time, once lifetimes exist

Range containment is only sound *within a lifetime*. A freed and re-registered
address matches two objects, and today nothing bounds either interval because no
entity has a close record.

`MemObject` carries a `Temporal`, so it *can* hold a window — but writing one
requires knowing the free at emit time, and the streams are append-only. That is
the same wall the entity-lifetimes plan hit, and the same answer applies:
`EntityClose` (Design A), not back-filled windows. Do this step only after that
plan lands.

## Risks

- **Double emission** is the predicted failure and step 5's assertion is what
  catches it. Expect it to fire the first time gpumem and ibverbs run together
  (case 06 is the coexistence case).
- **The schema hash churn in step 2 will touch every golden.** Do it as its own
  commit so that a later bisect can tell a hash change from a behaviour change.
- **`device_of` returning `kUnknownDevice` is ambiguous** — it means "host
  memory" *and* "CUDA is not present" *and* "the call failed". That ambiguity is
  what hid the wrong-constant bug for the provider's entire life. If step 1 does
  nothing else, it should at least keep the diagnostic that distinguishes them
  (`PCOMM_TRACE_HOOKS=1` prints stage/rc/type at the call site).
- **Do not let this become a reason to make gpumem optional-by-default.** It is
  not a replacement for per-access attribution; it replaces per-*allocation*
  attribution only. Those are different questions and the plan should not be
  read as retiring NVBit.

## Where the nvshmem_malloc interposer belongs (2026-08-10)

gpumem now interposes `nvshmem_malloc`/`calloc`/`align`/`free` to name symmetric
buffers, and "why is that gpumem's job?" is a fair question with a boring
answer: gpumem is the only component that needs the buffer's **extent at
runtime**, because `ObjectAttributor` resolves an access to an object as the
channel drains. This plan's own constraint forbids the alternative — a provider
asking another provider for memory information at runtime is out of bounds — so
gpumem cannot delegate the observation and still attribute.

It is also consistent with the shape decided above: independent emitters, dedup
by construction. ibverbs and mlx5dv already emit their own `MemObject` rows
(1014 on nccl_internode, 4553 on scale_nccl, with no gpumem loaded), and every
emitter interns the base in `InternDomain::Pointer`, so a second provider
hooking `nvshmem_malloc` would mint the SAME id and the rows would merge rather
than double.

**Two things are nevertheless wrong with the current placement, and both are
cheap to fix when they start to matter:**

1. **The interposers live in `gpu-backends/nvbit/nvbit_backend.cu`**, whose own
   header states that vendor types are allowed there "and nowhere above". They
   are not an NVBit concern at all — NVBit instruments instructions; this is
   library interposition. They belong in `providers/gpumem/` proper, which needs
   only a way to reach the engine instance from the provider translation unit.

2. **The extraction trigger is a second consumer, and this plan already set the
   precedent.** `device_of` was moved into the core *before* the second copy
   existed, explicitly because the first copy had carried a wrong constant for
   its entire life. The same applies here: the moment a second provider wants
   symmetric-buffer identity, the interposer goes to the core — reached over the
   C ABI, like every other core service gpumem uses — rather than being copied.

Neither is urgent while gpumem is the only consumer. Both become urgent the
moment it is not, and the cost of noticing late is a second copy that drifts.

**SUPERSEDED the same day.** The trigger had already fired and this section
missed it, because it looked for a second *provider* and the second consumer is
the UI. Measured: the same NVSHMEM workload captured with mlx5dv only has 20
mem_object rows (8, 40, 327680 — DEVX umems, i.e. the transport's objects) and
the two 4 KB buffers the application allocated are simply absent; captured with
gpumem it has them. "What did the program allocate" therefore answers "only if
you ran NVBit", at ~435 s of module lifting. Transport artifacts and application
objects are different things sharing a record type, and counting rows made them
look interchangeable. See
PLAN-20260810-0505-object-tracking-is-not-a-provider-concern.

## Explicitly out of scope

- A dedicated memory-tracking provider. Rejected on the grounds that allocation
  events are observed at different layers by different interposers — NVBit sees
  `cudaMalloc`, ibverbs sees `reg_mr`, mlx5dv sees `devx_umem_reg` for buffers
  that have no address at all. No single interposer sees them all, so a
  monopolist would need the union of every allocator hook and would *still* miss
  the dmabuf case.
- `fd → GPU VA`. Needed by step 4's dmabuf half and by the IBGDA data plane;
  tracked in the IBGDA plan.
- Adding `device` to `ib_mem_region`. Deliberately not done — see step 3.
