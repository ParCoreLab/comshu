#!/bin/bash
# Does the host doorbell path actually CAPTURE anything on MN5?
#
# mn5.sbatch proved the platform refuses to map a doorbell page into a GPU, so
# the host path arms there. That is the guard. This is the mechanism: a real
# two-node NVSHMEM run in CPU-assisted IBGDA, where the GPU builds WQEs in a
# GPU-resident send queue and a host thread rings the doorbell, captured with
# --enable-host-doorbell.
#
# PASS looks like: doorbells > 0 AND sends > 0 in the shutdown line, and
# non-empty mlx5_doorbell / mlx5_device_send tables. Case 12 has SKIPped on this
# cluster for its whole life (no PeerMappingOverride); this is the arm that
# replaces the skip, and if it works it should be folded back into case 12.
#
#   sbatch --account=etur02 --qos=acc_debug \
#     tests/hpc/probes/uar_doorbell_mode/mn5_cpu_assisted.sbatch
#
#SBATCH --job-name=pcomm-hd-cpuassist
#SBATCH --nodes=2
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=20
#SBATCH --gres=gpu:1
#SBATCH --time=00:15:00
#SBATCH --output=hd_cpuassist_%j.out
#SBATCH --error=hd_cpuassist_%j.out

set -x
PCOMM_ROOT="${PCOMM_ROOT:-$HOME/proj/pcomm}"
source "$PCOMM_ROOT/tests/hpc/suite/common.sh"

PCOMM="$PCOMM_ROOT/build/pcomm/pcomm"
export PCOMM_PROVIDER_PATH="$PCOMM_ROOT/build/providers/mlx5dv"
# The SDK's perftest binaries link libcudart.so.12, which lives in the SDK's own
# CUDA tree and is NOT on the default path here — the first run of this died at
# exit 127 for that and nothing else.
CUDART_DIR="$(dirname "$(ls "$PCOMM_SDK"/cuda/*/targets/x86_64-linux/lib/libcudart.so.12 2>/dev/null | head -1)")"
export LD_LIBRARY_PATH="$NVSHMEM_HOME/lib${CUDART_DIR:+:$CUDART_DIR}:${LD_LIBRARY_PATH:-}"
APP="$NVSHMEM_HOME/bin/perftest/device/pt-to-pt/shmem_put_bw"

# CPU-assisted IBGDA, explicitly. `cpu` is the GDRCopy handler and
# `cpu_host_memory` the plain-host-memory one; ask for the first and let
# NVSHMEM fall back, then read back which it chose from its own log.
export NVSHMEM_IB_ENABLE_IBGDA=1
export NVSHMEM_IBGDA_NIC_HANDLER=cpu
export NVSHMEM_REMOTE_TRANSPORT=ibgda
export NVSHMEM_DISABLE_P2P=1          # force the traffic onto the NIC
export NVSHMEM_DEBUG=INFO             # so "NIC handler will be" is in the log
export NVSHMEM_SYMMETRIC_SIZE=1g

OUT="${PCOMM_OUT_ROOT:-$HOME/pcomm-suite}/hd_cpuassist.$SLURM_JOB_ID"
mkdir -p "$(dirname "$OUT")"

echo "== nodes: $SLURM_JOB_NODELIST =="
echo "== app: $APP =="
ls -l "$APP" || { echo "NO APP — perftest missing from this SDK"; exit 1; }

# Bootstrap: the SDK's perftest binaries ask for PMI 1.0, which SLURM's pmi2
# plugin rejects outright ("unsupported PMI version: 1.0" — it killed the first
# attempt and left srun hanging). This SLURM offers pmix_v4, so use PMIx on both
# sides and say so to NVSHMEM explicitly rather than letting it guess.
export NVSHMEM_BOOTSTRAP=PMIX
# ...and name the plugin file explicitly. This SDK ships the PMIx bootstrap only
# as nvshmem_bootstrap_pmix.so.3, while NVSHMEM dlopens the UNSUFFIXED name — so
# the default lookup fails with ENOENT and surfaces as "non-zero status: 7 ...
# aborting due to error in nvshmemi_init_thread", which looks like a transport
# problem and is a missing symlink.
export NVSHMEM_BOOTSTRAP_PMIX_PLUGIN=nvshmem_bootstrap_pmix.so.3
srun --mpi=pmix -n 2 "$PCOMM" run -p mlx5dv --enable-host-doorbell \
     -o "$OUT" -- "$APP" 2>&1
rc=$?
echo "== run rc=$rc =="

echo "== which NIC handler did NVSHMEM choose? =="
grep -h "NIC handler will be" "$OUT"*.err 2>/dev/null | head -4 || \
  echo "  (no NVSHMEM log captured; it is on stdout above)"

echo "== what landed in the capture? =="
"$PCOMM" read "$OUT" 2>&1 | head -40
