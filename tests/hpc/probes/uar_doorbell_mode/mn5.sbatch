#!/bin/bash
# Which doorbell mechanism does MN5 need? One ACC node, no network traffic.
#
# This answers the one claim in
# plans/PLAN-20260905-1224-cpu-assisted-ibgda-is-the-cluster-we-have-todo.md
# that was inferred from someone else's run notes rather than measured here:
# that MN5's open kernel module refuses to map a NIC doorbell page into a GPU,
# which is what forces NVSHMEM_IBGDA_NIC_HANDLER=cpu and is the entire reason
# the host doorbell path exists.
#
#   sbatch --account=etur02 --qos=acc_debug tests/hpc/probes/uar_doorbell_mode/mn5.sbatch
#
# Expected: "the driver refuses ... NOT_PERMITTED" and uars_armed=1.
# If it says "DOES map" instead, the plan's premise is wrong and the host path
# is not what MN5 needs — which would be worth knowing immediately.
#SBATCH --job-name=pcomm-uar-mode
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=20
#SBATCH --gres=gpu:1
#SBATCH --time=00:10:00
#SBATCH --output=uar_mode_%j.out
#SBATCH --error=uar_mode_%j.out

set -uo pipefail
module load gcc/13.2.0 cmake >/dev/null 2>&1
module load cuda/12.6 >/dev/null 2>&1 || module load nvidia-hpc-sdk >/dev/null 2>&1

: "${TMPDIR:=/tmp}"
REPO="${PCOMM_REPO:-$HOME/proj/pcomm}"
PROBE_SRC="$REPO/tests/hpc/probes/uar_doorbell_mode/probe.c"
PROBE_BIN="$TMPDIR/uar_probe"

echo "== node: $(hostname) =="
nvidia-smi --query-gpu=name,driver_version --format=csv,noheader | head -2
echo "== nvidia kernel module params (PeerMappingOverride is the one that matters) =="
grep -iE "peermapping|RegistryDwords" /proc/driver/nvidia/params 2>/dev/null || echo "  (params unreadable)"
echo "== open kernel module? =="
modinfo nvidia 2>/dev/null | grep -iE "^license|^name" | head -2 || echo "  (modinfo unavailable)"

CUDA_INC=""; CUDA_STUB=""
for r in "${CUDA_HOME:-}" "${CUDA_ROOT:-}" /apps/ACC/CUDA/12.6 /usr/local/cuda; do
  [ -n "$r" ] && [ -f "$r/include/cuda.h" ] && { CUDA_INC="-I$r/include"; CUDA_STUB="-L$r/lib64/stubs"; break; }
done
echo "== building probe (${CUDA_INC:-no cuda include found}) =="
gcc -O0 -o "$PROBE_BIN" "$PROBE_SRC" $CUDA_INC $CUDA_STUB -libverbs -lmlx5 -lcuda || {
  echo "PROBE BUILD FAILED"; exit 1; }

PCOMM="$REPO/build/pcomm/pcomm"
PROV="$REPO/build/providers/mlx5dv"

echo
echo "===== --enable-host-doorbell (the mode this cluster is expected to need) ====="
PCOMM_PROVIDER_PATH="$PROV" "$PCOMM" run -p mlx5dv --enable-host-doorbell \
    -o "$TMPDIR/hd-out" -- "$PROBE_BIN" 2>&1

echo
echo "===== --enable-doorbell-hijack (the GPU path, for contrast) ====="
PCOMM_PROVIDER_PATH="$PROV" "$PCOMM" run -p mlx5dv --enable-doorbell-hijack \
    -o "$TMPDIR/gh-out" -- "$PROBE_BIN" 2>&1

echo
echo "===== done ====="
