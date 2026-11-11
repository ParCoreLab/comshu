#!/bin/bash
# The same CQE probe, on the OTHER NIC generation.
#
#   tests/hpc/probes/cqeread/du04.sh
#
# WHY A SECOND HOST AT ALL. The probe reads `struct mlx5_cqe64` out of the
# driver's ring by offset. MN5 answered the question — 14,957 CQEs, every one
# agreeing with its ibv_wc — but MN5 is one firmware and one rdma-core build,
# and a layout claim confirmed on exactly one of those is a coincidence with a
# large sample size. du04 differs in both places that could plausibly move a
# field:
#
#             HCA                  firmware      libmlx5              distro
#   MN5       ConnectX-7           28.36.1010    rdma-core 2510.0.12  RHEL 9
#   du04      ConnectX-5           16.35.8002    1.25.58.0            Ubuntu
#
# A generation apart on the silicon and a different build of the userspace
# provider. If the offsets hold on both, they are the ABI and not an accident.
#
# WHAT THIS HOST CAN AND CANNOT SETTLE, measured rather than intended.
#
# It validates the ADDRESSING: 13,857 CQEs, qp_num and requester-vs-responder
# class agreeing 100%, so the ring arithmetic and the offsets of sop_drop_qpn
# and op_own are ABI and not an MN5 accident.
#
# It does NOT reach the receive fields. Two PEs on one box produce only
# requester completions — every transfer is an RDMA write from the proxy, and
# the two-sided traffic that carries `wqe_counter` and `srqn` on MN5 is
# bootstrap and collective chatter that a 2-PE MPI job over shared memory never
# generates. `recv=` is absent from the summary here, and that is the honest
# limit: byte_cnt, wqe_counter and srqn_uidx remain confirmed on ConnectX-7
# only. perftest is not installed on this host, which is what would fix it.
#
# NVSHMEM_DISABLE_P2P=1 is what forces it onto the NIC at all: with two GPUs in
# one box every put would otherwise go over NVLink and the HCA would carry
# nothing. IBGDA is left OFF on purpose — under IBGDA the kernel rings the
# doorbell itself, ibv_poll_cq is never called, and there would be no completion
# to read. This probe needs the proxy path, which is the one case 09 exists to
# avoid.
#
# COMSHU_ROOT IS EXPORTED BEFORE common.sh IS SOURCED, and skipping it cost two
# runs here. common.sh defaults COMSHU_ROOT to $HOME/proj/comshu, and du04 HAS a
# second, older checkout at that path — so sourcing it from the synced tree in
# /DATA silently pointed COMSHU and COMSHU_PROVIDER_PATH at the stale one. The run
# then worked perfectly and captured 12,303 ib_poll_cq records with an old
# provider that has no probe in it, which reads exactly like "the CQE could not
# be read". A positive control tells you the probe did not run; it cannot tell
# you that you ran the wrong binary.
#
# POSITIVE CONTROL: any [comshu-cqeprobe] line. Absence means the probe did not
# run, not that the CQE was unreadable.
set -uo pipefail

REMOTE="${COMSHU_A100:-missa18@172.20.31.90}"
SSH="ssh -o BatchMode=yes -o ConnectTimeout=25 ${COMSHU_SSH_OPTS:--J mktips@100.64.0.8}"
RDIR="${COMSHU_A100_DIR:-/DATA/missa18/comshu}"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"

say() { printf '\033[36m==\033[0m %s\n' "$*"; }

# rsync and not git, for the same reason run_on_a100.sh does it: there is no
# bare repo on this host and the checkout is not one either.
say "sync $(git -C "$REPO_ROOT" rev-parse --short HEAD) -> $REMOTE:$RDIR"
$SSH "$REMOTE" "mkdir -p $RDIR" || exit 1
rsync -az --delete --exclude build --exclude 'build-*' --exclude .git \
      -e "$SSH" "$REPO_ROOT/" "$REMOTE:$RDIR/" || exit 1

say "build the ibverbs provider (with the probe) + the workload"
$SSH "$REMOTE" "cd $RDIR && cmake -B build -DCOMSHU_BUILD_TESTS=OFF -DCOMSHU_PROVIDER_IBVERBS=ON -DCOMSHU_PROVIDER_MLX5DV=ON 2>&1 | grep -iE 'mlx5dv.h|error'; cmake --build build -j 2>&1 | tail -2" || exit 1

$SSH "$REMOTE" "cd $RDIR && export COMSHU_ROOT=$RDIR && source tests/hpc/suite/common.sh && comshu_load_runtime 2>/dev/null;
  SM=\$(nvidia-smi --query-gpu=compute_cap --format=csv,noheader | head -1 | tr -d '.')
  APP=\$HOME/asym_patterns-sm\$SM
  if [ ! -x \"\$APP\" ] || [ tests/hpc/asym_patterns.cu -nt \"\$APP\" ]; then
    OMPI_CXX=g++ nvcc -ccbin=mpic++ -O2 -rdc=true \
      -gencode arch=compute_\$SM,code=sm_\$SM -Wno-deprecated-gpu-targets \
      tests/hpc/asym_patterns.cu -o \"\$APP\" \
      -I\"\$NVSHMEM_HOME/include\" -L\"\$NVSHMEM_HOME/lib\" \
      -Xlinker --disable-new-dtags -Xlinker -rpath -Xlinker \"\$NVSHMEM_HOME/lib\" \
      -lnvshmem_host -lnvshmem_device -lcudart || exit 1
  fi
  echo \"app=\$APP\"" || exit 1

say "run: 2 PEs, P2P disabled so every put crosses the NIC"
$SSH "$REMOTE" "cd $RDIR && export COMSHU_ROOT=$RDIR && source tests/hpc/suite/common.sh && comshu_load_runtime 2>/dev/null;
  export LD_LIBRARY_PATH=\"\$NVSHMEM_HOME/lib:\${NCCL_HOME:+\$NCCL_HOME/lib:}\$LD_LIBRARY_PATH\"
  # IBGDA OFF: under it the kernel rings the doorbell and ibv_poll_cq is never
  # called, so there would be no completion to read.
  export COMSHU_CQE_PROBE=1 NVSHMEM_DISABLE_P2P=1 NVSHMEM_IB_ENABLE_IBGDA=0
  SM=\$(nvidia-smi --query-gpu=compute_cap --format=csv,noheader | head -1 | tr -d '.')
  OUT=/tmp/comshu-cqeread-du04; rm -rf \$OUT; mkdir -p \$OUT
  mpirun -np 2 --allow-run-as-root -x PATH -x LD_LIBRARY_PATH -x COMSHU_PROVIDER_PATH \
    -x COMSHU_CQE_PROBE -x NVSHMEM_DISABLE_P2P -x NVSHMEM_IB_ENABLE_IBGDA -x NVSHMEM_BOOTSTRAP=MPI \
    \$COMSHU run -p ib -o \$OUT -- \$HOME/asym_patterns-sm\$SM hub 4096 3 2>&1 |
    grep -E '\[comshu-cqeprobe\]|ERROR|error while loading' | tail -40" 2>&1

say "done — read the rank= summary lines above"
