#!/bin/bash
# Case 09 — ibgda_putget: the only case that captures DEVICE-INITIATED network
# traffic. Every other IB case sees the CPU's data plane (ibv_post_send /
# ibv_poll_cq); IBGDA has no CPU data plane at all — the GPU writes the
# work-queue entry and rings the NIC doorbell itself — so this is the capture
# that proves the mlx5dv/DEVX provider sees what the ibverbs provider
# structurally cannot.
#
# Runs WITHOUT a scheduler as well as under one. The #SBATCH lines below are
# comments to a plain bash run, which is how suite/all_in_one.sbatch already
# treats every case; here it matters because the machine that can run IBGDA
# (du04) has no SLURM at all, while the machine with SLURM (MN5) cannot run
# IBGDA. This file has to work on both, and self-skip on the one that cannot.
#
#SBATCH --job-name=pcomm-09-ibgda-putget
#SBATCH --account=etur02
#SBATCH --qos=acc_debug
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=2
#SBATCH --gres=gpu:2
#SBATCH --time=00:10:00
#SBATCH --output=%x.%j.out

set -x
PCOMM_ROOT="${PCOMM_ROOT:-$HOME/proj/pcomm}"
source "$PCOMM_ROOT/tests/hpc/suite/common.sh"
CASE_DIR="$PCOMM_ROOT/tests/hpc/cases/09_ibgda_putget"
OUT=$(pcomm_out ibgda_putget)

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
  echo "=== 09_ibgda_putget : SKIP ($1) ==="
  mkdir -p "$OUT.csv"
  printf '{"case":"09_ibgda_putget","ok":true,"skipped":true,"reason":"%s","checks":0}\n' \
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
  echo "=== 09_ibgda_putget : FAIL ($1) ==="
  mkdir -p "$OUT.csv"
  printf '{"case":"09_ibgda_putget","ok":false,"reason":"%s","checks":0}\n' \
    "$1" > "$OUT.csv/result.json"
  exit 1
}

[ -d /sys/class/infiniband ] || skip "no InfiniBand device on this host"
have_peermem=0
lsmod 2>/dev/null | grep -qE "nvidia_peermem|nv_peer_mem" && have_peermem=1
grep -q "PeerMappingOverride=1" /proc/driver/nvidia/params 2>/dev/null || \
  skip "NVIDIA driver has no PeerMappingOverride=1 — IBGDA cannot map the UAR to the GPU"
[ "$have_peermem" = "1" ] || skip "no nvidia_peermem/nv_peer_mem module loaded"

MPIRUN="${PCOMM_MPIRUN:-mpirun}"
command -v "$MPIRUN" >/dev/null || fail "no mpirun on PATH"

# ---- workload -------------------------------------------------------------
# Same binary as cases 04/06/07 — the ring spans whatever PE count it is given.
APP="${PCOMM_IBGDA_APP:-$HOME/nvshmem_putget_sm80}"
if [ ! -x "$APP" ]; then
  pcomm_load_build
  # Everything below is a harness problem on a host that already proved it can
  # do IBGDA, so each one fails rather than skips.
  command -v "${PCOMM_NVCC:-nvcc}" >/dev/null 2>&1 || \
    fail "no nvcc (PCOMM_SDK=${PCOMM_SDK:-unset})"
  [ -n "${NVSHMEM_HOME:-}" ] && [ -d "$NVSHMEM_HOME" ] || \
    fail "NVSHMEM_HOME did not resolve (PCOMM_SDK=${PCOMM_SDK:-unset})"
  ${PCOMM_NVCC:-nvcc} -ccbin=mpic++ -O2 -rdc=true \
    -gencode arch="${PCOMM_CUDA_ARCH:-compute_80}",code="${PCOMM_CUDA_CODE:-sm_80}" \
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
# DMABUF is left ENABLED here on purpose: that is the binding meta.json declares
# (umem_binding=1) and the one SigmaTrace's address-based discriminator cannot
# read. The peermem variant is a separate case, not a flag on this one — they
# are different experiments and conflating them cost a day.
export NVSHMEM_BOOTSTRAP=MPI
export NVSHMEM_DISABLE_P2P=1
export NVSHMEM_REMOTE_TRANSPORT=ibgda
export NVSHMEM_IB_ENABLE_IBGDA=1
export NVSHMEM_IBGDA_NIC_HANDLER=gpu
export NVSHMEM_IBGDA_FORCE_NIC_BUF_MEMTYPE=gpumem

rm -rf "$OUT" "$OUT.csv"; mkdir -p "$OUT"

timeout 300 "$MPIRUN" -np 2 ${PCOMM_MPIRUN_EXTRA:-} \
  -x PATH -x LD_LIBRARY_PATH -x PCOMM_PROVIDER_PATH \
  -x NVSHMEM_BOOTSTRAP -x NVSHMEM_DISABLE_P2P -x NVSHMEM_REMOTE_TRANSPORT \
  -x NVSHMEM_IB_ENABLE_IBGDA -x NVSHMEM_IBGDA_NIC_HANDLER \
  -x NVSHMEM_IBGDA_FORCE_NIC_BUF_MEMTYPE \
  "$PCOMM" run -p mlx5dv -o "$OUT" -- "$APP" 1024 2
rc=$?; echo "mpirun exit=$rc"

# A run that IBGDA declined to serve is a skip, not a failure: NVSHMEM exits
# non-zero when the pinned transport cannot initialise, and that is the same
# fact the pre-flight checks test for, arriving later.
if [ $rc -ne 0 ]; then
  if grep -qiE "GPU-initiated communication|peermem|init failed for transport: IBGDA" \
       "${SLURM_SUBMIT_DIR:-.}"/*"${SLURM_JOB_ID:-}"*.out 2>/dev/null; then
    skip "NVSHMEM declined IBGDA on this host (rc=$rc)"
  fi
  echo "FAILED: workload did not complete (rc=$rc; 124 = timed out)."
  mkdir -p "$OUT.csv"
  printf '{"case":"09_ibgda_putget","ok":false,"checks":0,"rc":%d}\n' "$rc" > "$OUT.csv/result.json"
  exit 1
fi

echo "=== captured streams ==="; "$PCOMM" read "$OUT"
"$PCOMM" export "$OUT" --csv "$OUT.csv"
bash "$PCOMM_ROOT/tests/hpc/suite/sql_check.sh" "$OUT.csv" "$CASE_DIR"
