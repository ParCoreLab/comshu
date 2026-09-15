# SigmaTrace parity on ibdevx — case 11

The comparison `meta.json: comparison_note` said this case exists for. Method is
`plans/PLAN-20260810-0102-sigmatrace-parity-done.md` steps 1, 2 and 4, applied to
the control plane instead of the doorbell stream, because under ibdevx there is
no doorbell stream either tool can see (below).

- `sigma_fingerprint.cpp` — SigmaTrace's verbs + mlx5dv events as `class<TAB>key`
  per rank, timing-free. Case 12's `sigma_dump.cpp` covers mlx5dv only.
- `parity.py` — projects `pcomm export --csv` output onto the same classes and
  keys, checks each tool against itself, then prints the coverage matrix.

## Result — 2026-09-14, du04

Two runs of each tool, separately, one after the other on the same node: same `common.sh`, same
`mpirun` (Open MPI 4.1.4), same binary (`~/nvshmem_putget_sm80 1024 2`), same
four NVSHMEM variables as `job.sbatch`. pcomm at `7689461` through
`run_on_a100.sh --cases 11` (both runs PASS, 18 assertions); SigmaTrace with
`--enable-doorbell-hijack --load-cupti=0`, the build from 2026-08-10 (du04's
copy has no `.git`, so its revision is not recorded). A run of the same workload
with no tool at all prints the same Open MPI `mca_pml_ucx` warning both tools'
runs print, so the program under observation was the same in all five.

**Both tools are self-repeatable**: pcomm's two fingerprints are identical, and
so are SigmaTrace's. Every difference below is therefore between the tools.

| class | SigmaTrace | pcomm | reading |
|---|---:|---:|---|
| `device_open` | 4 | 4 | agree |
| `device_close` | 2 | 2 | agree |
| `mr_reg` | 4 | 4 | agree (512 MiB + 8 B per rank) |
| `umem_reg` / `umem_dereg` | 20 / 20 | 20 / 20 | agree (sizes) |
| `uar_alloc` / `uar_free` | 10 / 10 | 10 / 10 | agree (flags) |
| `qp_connect` | 10 | 10 | agree (dest LID, port) |
| `mr_dereg` | **0** | 4 | **SigmaTrace never records it** |
| `port` | 2 | 4 | what each counts: SigmaTrace per enumerated device, pcomm per opened context |
| `device_enum` | 2 | — | no pcomm counterpart |
| `devx_qp_create`, `devx_cmd` | — | 10, 40 | no SigmaTrace event |
| `cq_*`, `srq_*` | — | 4+4, 2+2 | no SigmaTrace event |
| `mr_on_gpu` | — | 2 | no SigmaTrace field: the 512 MiB MR is on device 0 / device 1 |
| `object_create` / `object_free` | — | 28 / 4 | pcomm's object registry |
| any data-plane class | 0 | 0 | **neither tool sees it** |

**Where both tools record a class, they agree on every event, except one.**
`mr_dereg` is a SigmaTrace gap, read from its source rather than inferred:
`Ibtrace/IbverbsHooks.hpp:37` interposes `ibv_dereg_mr`, and nothing under
`Profiler/` ever records `IbvMemoryDereg` — the event type is declared and its
reader visits it, but it is never written. A registration therefore never ends
in a SigmaTrace capture.

**What pcomm records that SigmaTrace has no vocabulary for** is most of what
makes ibdevx legible: the DEVX queue pairs themselves (SigmaTrace keeps them in
an internal map and emits only their connections), the DEVX command stream, the
CQs and SRQ, and which GPU the NVSHMEM heap's memory region lives on.

**Update, 2026-09-15 — pcomm now captures it.** `pcomm run --watch-send-queues`
reads each DEVX queue's doorbell record and send queue from outside the path,
and case 11 runs with it and asserts every put arrived (2,048 × 4 B per rank, on
du04 and MN5), fully decoded, with nothing lapped or dropped. SigmaTrace still sees
none of it. Design and measurements:
`plans/PLAN-20260914-1750-the-ibdevx-data-plane-is-a-host-send-queue-done.md`.
The result below is kept as measured on 2026-09-14.

