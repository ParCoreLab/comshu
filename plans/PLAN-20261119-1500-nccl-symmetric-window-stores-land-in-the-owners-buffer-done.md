# NCCL symmetric-window stores land in the owner's buffer

Status: **DONE** — MN5 jobs 49231339 (case 28) and 49231621 (case 27), both PASS.

## Question

NCCL reaches a peer GPU two ways. The classic transport (case 05) exchanges each
connection's buffer by fd, and no identity survives that exchange
(PLAN-20260817-0705, probe `ipc_identity`), so those accesses stay unplaced.
Symmetric windows (NCCL 2.27+, what GIN and the device API use) are the NVSHMEM
shape: `ncclGetLsaPointer(w, off, r) = lsaFlatBase + r*stride + off`. Does comshu
place stores through them in the owner's buffer?

## Before

Case 28 (`lsa_ring`: 4 ranks, one window, a 4-byte store ring through
`ncclGetLsaPointer`, gpumem only), MN5 49229603: capture complete (327,680 stores
per rank, each to the right neighbour) and **0 of 1,310,720 placed**. The
writer's address is in its own mapping of an imported handle, so it names no
object. Also seen there:

- every rank's window was recorded as TWO objects: ncclMemAlloc's mapping and
  NCCL's second mapping of the same handle at the rank's own flat-range slot;
- every rank reserved its flat range at the same VA (0x45a0000000, stride
  64 GiB). NCCL does not promise this, so nothing below relies on it;
- the placement assertion said `[skip]`, not `[XX]`: an empty required table
  skips. Fixed (9c000ab) by routing it through `link_health`.

## What changed

1. **A second mapping of our own memory is an alias, not an object** (f2db7c4,
   8c01b47). The first live mapping of a created handle is the object; later
   ones are tallied. Case 27 gains an alias route that must not be an object.
2. **`nccl_window_peer` (type 918)** (0c73efb). At `ncclCommWindowRegister` the
   nccl provider asks NCCL's PUBLIC `ncclGetPeerDevicePointer` (2.29+, resolved
   by dlsym) where each peer's copy is mapped here — the counterpart of
   `nvshmem_ptr`. No NCCL struct layout is read; on NCCL < 2.29 there are no
   rows and the stores stay unplaced.
3. **Reader: `_nccl_window_place` + `object_access.p2p_window`.** The owner's
   window is found by facts every rank agrees on: `comm_hash`, the owner's
   `nccl_rank`, and the window's registration order on that communicator —
   never by address. Refused (counted in `link_health.unattributed_p2p_accesses`):
   split communicators (hash 0), duplicate hashes on a rank, overlapping peer
   ranges, offsets past the window, and an owner that disagrees with the
   device's owner. `tests/object_access_rule.sh` carries a fixture where comm
   ranks differ from capture ranks and the owner has two windows; dropping
   either key turns it red (checked by mutation).

Case 28 now runs `-p p2p,nccl`: the stores come from gpumem, the windows from nccl.

## After (MN5 49231339)

| actor → owner | bytes placed | offsets | exact |
|---|---:|---|---|
| 0 → 1 | 1,310,720 | [0, 131072) | all |
| 1 → 2 | 1,310,720 | [0, 131072) | all |
| 2 → 3 | 1,310,720 | [0, 131072) | all |
| 3 → 0 | 1,310,720 | [0, 131072) | all |

`unattributed_p2p_accesses` = 0; 3 `nccl_window_peer` rows per rank; CuMemMap
objects per rank 101 → 100 (the self-slot alias gone).

## Open

- Non-blocking communicators: `ncclInProgress` from `ncclCommWindowRegister`
  means the window is not ready; see the CR-25 note in
  PLAN-20261119-1242-codebase-correctness-review-backlog.md.
- du04 ships NCCL 2.28.9, which has no `ncclGetPeerDevicePointer`: its window
  stores stay unplaced (counted).
- Classic transport (case 05): still unplaced, deferred.
- ~100 (NCCL) / ~780 (NVSHMEM) 2 MiB `CuMemMap` objects per rank: allocator
  backing, not investigated.
