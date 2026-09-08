#!/bin/bash
# Can this cluster produce a full 48-byte address vector after all?
#
# uar_hijack.hpp derives the AV width per WQE from MLX5_EXTENDED_UD_AV, and the
# README has said since job 45462619 that the wide branch (av_ds=3) has never
# executed: NVSHMEM_IB_FORCE_GRH=1 does not flip support_half_av_seg in the
# CUDA 12.9 SDK build MN5 runs, so dc_full came back compact. The conclusion
# written down then was "needs the paper's P-RoCE hardware".
#
# That conclusion was drawn without looking. MN5's acc nodes carry SIX mlx5
# devices, not one:
#
#   mlx5_0,1,4,5  InfiniBand  200 Gb/s NDR   <- what every case so far has used
#   mlx5_2        Ethernet     25 Gb/s       <- RoCE v2 GIDs at index 3 and 5
#   mlx5_3        Ethernet                    DOWN
#
# mlx5_2 is the admin/service NIC, and an Ethernet mlx5 port with RoCE v2 GIDs
# is exactly the lid == 0 case that sets support_half_av_seg = false. If NVSHMEM
# will bind IBGDA to it, this cluster produces av_ds=3 without any new hardware.
#
# WHY IT MIGHT NOT, stated up front so a failure is informative rather than
# confusing. Four things can each stop it, and the legs below separate them:
#   1. NVSHMEM may refuse the device (no IBGDA support on a RoCE port in this
#      build, or the HCA_LIST filter rejects it).
#   2. GPUDirect RDMA may not reach it. The IB HCAs sit next to the GPUs; a
#      service NIC generally does not, and NIC_BUF_MEMTYPE=gpumem needs the NIC
#      to DMA peer memory. Hence the host-buffer leg.
#   3. The subnet may not carry RDMA — 10.2.0.0/16 is a management network with
#      no reason to have PFC/ECN configured. Lossy RoCE still FUNCTIONS, it just
#      performs badly, and performance is not what this probe wants.
#   4. The GID index may be wrong. 3 is RoCE v2 over ens3f0np0 (10.2.x), 5 is
#      RoCE v2 over the VLAN-499 interface (10.9.x). Both are tried.
#
# PASS is one thing only: an rdma_write row with av_ds=3 that decodes `full`,
# with lengths matching the IB legs of mn5_dc_av.sbatch. Bandwidth is
# irrelevant here — 25 Gb/s of lossy Ethernet is fine for exercising a decoder.
#
#   sbatch --account=etur02 --qos=acc_debug \
#     tests/hpc/probes/uar_doorbell_mode/mn5_roce_av.sbatch
#
#SBATCH --job-name=pcomm-roce-av
#SBATCH --nodes=2
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=20
#SBATCH --gres=gpu:1
#SBATCH --time=00:25:00
#SBATCH --output=roce_av_%j.out
#SBATCH --error=roce_av_%j.out

set -uo pipefail
: "${TMPDIR:=/tmp}"
PCOMM_ROOT="${PCOMM_ROOT:-$HOME/proj/pcomm}"
source "$PCOMM_ROOT/tests/hpc/suite/common.sh"