**Neither tool captured one byte of the data plane.** The put-ring moves
1024 × 4 B × 2 iterations × 2 PEs = 16,384 B through the HCA; SigmaTrace reports
`doorbell_submitted: 0`, and pcomm has no `mlx5_doorbell`, `mlx5_device_send` or
`ib_post_send` row. Under ibdevx the CPU proxy writes WQEs into DEVX queues and
rings a UAR directly: SigmaTrace's hijack only sees UARs a GPU maps
(`Profiler/Handlers/CUDAHijack.cpp`), and pcomm's host doorbell arms only non-cached UARs
(`providers/mlx5dv/host_doorbell.hpp`, `is_ibgda_uar`: `NC` or `NC_DEDICATED`).
ibdevx's are `flags=0x0`, BlueFlame — the kind UCX's RC transport rings, which
the gate leaves alone on purpose because a repeater forwarding eight bytes
truncates a BlueFlame write and hangs MPI. Watching ibdevx's doorbells therefore
needs a way to tell its BlueFlame UARs from UCX's, not a looser gate. This is the configuration
`job.sbatch` calls SigmaTrace's published one, and on this host it is
control-plane only for both tools.

## Reproducing it

du04 only; reach it with `ssh -J mktips@100.64.0.8 missa18@172.20.31.90`.

```bash
# pcomm, twice, from a clean checkout on the workstation:
PCOMM_A100=missa18@172.20.31.90 PCOMM_SSH_OPTS='-o ProxyJump=mktips@100.64.0.8' \
  tests/hpc/run_on_a100.sh --cases 11
# then copy /DATA/missa18/pcomm/results/ibdevx_peermem-local.csv aside before
# the next run: the runner's rsync --delete clears results/.

# SigmaTrace, twice: case 11's environment, one Profiler per rank, each into
# its own directory, because its event files carry a pid, not a rank.
source /DATA/missa18/pcomm/tests/hpc/suite/common.sh; pcomm_load_runtime
export LD_LIBRARY_PATH="$NVSHMEM_HOME/lib:$LD_LIBRARY_PATH"
export NVSHMEM_BOOTSTRAP=MPI NVSHMEM_DISABLE_P2P=1 \
       NVSHMEM_REMOTE_TRANSPORT=ibdevx NVSHMEM_IB_DISABLE_DMABUF=1
mpirun -np 2 -x PATH -x LD_LIBRARY_PATH -x NVSHMEM_BOOTSTRAP -x NVSHMEM_DISABLE_P2P \
  -x NVSHMEM_REMOTE_TRANSPORT -x NVSHMEM_IB_DISABLE_DMABUF \
  bash -c 'exec ~/SigmaTrace/build/target/Profiler --enable-doorbell-hijack \
    --load-cupti=0 -f -o sigma1/rank-$OMPI_COMM_WORLD_RANK ~/nvshmem_putget_sm80 1024 2'

# Fingerprint and compare (build line is in sigma_fingerprint.cpp's header):
for r in sigma1/rank-*/; do ./sigma_fingerprint "${r%/}" > "${r%/}.tsv"; done
python3 parity.py --pcomm pcomm1.csv --pcomm pcomm2.csv --sigma sigma1 --sigma sigma2
```

No `--` between Profiler's options and the program — with it the application
silently receives different arguments (case 12's README has the measurement).

## Two things that looked like disagreements and were not

- **The device GUID.** SigmaTrace stores `node_guid` as verbs returns it, a
  `__be64`; pcomm stores host order. `0x6a9fd80003f6ceb8` is
  `0xb8cef60300d89f6a` byte-reversed. The dumper converts it.
- **Four pcomm closes of opener 520.** That is `MemObject`, the engine's object
  registry — pcomm's own `entity_close_accounted` had already matched them;
  the harness simply had not read `mem_object`.
