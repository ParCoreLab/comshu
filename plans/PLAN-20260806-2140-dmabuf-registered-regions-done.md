# PLAN — dmabuf-registered memory regions are invisible to the ibverbs provider

- **Created:** 2026-08-06 21:40 +03
- **Status:** DONE (2026-08-07). The dmabuf half shipped on 2026-08-06; the
  residue it could not explain turned out not to be a registration door at all —
  see PLAN-20260806-2320-dlvsym-interposer-not-observed, which closed it by
  installing object-ops on contexts opened through doors we do not interpose.
  `entity_close_accounted` now passes on 02, 06 and 07 with the markers dropped.

- **Original status:** PARTLY DONE. `ibv_reg_dmabuf_mr` is hooked (option A, with B's
  capability — see below). It closed NVSHMEM's half and **did not** close
  NCCL's; the residue is narrowed to registrations and is still open.
- **Blocks:** `entity_close_accounted` on cases 02, 06, 07 (marked
  `expect_fail` with the evidence, MN5 job 44358674).
- **Related:** `PLAN-20260806-1329-generic-memory-attribution` (the `MemObject`
  contract this has to satisfy); `PLAN-20260805-0053-entity-lifetimes` (the
  assertion that found it).

## What is broken

`ibv_reg_dmabuf_mr` is an exported libibverbs symbol we do not hook. Every
region registered through it is missing from the capture — not merely missing a
close record, **missing entirely**. Its `ibv_dereg_mr` still fires, which is how
this surfaced: closes naming entities nothing created.

