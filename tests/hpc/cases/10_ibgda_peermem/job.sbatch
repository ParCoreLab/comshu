#!/bin/bash
# Case 10 — ibgda_peermem: device-initiated IB with the buffers registered BY
# ADDRESS through nvidia_peermem instead of by DMABUF descriptor.
#
# Identical to case 09 in every respect but one flag. That is the whole design:
#
#            transport   dmabuf   umem binding   GPU-initiated
#   case 09  ibgda       on       by_dmabuf      yes
#   case 10  ibgda       OFF      by_addr        yes     <- 09 + binding change
#   case 11  ibdevx      OFF      by_addr        no      <- 10 + transport change
#
# This case is the only one that executes the by_addr arm of umem_referential.
# Under by_dmabuf a send-queue base of 0x0 is the legitimate first slot in the
# descriptor, so the "resolved the umem id but not the buffer" clause can never
# fire; here it can.
#
# Runs WITHOUT a scheduler as well as under one. The #SBATCH lines below are
# comments to a plain bash run, which is how suite/all_in_one.sbatch already
# treats every case; here it matters because the machine that can run IBGDA
# (du04) has no SLURM at all, while the machine with SLURM (MN5) cannot run
# IBGDA. This file has to work on both, and self-skip on the one that cannot.
#
#SBATCH --job-name=pcomm-10-ibgda-peermem
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
# BEFORE the mpirun check below — see the note in 11_ibdevx_peermem. common.sh
# purges a GPP-inherited module environment and the inherited mpirun goes with
# it, so checking for mpirun before this line tests the module environment
# rather than the host. Case 11 is where it surfaced (job 45630290); this case
# has the identical ordering and is only hidden by the peermem skip above it.
pcomm_load_runtime
CASE_DIR="$PCOMM_ROOT/tests/hpc/cases/10_ibgda_peermem"
OUT=$(pcomm_out ibgda_peermem)

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
  echo "=== 10_ibgda_peermem : SKIP ($1) ==="
  mkdir -p "$OUT.csv"
  printf '{"case":"10_ibgda_peermem","ok":true,"skipped":true,"reason":"%s","checks":0}\n' \
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
  echo "=== 10_ibgda_peermem : FAIL ($1) ==="
  mkdir -p "$OUT.csv"
  printf '{"case":"10_ibgda_peermem","ok":false,"reason":"%s","checks":0}\n' \
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

# AFTER the build block, not before it — the order case 04 documents and case
# 12 learned. The block's pcomm_load_build/pcomm_load_runtime PREPEND SDK 25.7's
# lib dir while NVSHMEM_HOME globs to the newest SDK (26.5 on MN5), so exported
# above it, 25.7 ends up first and the fresh binary gets a libnvshmem_host it
# was not linked against: "undefined symbol: nvshmem_selected_device_transport",
# rc=127 (MN5 job 45889178, the first run that had to build it). This line sat
# above the block here since the case was written, and never paid while the
# binary already existed.
#
# The workload links libnvshmem_host.so.3, which lives in the SDK rather than in
# any default search path — cases 04/06/07 all carry this line and omitting it
# here cost a run (rc=127, "cannot open shared object file"). common.sh cannot
# set it because it does not know whether a case needs NVSHMEM.
export LD_LIBRARY_PATH="$NVSHMEM_HOME/lib:${NCCL_HOME:+$NCCL_HOME/lib:}${LD_LIBRARY_PATH:-}"

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
# DMABUF is DISABLED here — the single variable that separates this case from
# case 09. Everything else is identical, deliberately: same transport, same
# workload, same rank count, so a difference between the two boards is
# attributable to the umem binding and to nothing else.
#
# With DMABUF off, IBGDA registers NIC buffers through nvidia_peermem, so the
# umem carries a REAL POINTER (binding=by_addr, umem_binding=0) instead of a
# descriptor. That is the arm of umem_referential that had never been executed:
# under by_dmabuf, sq_base is an offset and 0x0 is a legitimate first slot, so
# the "known umem but no send-queue address" clause could not fire. Here a zero
# sq_base is a genuine defect, and this case is the only thing that can catch it.
#
# This requires nvidia_peermem — which is exactly why the pre-flight below
# refuses to run without it. On MN5 this flag killed IBGDA outright for that
# reason, and the case correctly SKIPs there.
export NVSHMEM_BOOTSTRAP=MPI
export NVSHMEM_DISABLE_P2P=1
export NVSHMEM_REMOTE_TRANSPORT=ibgda
export NVSHMEM_IB_ENABLE_IBGDA=1
export NVSHMEM_IBGDA_NIC_HANDLER=gpu
export NVSHMEM_IBGDA_FORCE_NIC_BUF_MEMTYPE=gpumem
export NVSHMEM_IB_DISABLE_DMABUF=1

rm -rf "$OUT" "$OUT.csv"; mkdir -p "$OUT"

timeout 300 "$MPIRUN" -np 2 ${PCOMM_MPIRUN_EXTRA:-} \
  -x PATH -x LD_LIBRARY_PATH -x PCOMM_PROVIDER_PATH \
  -x NVSHMEM_BOOTSTRAP -x NVSHMEM_DISABLE_P2P -x NVSHMEM_REMOTE_TRANSPORT \
  -x NVSHMEM_IB_ENABLE_IBGDA -x NVSHMEM_IBGDA_NIC_HANDLER \
  -x NVSHMEM_IBGDA_FORCE_NIC_BUF_MEMTYPE -x NVSHMEM_IB_DISABLE_DMABUF \
  "$PCOMM" run -p mlx5dv -o "$OUT" -- "$APP" 1024 2 2>&1 | tee "$OUT.run.log"
rc=${PIPESTATUS[0]}; echo "mpirun exit=$rc"

# A run that IBGDA declined to serve is a skip, not a failure: NVSHMEM exits
# non-zero when the pinned transport cannot initialise, and that is the same
# fact the pre-flight checks test for, arriving later.
#
# Only the workload's own output, and only NVSHMEM's own words: grepping the
# job's .out for "peermem" matched the set -x trace of the grep itself, so every
# failure came back a SKIP (case 11 has the measurement).
if [ $rc -ne 0 ]; then
  if grep -qE "nv_peer_mem, or nvidia_peermem detected|GPU-initiated communication|init failed for transport: IBGDA" \
       "$OUT.run.log" 2>/dev/null; then
    skip "NVSHMEM declined IBGDA on this host (rc=$rc)"
  fi
  echo "FAILED: workload did not complete (rc=$rc; 124 = timed out)."
  mkdir -p "$OUT.csv"
  printf '{"case":"10_ibgda_peermem","ok":false,"checks":0,"rc":%d}\n' "$rc" > "$OUT.csv/result.json"
  exit 1
fi

echo "=== captured streams ==="; "$PCOMM" read "$OUT"
"$PCOMM" export "$OUT" --csv "$OUT.csv"
pcomm_derive "$OUT"
bash "$PCOMM_ROOT/tests/hpc/suite/sql_check.sh" "$OUT.csv" "$CASE_DIR"