PCOMM="$PCOMM_ROOT/build/pcomm/pcomm"
export PCOMM_PROVIDER_PATH="$PCOMM_ROOT/build/providers/mlx5dv"
CUDART_DIR="$(dirname "$(ls "$PCOMM_SDK"/cuda/*/targets/x86_64-linux/lib/libcudart.so.12 2>/dev/null | head -1)")"
export LD_LIBRARY_PATH="$NVSHMEM_HOME/lib${CUDART_DIR:+:$CUDART_DIR}:${LD_LIBRARY_PATH:-}"
APP="$NVSHMEM_HOME/bin/perftest/device/pt-to-pt/shmem_put_bw"

export NVSHMEM_IB_ENABLE_IBGDA=1
export NVSHMEM_IBGDA_NIC_HANDLER=cpu
export NVSHMEM_REMOTE_TRANSPORT=ibgda
export NVSHMEM_DISABLE_P2P=1
export NVSHMEM_SYMMETRIC_SIZE=1g
export NVSHMEM_BOOTSTRAP=PMI
export NVSHMEM_BOOTSTRAP_PMI=PMI-2

SRUN="srun --mpi=pmi2 -n 2 --gpus-per-task=1 --overlap --export=ALL"

echo "######## preflight: what is mlx5_2, and can the GPU see it? ########"
$SRUN bash -c '
  echo "### $(hostname)"
  echo "-- link layers --"
  for d in /sys/class/infiniband/*; do
    n=$(basename "$d")
    echo "  $n: $(cat "$d"/ports/1/link_layer 2>/dev/null) $(cat "$d"/ports/1/state 2>/dev/null) lid=$(cat "$d"/ports/1/lid 2>/dev/null)"
  done
  echo "-- mlx5_2 RoCE v2 gids --"
  for i in 3 5; do
    echo "  gid[$i] type=$(cat /sys/class/infiniband/mlx5_2/ports/1/gid_attrs/types/$i 2>/dev/null) ndev=$(cat /sys/class/infiniband/mlx5_2/ports/1/gid_attrs/ndevs/$i 2>/dev/null) $(cat /sys/class/infiniband/mlx5_2/ports/1/gids/$i 2>/dev/null)"
  done
  # PCIe distance decides whether GPUDirect RDMA can reach this NIC at all,
  # which is leg-selection information, not trivia.
  echo "-- GPU <-> NIC topology --"
  nvidia-smi topo -m 2>/dev/null | head -12
' 2>&1 | grep -vE '^(unload|load|Due to MODULEPATH|  1\) bsc)' | head -60

# ---------------------------------------------------------------------------
leg() {  # leg <name> <env assignments...>
  local name="$1"; shift
  local out="${PCOMM_OUT_ROOT:-$HOME/pcomm-suite}/roce_av.$SLURM_JOB_ID.$name"
  echo
  echo "######## leg: $name   ($*) ########"
  local log="$TMPDIR/roce.$name.log"
  ( export "$@"
    timeout 400 $SRUN "$PCOMM" run -p mlx5dv --enable-host-doorbell \
      -o "$out" -- "$APP" > "$log" 2>&1 )
  echo "-- rc=$? --"
  # The transport's own account of what it bound to. If it silently fell back to
  # an IB device this leg proves nothing, and that is exactly how a probe like
  # this produces a false positive.
  grep -aE "device used|NIC handler will be|host doorbell|GID|gid|WARN|ERROR|error" "$log" \
    | sort -u | head -12
  grep -aE "^[ \t]*[0-9]+[ \t]" "$log" | tail -2

  local csv="$TMPDIR/csv.$name"
  rm -rf "$csv"
  "$PCOMM" export "$out" --csv "$csv" >/dev/null 2>&1
  local f="$csv/mlx5_device_send.csv"
  [ -f "$f" ] || { echo "  NO mlx5_device_send.csv — nothing was decoded"; return; }
  echo "-- rows: $(( $(wc -l < "$f") - 1 )) --"
  echo "-- opcode x decode x av_ds x (dest_dct zero?) --"
  tail -n +2 "$f" | awk -F, '{dct=($12=="0x000000")?"dct=0":"dct=SET"; print $6, $13, "av_ds="$NF, dct}' \
    | sort | uniq -c | sort -rn | head
  echo "-- distinct lengths on rdma_write --"
  tail -n +2 "$f" | awk -F, '$6=="rdma_write"{print $11}' | sort -n | uniq -c | tail -6
  # The verdict this file exists to reach.
  if tail -n +2 "$f" | awk -F, '$6=="rdma_write" && $NF==3' | grep -q .; then
    echo "  *** av_ds=3 PRODUCED — the full-AV branch executed ***"
    head -1 "$f"; tail -n +2 "$f" | awk -F, '$6=="rdma_write" && $NF==3' | head -3
  fi
}

# GID 3 = RoCE v2 over the 10.2/16 service network; both nodes are on it.
leg roce_rc  NVSHMEM_HCA_LIST=mlx5_2:1 NVSHMEM_IB_GID_INDEX=3 \
             NVSHMEM_IBGDA_NUM_RC_PER_PE=2
# DC, which is where the AV actually lives — an RC WQE carries none, so the RC
# leg above can only prove the device works, never that av_ds=3 was produced.
leg roce_dc  NVSHMEM_HCA_LIST=mlx5_2:1 NVSHMEM_IB_GID_INDEX=3 \
             NVSHMEM_IBGDA_NUM_RC_PER_PE=0 NVSHMEM_IBGDA_NUM_DCI=1
# Same, on the VLAN-499 GID, in case the flat service network does not carry it.
leg roce_dc_vlan NVSHMEM_HCA_LIST=mlx5_2:1 NVSHMEM_IB_GID_INDEX=5 \
             NVSHMEM_IBGDA_NUM_RC_PER_PE=0 NVSHMEM_IBGDA_NUM_DCI=1
# If GPUDirect RDMA cannot reach the service NIC, the queues can live in host
# memory instead. The AV is built the same way either way, so this leg still
# answers the question even though it is not how anyone would run IBGDA.
leg roce_dc_hostbuf NVSHMEM_HCA_LIST=mlx5_2:1 NVSHMEM_IB_GID_INDEX=3 \
             NVSHMEM_IBGDA_NUM_RC_PER_PE=0 NVSHMEM_IBGDA_NUM_DCI=1 \
             NVSHMEM_IBGDA_FORCE_NIC_BUF_MEMTYPE=hostmem

echo
echo "######## done ########"
