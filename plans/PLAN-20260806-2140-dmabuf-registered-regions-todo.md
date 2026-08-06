# PLAN — dmabuf-registered memory regions are invisible to the ibverbs provider

- **Created:** 2026-08-06 21:40 +03
- **Status:** TODO — cause identified from source and confirmed by the failure
  pattern; the fix has one real design decision and was not rushed.
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
