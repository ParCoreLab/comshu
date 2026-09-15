#!/bin/bash
# Case 11 — ibdevx_peermem: the DEVX control plane, and the CPU-driven data
# plane under it — which no door saw until the send-queue watcher (below).
#
# This is SigmaTrace's published artifact configuration, and it is the third
# corner of a deliberate 2x2. Cases 09, 10 and 11 differ from each other in
# exactly one variable at a time:
#
#            transport   dmabuf   umem binding   GPU-initiated
#   case 09  ibgda       on       by_dmabuf      yes
#   case 10  ibgda       OFF      by_addr        yes     <- 09 + binding change
#   case 11  ibdevx      OFF      by_addr        no      <- 10 + transport change
#
# Read together they separate what the provider decodes from who drives the
# hardware. Case 11's job is to show that DEVX decoding, the umem registry and
# topology closure hold when the doorbell is rung by the CPU — which cases 09
# and 10 cannot show, because there the two always move together.
#
# Runs WITHOUT a scheduler as well as under one. The #SBATCH lines below are
# comments to a plain bash run, which is how suite/all_in_one.sbatch already
# treats every case; here it matters because the machine that can run this
# (du04) has no SLURM at all, while the machine with SLURM (MN5) lacks the
# peermem module. This file has to work on both, and self-skip on the one that
# cannot.
#
#SBATCH --job-name=pcomm-11-ibdevx-peermem
#SBATCH --account=etur02
#SBATCH --qos=acc_debug
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=2
#SBATCH --cpus-per-task=20
#SBATCH --exclusive
#SBATCH --gres=gpu:2
#SBATCH --time=00:10:00
#SBATCH --output=%x.%j.out

set -x
PCOMM_ROOT="${PCOMM_ROOT:-$HOME/proj/pcomm}"
source "$PCOMM_ROOT/tests/hpc/suite/common.sh"
# BEFORE the mpirun check below, not after it, and this ordering has cost a red
# board (job 45630290). common.sh purges the module environment when an ACC job
# inherits a GPP one — which is every job since the default login node moved to
# glogin1 — and that purge takes the inherited Intel MPI with it. Measured on
# as01r1b32 (job 45636242):
#
#   before common.sh    mpirun /gpfs/apps/MN5/GPP/ONEAPI/.../bin/mpirun
#   after  common.sh    <none>
#   after  this line    mpirun /apps/ACC/NVIDIA-HPC-SDK/25.7/.../mpi/bin/mpirun
#
# So `command -v mpirun` in that window tests the module environment, not the
# host, and reports FAIL for a cluster that runs MPI perfectly well. Cases
# 04/06/07 call this first and have never had the problem.
pcomm_load_runtime
CASE_DIR="$PCOMM_ROOT/tests/hpc/cases/11_ibdevx_peermem"
OUT=$(pcomm_out ibdevx_peermem)

# The workload links libnvshmem_host.so.3, which lives in the SDK rather than in
# any default search path — cases 04/06/07 all carry this line and omitting it
# here cost a run (rc=127, "cannot open shared object file"). common.sh cannot
# set it because it does not know whether a case needs NVSHMEM.
export LD_LIBRARY_PATH="$NVSHMEM_HOME/lib:${NCCL_HOME:+$NCCL_HOME/lib:}${LD_LIBRARY_PATH:-}"

# ---- prerequisites --------------------------------------------------------
# IBGDA needs the NIC to be allowed to expose its doorbell page to the GPU. That
# takes either the nvidia_peermem/nv_peer_mem kernel module or DMABUF support,
# AND the NVIDIA driver's PeerMappingOverride=1. MN5's ACC partition has
# neither: NVSHMEM reports "no active IB device that supports GPU-initiated
# communication is found, exiting" (jobs 44156479, 44156594).
#
# A host that cannot do IBGDA must SKIP, not fail. A red board there would be
# reporting a fact about the cluster as if it were a defect in the tool — and
# would train everyone to ignore the case.
# SKIP means one thing only: this host cannot do IBGDA, so the case has nothing
# to say. It is NOT a place to put "something went wrong".
skip() {
  echo "=== 11_ibdevx_peermem : SKIP ($1) ==="
  mkdir -p "$OUT.csv"
  printf '{"case":"11_ibdevx_peermem","ok":true,"skipped":true,"reason":"%s","checks":0}\n' \
    "$1" > "$OUT.csv/result.json"
  exit 0
}

