# PLAN — what the application allocated should not depend on which provider ran

- **Created:** 2026-08-10 05:05 +03
- **Status:** TODO — measured, designed, not started.
- **Supersedes** the placement argument in
  `PLAN-20260806-1329-generic-memory-attribution-todo.md` ("Where the
  nvshmem_malloc interposer belongs"). That section concluded the current
  placement was fine and the trigger for moving it was "a second consumer". The
  measurement below shows the trigger already fired — it just was not a second
  *provider*, it was the UI.
- **Related:** `PLAN-20260807-1718-ui-backbone` (the Object and GPU views this
  blocks); `PLAN-20260806-1329` (independent emitters, dedup by interned id —
  which stays).

## The measurement

The same NVSHMEM workload, captured two ways:

| capture | provider | `mem_object` rows | sizes |
|---|---|---|---|
| `runA` (case 12) | mlx5dv only | 20 | 8, 40, 327680 |
| `close2` | gpumem (p2p) | 62 | 8, 16, 88, 256, 804, 808, **4096**, 536870912 |

The application allocates two 4 KB buffers with `nvshmem_malloc`. **They appear
in one capture and not the other.** The mlx5dv rows are DEVX umem registrations
— control structures and work queues, i.e. *the transport's* objects. The
program's own objects are absent.

So "does this capture know what the application allocated" currently answers
"only if you ran NVBit". Case 05 measures NVBit's fixed cost at ~435 s of module
lifting before any of our work starts. Paying that to learn a buffer's name is
backwards.

### Precisely what the transport does and does not give you

Not "the transport's addresses are fake" — they are real memory, and saying
otherwise would be wrong. Three distinct situations, measured on the same
workload:

| registration | address in the OBJECT record | whose buffer | linkable |
|---|---|---|---|
| umem/MR `by_addr` | real (`0x727cf2400000`) | NVSHMEM's own | yes |
| umem `by_dmabuf` | real (`0x7e7f2e400000`) — recovered | NVSHMEM's own | yes |
| `nvshmem_malloc` | real (`0x628681800`) | **the application's** | **only if gpumem ran** |

**The dmabuf row resolves, and that was already built.** The umem record keeps
`addr = 0x0` and `fd = 84`, faithfully recording what the API was handed — but
`core/dmabuf_registry.hpp` intercepts the `cuMemGetHandleForAddressRange` that
NVSHMEM must call to *obtain* that fd, so `fd -> (dptr, size)` is known before
the registration and mlx5dv emits a MemObject with the real range. Measured on
runA: umems at `0x0`/fd 84 with sizes 8, 327680, 40 have MemObject rows at
`0x7e7f2e400000`, `0x7e7f2e460000` with the same sizes. Nothing here needs
fixing.

The sizes give away whose buffers they are: 8, 40 and 327680 bytes — control
structures and work queues. NVSHMEM does not register per-`nvshmem_malloc`; it
registers what *it* hands the NIC.

**So the remaining gap is exactly one row of that table, and nothing else.** The
link is not broken and no new join is needed: `mem_owner` already resolves an
address to the object containing it, and every address it needs is already
present — including the dmabuf ones. What is missing is one END of the join. On
an ib/DEVX-only capture nothing records `dst`, so the transport says "the NIC
was given this range" and nothing says a range inside it is the buffer the
program allocated.

## Why the earlier answer was wrong

`PLAN-20260806-1329` decided **independent emitters, dedup by interned id**, and
that decision is correct and stays. What I got wrong was concluding from
"ibverbs and mlx5dv emit MemObject too" that the concern was already covered.
They emit *transport* objects — a registered MR, a umem, sometimes with no
address at all. An application object and a transport artifact are different
things that happen to share a record type, and counting rows made them look
interchangeable.

The distinction is exactly what the UI needs. "Which GPUs touched this object,
and from which line" is a question about `dst`, not about a 327680-byte work
queue.

## The constraint that shaped the current placement, and why it does not bind

`PLAN-20260806-1329` forbids a provider asking **another provider** for memory
information at runtime, and gpumem genuinely needs a buffer's extent at drain
time to attribute an access. That is why the hook sits with gpumem today.

The constraint says nothing about asking the **core**. `core/mem_attr.hpp`'s
`device_of` is the precedent: extracted into the core precisely so two providers
would not carry two copies, and called by whoever needs it. A core-owned object
registry is the same shape.

## Design

**The core owns the registry and the allocator interposers. Providers keep
emitting what only they can see.**

1. **Registry in the core**, reachable over the C ABI that gpumem already uses
   for interning and recording. One lookup: address → (object, base, size,
   device). This is the piece that lets gpumem stop owning identity while
   keeping attribution.

2. **Allocator interposers in the engine**, which is preloaded first and
   therefore always wins the symbol. `nvshmem_malloc` / `calloc` / `align` /
   `free` to begin with, because that is where the measurement bites. The engine
   already interposes `dlsym`, so interposition is not new to it — what is new
   is interposing a *third-party library*, and that is the part to argue about
   in review rather than assume.

3. **The core emits `MemObject` and `EntityClose`** for what it observes.
   Providers continue to emit for what the core cannot see — a dmabuf umem has
   no address to intern, and only mlx5dv knows it exists. Dedup is unchanged:
   everyone interns the base in `InternDomain::Pointer`, so the same buffer
   mints the same id and the rows merge.

4. **gpumem consults the core registry** instead of registering symmetric
   buffers itself, and keeps its own registry only for what NVBit alone sees
   (cuMemAlloc'd buffers, mapped regions, peer mappings and their `sym_delta`).

**Rejected: a dedicated object-tracking provider.** It is the tidy answer and it
reintroduces the exact problem the constraint exists for — gpumem would have to
ask it at runtime. The registry has to live below the providers, not beside
them. (This is narrower than the "memory-tracking monopolist" `PLAN-20260806-1329`
rejected: that one wanted to own *all* allocation observation, including the
dmabuf case no single interposer can see. This owns the registry and the
allocator APIs that have addresses, and leaves provider-only sightings alone.)

## Steps

1. Core registry + C ABI lookup. No behaviour change yet: gpumem still registers
   its own, the core's is empty.
2. Move the `nvshmem_*` interposers from `gpu-backends/nvbit/nvbit_backend.cu`
   into the engine. They are not an NVBit concern and that file's own header
   reserves it for vendor instrumentation types.
3. gpumem consults the core registry in `ObjectAttributor` before its own.
   Acceptance: the p2p capture still attributes 40960 accesses to the 4 KB
   buffer, unchanged.
4. **The acceptance that matters:** re-run case 12 (mlx5dv only, no NVBit) and
   assert the 4 KB buffers now appear in `mem_object`. That is the whole point,
   and it is a single measurable row count against the table above.
5. Only then, an assertion: an NVSHMEM workload's `nvshmem_malloc` buffers are
   present regardless of which providers were loaded.

## Risks

- **The engine interposing NVSHMEM is a layering change.** The core currently
  knows about no third-party library. The counter-argument is that it already
  interposes `dlsym` and owns interning, and that the alternative is N copies in
  N providers — but this is the decision to make deliberately, not by drift.
- **Two emitters for one buffer** once the core and gpumem both see a
  `cuMemAlloc`. Dedup by interned id already handles it and `mem_owner` reports
  `devices > 1` when emitters disagree; the risk is not correctness but a
  doubled `emitters` count that reads as significant when it is not.
- **`nvshmem_free` ordering.** The close must be emitted before the real free
  returns, since the address can be reallocated immediately. Already handled in
  the current hook; the move must not lose it.

## Explicitly out of scope

- `cudaMalloc`/`cuMemAlloc` interposition in the core. NVBit already sees those
  and gpumem needs them for its own registry regardless; moving them buys
  nothing until a non-gpumem capture needs plain device allocations named.
- Source-line attribution for objects (the `adm` port), which is a separate
  enrichment and does not change where tracking lives.
