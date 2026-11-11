# nvbit_ldl_wedge — result: does not reproduce on the current stack

In August, instrumenting one spilled-register reload (`LDL.64` at `+0x19680`,
index 1006) in `nvshmemi_transfer_rma_p<int>` hung NVSHMEM's IBRC proxy, and
only when the injected call pushed to a channel (MN5 jobs 43918460..43920181,
NVSHMEM 3.7). comshu avoided it by not instrumenting LOCAL memory at all, which
is right on its own merits: per-thread stack is not communication.

This probe asked whether the hang is NVBit's or NVSHMEM's, with no comshu in the
process: a stand-alone NVBit tool, NVBit's example build flags, case 04's putget
over IBRC on one node with `NVSHMEM_DISABLE_P2P=1`.

On 2026-11-11, on MN5's current stack (NVHPC SDK 25.7, CUDA 12.9, H100):

| job      | arm                                        | instrumented | pushed   | result    |
|----------|--------------------------------------------|-------------:|---------:|-----------|
| 48932414 | bare (no NVBit)                            |            — |        — | completed |
| 48932414 | NVBit, target JIT'd, no call               |            0 |        — | completed |
| 48932414 | empty call on every LDL                    |           45 |        — | completed |
| 48932414 | global atomic on every LDL                 |           45 | 3.7 M ops| completed |
| 48932414 | NVBit stock 16 B channel push, every LDL   |           45 |   1.0 MB | completed |
| 48932414 | comshu's body (32 shfl, ~300 B), every LDL |           45 |  33.5 MB | completed ×2 |
| 48932414 | comshu's body, every LDL + every GLOBAL    |          540 |  43.0 MB | completed ×2 |

The bisect's offset does not exist in this build (job 48931785's dump lists
the 45 LDLs present), so the exact instruction cannot be re-targeted. Everything
around it was, in the original's configuration, and nothing hangs. Treat the
August hang as specific to that NVSHMEM 3.7 build unless it is seen again. Rerun
`job.sbatch` against the next NVSHMEM to check.