# FAIL is for a host that CAN do IBGDA where the harness broke — a missing
# compiler, an SDK that did not resolve, a workload that would not build. That is
# not a skip, and calling it one is how the first run of this case reported
# ok:true having tested nothing: peermem, PeerMappingOverride and mpirun all
# passed, then nvcc was missing because common.sh had MareNostrum's paths
# hardcoded, and skip() swallowed it. A green board that built nothing is the
# precise failure this suite exists to stop.
fail() {
  echo "=== 11_ibdevx_peermem : FAIL ($1) ==="
  mkdir -p "$OUT.csv"
  printf '{"case":"11_ibdevx_peermem","ok":false,"reason":"%s","checks":0}\n' \
    "$1" > "$OUT.csv/result.json"
  exit 1
}

[ -d /sys/class/infiniband ] || skip "no InfiniBand device on this host"
# Only peermem is required here, and the difference from cases 09/10 is
# deliberate. PeerMappingOverride=1 exists to let the NIC's UAR doorbell page be
# mapped into the GPU's address space — that is what GPU-initiated transports
# need, and ibdevx rings the doorbell from the CPU. Registering GPU buffers with
# the NIC by address still needs nvidia_peermem, so that check stays.
#
# Not requiring PeerMappingOverride is the louder choice on purpose: if it turns
# out to be needed after all, this case FAILS on a host that has peermem without
# it, instead of skipping and hiding the fact. A skip condition that is too broad
# makes a case invisible exactly where it would have been informative.
lsmod 2>/dev/null | grep -qE "nvidia_peermem|nv_peer_mem" || \
  skip "no nvidia_peermem/nv_peer_mem module loaded — by_addr registration of GPU memory needs it"
# NO PeerMappingOverride GATE HERE, and the absence is now measured rather than
# assumed. A gate was added on 2026-09-10 and removed on 2026-09-11: MN5 lacks
# the flag and failed umem_binding_as_configured, which looked like the answer
# to the question the paragraph above poses. It was not. du04 HAS
# PeerMappingOverride=1 and failed that assertion identically, so the driver
# flag was never the discriminator — the assertion was reading the wrong table.
# See meta.json prerequisite_answer; the narrow prerequisite this case was built
# with stands, and stands for the reason it was written.

MPIRUN="${PCOMM_MPIRUN:-mpirun}"
command -v "$MPIRUN" >/dev/null || fail "no mpirun on PATH"

# ---- workload -------------------------------------------------------------
# Same binary as cases 04/06/07 — the ring spans whatever PE count it is given.
APP="${PCOMM_IBGDA_APP:-$HOME/nvshmem_putget-$(sha1sum "$PCOMM_ROOT/tests/hpc/cases/04_nvshmem/putget.cu" | cut -c1-10)}"
if [ ! -x "$APP" ]; then
  pcomm_load_build
  # Everything below is a harness problem on a host that already proved it can
  # do IBGDA, so each one fails rather than skips.
  command -v "${PCOMM_NVCC:-nvcc}" >/dev/null 2>&1 || \
    fail "no nvcc (PCOMM_SDK=${PCOMM_SDK:-unset})"
  [ -n "${NVSHMEM_HOME:-}" ] && [ -d "$NVSHMEM_HOME" ] || \
    fail "NVSHMEM_HOME did not resolve (PCOMM_SDK=${PCOMM_SDK:-unset})"
  ${PCOMM_NVCC:-nvcc} -ccbin=mpic++ -O2 -rdc=true \
    -gencode arch=compute_80,code=sm_80 -gencode arch=compute_90,code=sm_90 \
    -Wno-deprecated-gpu-targets \
    "$PCOMM_ROOT/tests/hpc/cases/04_nvshmem/putget.cu" -o "$APP" \
    -I"$NVSHMEM_HOME/include" -L"$NVSHMEM_HOME/lib" \
    -lnvshmem_host -lnvshmem_device -lcudart || fail "workload did not build"
  pcomm_load_runtime
fi

# ---- force GPU-initiated IB ----------------------------------------------
# The exact inverse of case 07, which pins IBGDA *off* because "the IB half of
# this capture would silently come back empty rather than fail".
#
# NVSHMEM_DISABLE_P2P=1 is what makes a single node meaningful: without it two
# PEs on one node talk over NVLink and the NIC is never touched. With it the
# remote transport is used in loopback through the HCA, which is real
# device-initiated traffic — it is not enough for a bandwidth claim, and this
# case makes none.
#
# TRANSPORT is the single variable that separates this case from case 10:
# ibdevx instead of ibgda, DMABUF disabled in both. Case 10 changes the umem
# binding and holds the transport; this one changes the transport and holds the
# binding. Neither changes two things, which is what makes either board
# readable.
#
# ibdevx is SigmaTrace's published artifact configuration, and matching it is
# the point of this case: it is the one setup where that tool's results and
# pcomm's can be compared directly.
#
# Note what this case does NOT claim. ibdevx drives the DEVX control plane from
# the CPU, so there is no GPU-initiated data plane here and ibgda_engaged is
# deliberately absent rather than expected-to-fail — asserting device-initiated
# traffic in a CPU-driven transport would be asserting a bug. What this case
# does prove is that the DEVX decoding, the umem registry and the topology
# closure hold independently of who rings the doorbell, which is the part case
# 09 cannot separate — AND, since 2026-09-15, that the CPU-driven data plane is
# captured whole: every put the workload makes, decoded, with nothing lapped or
# dropped (asserts/data_plane_*.sql).
export NVSHMEM_BOOTSTRAP=MPI
export NVSHMEM_DISABLE_P2P=1
export NVSHMEM_REMOTE_TRANSPORT=ibdevx
export NVSHMEM_IB_DISABLE_DMABUF=1

