# A peer access lands in the owner's buffer, at its true offset — done

- **Commits:** 87e2ea9 (engine), cc141f1 (gpumem + reader). Board: MN5 49224860,
  SUITE GREEN (09/10 SKIP, no PeerMappingOverride).

## What was wrong

1. **Imported mappings recorded as the mapper's objects.** The cuMemMap door
   (b817153) made every VMM mapping an object of the mapping process, including
   NCCL's and NVSHMEM's mappings of each PEER's buffer (imported handles). Rank 0
   then held objects on GPUs 1 and 3; `_device_owner` gave GPU 1 to rank 0; and
   nccl_p2p placed 589,935 peer writes in the writer's own "buffer" (boards
   49207744 through 49220686) while every check stayed green. NVSHMEM captures
   carried the same contamination (peers' heaps on 04/06/07/13).
2. **p2p offsets estimated.** gpu_access recorded only the address the GPU
   issued. Through an NVSHMEM peer mapping that lies far from the buffer's
   recorded extent, so the reader anchored on the lowest address touched —
   wrong whenever the buffer's start went untouched. The attributor had the
   exact translation at capture and dropped it.

## Rules now

- An object is memory a process ALLOCATED. cuMemMap records one only for a
  handle this process created (cuMemCreate); an imported handle's mapping is a
  window, not an object. A handle never seen being made records nothing.
- A GPU access carries `owner_address`: the byte where its owner recorded the
  object, set by the attributor that found it (UVA: the address; NVSHMEM: the
  symmetric address, `peer_base + (dest - heap_base)` undone). Offset =
  owner_address - base, exact.
- What the capture cannot name stays unplaced and counted
  (`link_health.unattributed_p2p_accesses`), never guessed.

## Verified (49224860)

| case | placed | exact | self-owned | offsets | unplaced |
|---|---|---|---|---|---|
| 03 p2p_kernel (1 process, 4 GPUs) | 65,536 | all | all (correct) | 0–16,380 / 16 KiB | 0 |
| 04, 06 NVSHMEM | 786,432 | all | 0 | 0–262,140 / 256 KiB | 0 |
| 07 NVSHMEM multinode | 1,179,648 | all | 0 | 0–262,140 | 0 |
| 13 hubspoke | 41,472 | all | 0 | 0–65,532 / 64 KiB | 0 |
| 05 nccl_p2p | 0 | — | 0 | — | 1,179,948 |

New guards: suite assertion `objects_live_where_their_rank_runs` (fails 05 and
all NVSHMEM cases on 49220686, passes everywhere on 49224860); case 27 maps an
exported-and-imported chunk and must still count two CuMemMap objects;
`tests/object_access_rule.sh` carries owner_address (true offsets 256/512 where
the old anchor said 0/256).

## Open

- **NCCL over NVLink is unplaced.** Placing it needs the importer's mapping tied
  to the exporter's allocation. Fabric handles are globally meaningful
  (PeerMapping already records them); NCCL on MN5 uses POSIX fds, which are not.
  Research item, not a reader heuristic.
- gpu_access is stamped at drain, so a reused address's lifetime cannot be
  chosen for these rows (`life` NULL when the extent had several).
