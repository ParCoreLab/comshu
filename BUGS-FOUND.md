# Bugs found with pcomm

Defects in **other people's software** that pcomm found, with the evidence that
identified each one and the measurements that attributed it. Kept separate from
`plans/` on purpose: a plan records how we decided something about pcomm, while
this file records what pcomm found out about the world — which is the claim a
paper actually makes for a tool.

The standard for an entry here is that the tool found it, not that a human
suspected it and the tool agreed. Each entry says what fired, what was measured,
and — explicitly — which competing explanations were excluded and how. An entry
that cannot name what would have falsified it does not belong here.

---

## 1. HPC-X hcoll leaks two memory regions per node, above 4 nodes

**One line.** HPC-X's hcoll multicast component registers three InfiniBand
memory regions per node and deregisters one, leaving 8 MiB + 40 B of pinned
host memory per node registered for the life of every MPI process that engages
multicast.

### Environment

| | |
|---|---|
| system | MareNostrum 5, ACC partition (Intel 8460Y+, 4× H100, NDR InfiniBand) |
| component | `hmca_mcast_vmc.so` (built 2025-07-18) |
| hcoll | 4.8.3230 (`1a4e38d`) |
| HPC-X | v2.22.1, as shipped inside NVIDIA HPC SDK 25.7 |
| OFED | `OFED-internal-25.10-3.1.8` |
| workload | NVSHMEM `hub` pattern over `-p ib`, OpenMPI bootstrap |

### What fired

`entity_close_accounted`, a pcomm suite assertion stating that every entity a
capture records as opened is also recorded as closed, checked per rank. On a
64-rank capture (16 nodes × 4 ranks, job 44514759):

```
32 unclosed ib_mem_region
   = exactly 2 on each of the 16 ranks with rank % 4 == 0 (local rank 0 of a node)
   =           0 on the other 48 ranks
per node leader: one 40-byte and one 8388608-byte host region,
                 same PD, access=3 (LOCAL_WRITE|REMOTE_WRITE), origin=reg_mr
```

Nothing else in that capture was abnormal: the 126-edge communication star
resolved exactly, with zero unresolved peers and zero self-edges across 16 LIDs.

### The evidence that names the owner

pcomm's MR-caller diagnostic `dladdr`s `__builtin_return_address(0)` inside each
registration interposer. Keyed on `(door, library)` it could not answer this —
`ibv_reg_mr` has two callers in this workload — so the key was extended to
include the region length, because the leak had been identified *by* size:

```
hmca_mcast_vmc.so   reg_mr len=20971520    dereg_mr len=20971520    balanced
hmca_mcast_vmc.so   reg_mr len=8388608     (no dereg observed)      LEAKED
hmca_mcast_vmc.so   reg_mr len=40          (no dereg observed)      LEAKED
```

Three registrations, one deregistration, one library.

### Competing explanations, and why each is excluded

**"pcomm misses the deregistration door."** Excluded by the balanced row. Every
`ibv_dereg_mr` this library makes is visible to the same interposer, including
the one for the 20 MiB region. Its frees are not escaping through an unhooked
path; there are no frees for the other two sizes. `ibv_dereg_mr` is also the
only deregistration entry point in the verbs API, so there is no second door to
miss.

**"It frees them after pcomm stops recording."** Excluded because the caller
trace prints from the interposer itself, independent of capture state. A free
during teardown — after finalize, after the last flush — would still have
printed its line. None exists.

**"It is a property of our configuration."** Excluded by measurement.
`NVSHMEM_DISABLE_P2P` was the original hypothesis and was measured at every
scale below the threshold, in both settings, in the same allocation: it never
mattered. The variable is node count alone.

**"The application leaks it."** The application is 60 lines of NVSHMEM and never
calls `ibv_reg_mr`. The registering library is named above.

### Scale threshold

| shape | job | leak |
|---|---|---|
| 4 nodes × 1 rank | suite 16/17/18 | none |
| 2 nodes × 4 ranks | 44516867 | none |
| 4 nodes × 4 ranks | 44517375 | none |
| 8 nodes × 4 ranks | 44517372 | 2 per node leader |
| 16 nodes × 4 ranks | 44514759 | 2 per node leader |

The threshold is between 4 and 8 nodes, consistent with hcoll enabling its
multicast component by node count. Below it the component does not run and
nothing leaks.

### Impact

Bounded but real. The leaked memory is **registered** — pinned, DMA-mapped, and
charged against a limited per-HCA resource rather than ordinary heap. It is
reclaimed at process exit, so this is not an unbounded growth bug within a run;
the cost is 8 MiB + 40 B held per node for the life of every process that
engages multicast. On the 16-node capture that is 128 MiB of pinned host memory
across the job.

The more interesting property for a paper is that it is **invisible without
per-entity lifetime accounting**. Aggregate counters do not show it: the process
exits cleanly, the collective completes, no error is reported anywhere, and
total registered bytes look unremarkable next to NVSHMEM's own. It only appears
when open and close are matched **per handle, per rank** — which is what the
assertion does.

### Reproduction

```bash
sbatch --nodes=8 tests/hpc/probes/leaderdereg/job.sbatch
```

The probe runs two sections in one allocation (with and without
`NVSHMEM_DISABLE_P2P`), reports unclosed regions per rank, and prints the
per-library, per-size caller table. It is parametric in `--nodes` so the
threshold can be re-measured when HPC-X changes.

### How the tool found it