# `-p ib,mlx5dv`, not `-p mlx5dv`, and the second provider is not decoration.
#
# ibdevx drives the control plane through DEVX but NVSHMEM still creates its
# queue pairs with plain verbs. ibv_destroy_qp is interposed by the ENGINE, so
# the closes are always recorded; ibv_create_qp is interposed by the IBVERBS
# PROVIDER, so on a mlx5dv-only run the opens are not. Every queue pair was
# therefore a "close names an entity no open record created" — 99 per rank,
# which is what entity_close_accounted reported the first time this case ever
# reached its assertions (job 45636332). Measured directly, same workload, same
# node, job 45636769:
#
#   -p mlx5dv      ib_queue_pair   0 rows   99 orphaned closes per rank
#   -p ib,mlx5dv   ib_queue_pair 198 rows    0 orphaned closes
#
# That assertion's whole point is that an unmatched close means a creation door
# nobody hooked, and that the fix is to capture the door rather than filter the
# row. Here the door exists and simply was not loaded. The providers co-run
# (case 06 is the regression test) and `ib` does not perturb what the 200-range
# streams record — mlx5_umem, mlx5_uar and mlx5_devx_* are identical across the
# two arms above.
#
# `--watch-send-queues`, because without it this case had no data plane at all.
# ibdevx never calls ibv_post_send and rings a BlueFlame UAR that neither
# doorbell path watches, so until 2026-09-15 pcomm — and SigmaTrace, measured
# side by side in parity/ — recorded not one byte of the 16,384 this workload
# moves. The watcher reads each DEVX queue's doorbell record and send queue from
# outside the path (providers/mlx5dv/sq_watch.hpp): it touches no doorbell, so it
# cannot break the run, and what it misses is counted in mlx5_send_coverage
# rather than absent. PLAN-20260914-1750 has the design and the measurements.
# Both hosts run it: RC queue pairs on du04, DC initiators on MN5.
rm -rf "$OUT" "$OUT.csv"; mkdir -p "$OUT"

timeout 300 "$MPIRUN" -np 2 ${PCOMM_MPIRUN_EXTRA:-} \
  -x PATH -x LD_LIBRARY_PATH -x PCOMM_PROVIDER_PATH \
  -x NVSHMEM_BOOTSTRAP -x NVSHMEM_DISABLE_P2P -x NVSHMEM_REMOTE_TRANSPORT \
  -x NVSHMEM_IB_DISABLE_DMABUF \
  "$PCOMM" run -p ib,mlx5dv --watch-send-queues -o "$OUT" -- "$APP" 1024 2 2>&1 | tee "$OUT.run.log"
rc=${PIPESTATUS[0]}; echo "mpirun exit=$rc"

# A run the transport declined to serve is a skip, not a failure: NVSHMEM exits
# non-zero when the pinned transport cannot initialise, and that is the same
# fact the pre-flight checks test for, arriving later.
#
# Only the workload's own output is searched, and only for NVSHMEM's own words.
# This used to grep the job's .out for "peermem", which set -x had already
# written the grep's own pattern into — so every failure matched itself and
# came back a SKIP (MN5, job 45887836: mpirun could not even bind a process).
# On the combined board the same glob named the whole board's .out.
if [ $rc -ne 0 ]; then
  if grep -qE "nv_peer_mem, or nvidia_peermem detected|GPU-initiated communication|init failed for transport: (IBGDA|IBDEVX)" \
       "$OUT.run.log" 2>/dev/null; then
    skip "NVSHMEM declined the ibdevx transport on this host (rc=$rc)"
  fi
  echo "FAILED: workload did not complete (rc=$rc; 124 = timed out)."
  mkdir -p "$OUT.csv"
  printf '{"case":"11_ibdevx_peermem","ok":false,"checks":0,"rc":%d}\n' "$rc" > "$OUT.csv/result.json"
  exit 1
fi

echo "=== captured streams ==="; "$PCOMM" read "$OUT"
"$PCOMM" export "$OUT" --csv "$OUT.csv"
pcomm_derive "$OUT"
bash "$PCOMM_ROOT/tests/hpc/suite/sql_check.sh" "$OUT.csv" "$CASE_DIR"