Found by `entity_close_accounted` on its first real run, which is exactly the
job the plan predicted for it ("an unmatched close is evidence of a creation
door we do not hook, which is information we currently have no way to obtain").

## Evidence

Measured (MN5 44358674, case 02, per rank): **290 closes, ~47 unmatched, 120
`ib_mem_region` opens, 102 `ib_queue_pair` opens.** A subset, not everything —
so the close hooks are correct and a registration door is missing.

From source, not inference:

- NCCL resolves it (`reference/nccl-git src/misc/ibvsymbols.cc:43`) and calls it
  (`src/transport/net_ib/reg.cc:35`), plus `mlx5dv_reg_dmabuf_mr` at `:37`.
- NVSHMEM loads it (`reference/nvshmem-3.7.2-0
  src/modules/transport/common/transport_ib_common.cpp:921`).

The failure pattern matches exactly: cases 01 and 08 (`ib_pingpong`,
`ib_send_bw`) take the plain path and PASS; cases 02, 06, 07 (NCCL, NVSHMEM)
take the dmabuf path and fail.

Unlike `ibv_create_qp_ex`, this **is** an exported symbol
(`readelf --dyn-syms /lib64/libibverbs.so.1` finds it), so a plain preload hook
reaches it. Signature (`verbs.h:2632`):

```c
struct ibv_mr *ibv_reg_dmabuf_mr(struct ibv_pd *pd, uint64_t offset, size_t length,
                                 uint64_t iova, int fd, int access);
```

## The decision to make first, and why the hook alone is not the fix

`registration_has_object` requires a `mem_object` for every `ib_mem_region` at
the same `(rank, base, size)`. It is also the positive control for device
attribution. So adding the hook without an answer for `base` turns one red into
another.

**A dmabuf MR's address is only sometimes knowable**, and NCCL demonstrates both
cases in one codebase:

| call site | iova argument |
|---|---|
| `src/transport/net_ib/reg.cc:35` | `addr` — the real buffer address |
| `src/transport/net_ib/gdaki/gin_host_gdaki.cc:131` | `0` |

So `base = iova` is honest where iova is nonzero and a fiction where it is not.
Emitting a `MemObject` with base 0 is the thing mlx5dv already refuses for this
exact reason — range containment would then resolve real addresses against it,
which is worse than a missing row and unrecoverable afterwards.

Three shapes, and this is the choice:

**A. Record the region, emit `MemObject` only when `iova != 0`, and exempt
dmabuf regions from `registration_has_object`.** Requires distinguishing them,
and `IbMemRegion` has no field for that — FlatBuffers structs cannot gain
fields, so it means a new versioned record type, not an edit. Mirrors
`mlx5_umem`, which already carries `binding` and `dmabuf_fd` one layer down.

**B. Resolve the fd through the same `fd → GPU VA` table mlx5dv now keeps.**
Would give a real base even when `iova == 0` — NCCL's gdaki path calls
`cuMemGetHandleForAddressRange` itself (`gin_host_gdaki.cc:129`), so the export
is observable. **Blocked:** that table lives in `providers/mlx5dv/` and is
per-`.so`; ibverbs cannot see it. Sharing it needs the engine, since a static
library linked into two `.so`s gives two copies.

**C. Record the region with `base = iova` unconditionally.** Cheapest, and wrong
in the gdaki case in the specific way this project keeps punishing: a plausible
value that is silently not the thing it claims to be.

**Recommendation: A now, B when the registry is shared.** A is honest and
self-contained; B is strictly better and is one more argument for promoting the
dmabuf table into the engine — which the memory-attribution plan already wants
for `MemObject` identity.

## Steps

1. New versioned record for dmabuf registrations (or `IbMemRegion` v2 carrying
   `dmabuf_fd` + `iova`), through the full schema path: `.fbs`, generated
   bindings, conformance, reader table, hash. `schema_check` enforces all five.
2. Hook `ibv_reg_dmabuf_mr`; emit the region always, `MemObject` only when the
   address is real.
3. Teach `registration_has_object` about dmabuf regions — exempting them
   **explicitly and by name**, not by loosening the existing clause.
4. Drop the `expect_fail` markers on cases 02, 06, 07 and let
   `entity_close_accounted` guard the fix.
5. `mlx5dv_reg_dmabuf_mr` (NCCL `reg.cc:37`) is a second door into the same gap.
   Check whether it is reached in these captures before assuming it is not.

## Constraints

- **Do not weaken `entity_close_accounted`.** It found a real capture gap on its
  first run, which is the entire reason it exists. The `expect_fail` markers
  carry the evidence and come off when the door is hooked.
- **Do not synthesise a base.** An unresolvable address is recorded as absent.
- Verify with the counts above: unmatched closes should go to zero on cases 02,
  06 and 07, and the region count should rise by roughly the same amount.


## What actually happened (2026-08-06)

**Option A shipped, and B came free.** The plan called B blocked on the engine.
That was wrong: the constraint "any provider may hook CUDA itself" is the answer,
not the obstacle. The fd → address table moved to `core/dmabuf_registry.hpp` and
**both** providers install it, each keeping its own copy populated by its own
interception. Only legal because the dlsym registry now chains. So address
recovery is fd-table-first (the only source that also yields the device), then
`iova` when nonzero.

**It closed NVSHMEM's half.** Case 07's symmetric heap went from *absent* to
512 MB + 1 GB regions at `0x10020000000`/`0x10040000000` with `device=0`/`device=1`
per rank. That also falsified case 07's `gpudirect=0`, which had been declared
from a capture whose most important registrations were missing — the oracle was
only as good as the coverage behind it.

**It did not close NCCL's.** ~42 unmatched per rank against ~47 before, with only
40 dmabuf regions across 8 ranks.

**`EntityClose` gained `opener`** — the type_id of the record that should have
created the subject — because "something closed that nothing opened" is not
actionable. Job 44363299: **all 33 remaining unmatched closes are opener=100,
`ib_mem_region`.** Queue pairs are fully accounted for. The residue is a
registration path.

### What the residue is NOT

- **Not `mlx5dv_reg_dmabuf_mr`** — at least not on case 02, which runs
  `providers: ['ib']`, so that interposer is not loaded and hooking it cannot
  move this board.
- **Not queue-pair creation** — `opener` says so directly.

### Next instrument, before any hook

Our `dlsym` interposer sees every lookup, and NCCL resolves its `ibv_*` entry
points that way (`nccl src/misc/ibvsymbols.cc`). A diagnostic that logs which
**registration** symbols are resolved gives the candidate set definitively and
costs nothing.

Resolution is not invocation — that is exactly what cost case 08 a week — so
this narrows the set rather than naming the door. But going from "some
registration path" to "one of these three symbols" is worth one run, and it is
cheaper than adding a hook per guess.


## Round 3 (2026-08-06 late): NCCL uses dlvsym, and four facts do not fit

Reading the source rather than the binary, which is what `reference/` is for:
NCCL does **not** resolve ibverbs with `dlsym`. `src/misc/ibvsymbols.cc` has two
build modes; the SDK build on MN5 is the dynamic one, and it uses **`dlvsym`
with an explicit version**:

```c
#define IBVERBS_VERSION "IBVERBS_1.1"
tmp = dlvsym(handle, symbol, IBVERBS_VERSION);
LOAD_SYM_VERSION(ibvhandle, "ibv_reg_mr_iova2",  ..., "IBVERBS_1.8");
LOAD_SYM_VERSION(ibvhandle, "ibv_reg_dmabuf_mr", ..., "IBVERBS_1.12");
```

Confirmed against the library case 02 actually loads (the HPC-SDK one, NOT
/lib64/libnccl.so.2 — checking the wrong artifact cost a round): no UND `ibv_*`,
and it references both `dlsym` and `dlvsym`.

### The standing contradiction

All four of these are established, and they are inconsistent:

1. NCCL resolves `ibv_*` through `dlvsym` (source + ELF).
2. Our `dlvsym` **is** exported by `libpcomm_engine.so` and routes through
   `on_symbol_load` (readelf + the interposer body).
3. `RegisteredSymbols` **is** shared — the provider's symbol is UND and binds to
   the engine's definition, so the interposer sees the provider's registrations.
4. Job 44367535 traced **zero** `ibv_*` lookups, while that same run captured
   `ib_mem_region` on all 8 ranks and 26880 sends, so the hooks did fire.

Four hypotheses died getting here: a dlsym-hidden symbol, direct linking, a
split registry, and the wrong NCCL. Do not add a fifth from the armchair.

### The cheap test that discriminates next

`on_symbol_load` returns early when `real_symbol` is null, **before** the trace:

```cpp
if (!real_symbol) return real_symbol;   // trace never runs
```

A versioned lookup that does not match — `"IBVERBS_1.12"` for
`ibv_reg_dmabuf_mr` on a libibverbs that predates it — returns null and is
therefore invisible to the diagnostic. Moving the trace above that return costs
one line and distinguishes "never looked up" from "looked up and failed", which
is exactly the ambiguity now blocking the search.

Note the shape: this is the third time in this project a diagnostic could not
see the thing it was pointed at because of an early return placed for a
different reason.