Worth stating precisely, because the sequence is the argument:

1. **Detection was automatic and uninformed.** No one suspected hcoll. A generic
   invariant — every opened entity is closed — fired on a capture taken for an
   unrelated reason (a scale test for the UI). The assertion knows nothing about
   hcoll, multicast, or memory registration.
2. **The signature carried information.** "2 per node leader, 0 elsewhere,
   identical sizes" is not a shape a race produces. That regularity is what made
   it worth four allocations instead of a retry.
3. **Attribution needed one diagnostic change**, not a redesign: adding region
   length to an existing caller trace. Total cost, detection to named library
   and excluded alternatives: four jobs, about 15 minutes of compute.
4. **Two of those four jobs falsified the investigator, not the bug.** The first
   killed the hypothesis the probe was built on (`DISABLE_P2P`) and exposed that
   the probe had its rank count hardcoded while being the tool meant to vary it.

### Status

Not reported upstream yet. The conclusion rests on observed call behaviour —
hcoll is closed source and its source was not read — which is worth stating in
any report: what is demonstrated is that the library registers three regions and
deregisters one, not what its code intends.

Recorded in pcomm as `expect_fail_asserts` on the single capture that crosses
the threshold, with the measurement attached, so the suite neither goes red for
a third-party defect nor hides it. `sql_check` reports **XPASS** if it ever
starts passing, which is the signal that HPC-X fixed it.

**Full investigation:** `plans/PLAN-20260812-0250-hcoll-mcast-leaks-two-mrs-done.md`

---

## 2. llm.c reduces 4 floats where it means 1, and overflows a 4-byte managed buffer

**One line.** Four call sites in `llm.c` pass `sizeof(float)` as the *element
count* argument of `ncclAllReduce`, so every scalar reduction moves 4 floats
instead of 1 — and one of them writes 16 bytes into a 4-byte
`cudaMallocManaged` allocation, a 12-byte heap overflow on every multi-GPU
barrier.

| | |
|---|---|
| Environment | llm.c at upstream `f1e2ace`; NCCL 2.29.3; verified on du04 (4x A100, CUDA 13.0, driver 580.95.05) and MN5 (8 ranks, 2 nodes, 4x H100) |
| What fired | `tests/hpc/apps/04_llmc/asserts/scalar_reduction_count_is_sizeof_float.sql`, over `nccl_collective` from job 45082117 and the du04 run |
| Evidence | `ncclAllReduce`'s third parameter is `size_t count`, a COUNT OF ELEMENTS, with the datatype given separately as the fourth. `sizeof(float)` is 4. The capture records `count = 4` with `datatype = ncclFloat` at all four sites, where the surrounding code reduces one scalar. The allocation is `cudaMallocManaged(&result.unified_buffer, sizeof(float))` — 4 bytes — at `llmc/zero.cuh:455`; `multi_gpu_barrier` (`zero.cuh:483`) reduces into it with `count = 4`, i.e. 16 bytes. |
| Sites | `llmc/zero.cuh:483` (barrier, the overflowing one), `llmc/zero.cuh:588`, `train_gpt2.cu:958` (`accumulated_mean_loss`), `train_gpt2.cu:1022` (`grad_norm_squared`, stage-1 only) |
| Excluded | *Not* a capture artefact: the number is `4` in the capture because `4` is what the source passes, and the assertion derives its expectation from `sizeof(float)` in the C language rather than from the capture. *Not* a datatype confusion in pcomm's decode: `datatype` decodes as `ncclFloat` while `count` decodes as `4`, and the two are separate fields recorded from separate arguments. *Not* deliberate padding: the allocation is `sizeof(float)`, so the code's own two uses of the constant contradict each other. |
| Impact | Honest limits: the three non-buffer sites reduce a scalar redundantly and are *harmless in result* — a sum or average over 4 copies of correlated garbage is still written back to a slot the code then reads as one float, and the extra 12 bytes read are within the surrounding allocation for those three. The barrier site is the real defect: the 4-byte managed allocation has 12 bytes written past its end, every barrier, on every rank. Whether it corrupts anything depends on what the CUDA managed allocator placed after it, which is why this is a latent memory-safety bug rather than an observed wrong answer. Bare and captured runs print identical losses to six decimals, so it is not currently producing visible numerical damage. |
| Reproduction | `tests/hpc/apps/04_llmc/run.sh` on du04, or `job.sbatch` on MN5; the assertion is `asserts/scalar_reduction_count_is_sizeof_float.sql` |
| How found | Not by suspicion. The `nccl` provider records the arguments the application passed, so `count` was in the capture as an ordinary column; the study was predicting collective counts and volumes from `llmc/zero.cuh`, and the scalar reductions were the one place where predicted and recorded element counts could not be reconciled by any reading of the source. The allocation size was then read to see what the count implied. |
| Status | Not reported upstream. The claim covers `f1e2ace` only; it was not checked against later commits, and no fix was attempted. |

**Full investigation:** `tests/hpc/apps/04_llmc/REPORT.md`

## Template for the next entry

```
## N. <one-line claim: component, defect, condition>

Environment | table of exact versions
What fired  | which pcomm check, on which capture, with the numbers
Evidence    | the measurement that names the owner
Excluded    | each competing explanation and what rules it out
Threshold   | where it starts and stops, with job ids
Impact      | honest, including what it is NOT
Reproduction| one command
How found   | the sequence, including what falsified the investigator
Status      | reported / not, and what the claim does and does not cover
```
