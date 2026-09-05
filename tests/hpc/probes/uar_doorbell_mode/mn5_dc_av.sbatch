#!/bin/bash
# Does the WQE decoder read a DC address vector correctly — at BOTH widths?
#
# Commit "the address vector has two widths" rewrote the DC path: the old parser
# assumed the 16-byte compact AV unconditionally, which on a full-AV port read
# raddr, rkey, lkey, laddr and length 32 bytes early and emitted rows that were
# wrong rather than absent. The fix derives the width per WQE from the
# MLX5_EXTENDED_UD_AV bit in dqp_dct, and reads the destination DCT from its low
# 24 bits.
#
# None of that has run. Job 45449988 captured 7,232 sends on MN5 and every one
# was RC (dest_dct=0), so the branch that was rewritten is the branch that has
# never executed. This exercises it, both ways, on one node pair:
#
#   rc       NVSHMEM default (2 RC QPs per PE)      -> dest_dct = 0,  no AV
#   dc_half  NUM_RC_PER_PE=0                        -> DC, ds=4, 16 B AV
#   dc_full  NUM_RC_PER_PE=0 + IB_FORCE_GRH=1       -> DC, ds=6, 48 B AV
#
# The third leg is the point. ibgda.cpp sets support_half_av_seg = false when
# IB_FORCE_GRH is set, which is the same branch RoCE takes (lid == 0) — so an IB
# cluster can be made to produce the layout that would otherwise need RoCE
# hardware to test. The paper's own mgda_bench lists its full-AV path as
# untested (HANDOFF-V3 S6), so this is the first exercise on either side.
#
# PASS: dc_half and dc_full both show non-zero dest_dct and decode=full on their
# rdma_write rows, with lengths matching the rc leg. FAIL that is worth having:
# dc_full comes back truncated, or its raddr/length are nonsense.
#
#   sbatch --account=etur02 --qos=acc_debug \
#     tests/hpc/probes/uar_doorbell_mode/mn5_dc_av.sbatch
#
#SBATCH --job-name=pcomm-dc-av
#SBATCH --nodes=2
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=20
#SBATCH --gres=gpu:1
#SBATCH --time=00:20:00
#SBATCH --output=dc_av_%j.out
#SBATCH --error=dc_av_%j.out

set -uo pipefail
: "${TMPDIR:=/tmp}"
PCOMM_ROOT="${PCOMM_ROOT:-$HOME/proj/pcomm}"
source "$PCOMM_ROOT/tests/hpc/suite/common.sh"

PCOMM="$PCOMM_ROOT/build/pcomm/pcomm"
export PCOMM_PROVIDER_PATH="$PCOMM_ROOT/build/providers/mlx5dv"
CUDART_DIR="$(dirname "$(ls "$PCOMM_SDK"/cuda/*/targets/x86_64-linux/lib/libcudart.so.12 2>/dev/null | head -1)")"
export LD_LIBRARY_PATH="$NVSHMEM_HOME/lib${CUDART_DIR:+:$CUDART_DIR}:${LD_LIBRARY_PATH:-}"
APP="$NVSHMEM_HOME/bin/perftest/device/pt-to-pt/shmem_put_bw"

# CPU-assisted IBGDA: the only mode this cluster can run, and the one the host
# doorbell path watches. See mn5.sbatch for the measurement behind that.
export NVSHMEM_IB_ENABLE_IBGDA=1
export NVSHMEM_IBGDA_NIC_HANDLER=cpu
export NVSHMEM_REMOTE_TRANSPORT=ibgda
export NVSHMEM_DISABLE_P2P=1
export NVSHMEM_SYMMETRIC_SIZE=1g
export NVSHMEM_BOOTSTRAP=PMI
export NVSHMEM_BOOTSTRAP_PMI=PMI-2

leg() {  # leg <name> <env assignments...>
  local name="$1"; shift
  local out="${PCOMM_OUT_ROOT:-$HOME/pcomm-suite}/dc_av.$SLURM_JOB_ID.$name"
  echo
  echo "######## leg: $name   ($*) ########"
  ( export "$@"
    srun --mpi=pmi2 -n 2 --gpus-per-task=1 --overlap --export=ALL \
         "$PCOMM" run -p mlx5dv --enable-host-doorbell -o "$out" -- "$APP" 2>&1 \
      | grep -E "host doorbell|NIC handler will be|IBGDA: device used" | head -6 )
  echo "-- rc=$? --"

  local csv="$TMPDIR/csv.$name"
  rm -rf "$csv"
  "$PCOMM" export "$out" --csv "$csv" >/dev/null 2>&1
  local f="$csv/mlx5_device_send.csv"
  [ -f "$f" ] || { echo "  NO mlx5_device_send.csv"; return; }
  echo "-- rows: $(( $(wc -l < "$f") - 1 )) --"
  echo "-- opcode x decode x av_ds x (dest_dct zero?) --"
  # av_ds is THE column this leg exists for: 0 = no AV (RC), 1 = compact 16 B,
  # 3 = full 48 B. Without it a dc_full leg that silently produced compact AVs
  # looked exactly like one that worked (job 45452104).
  tail -n +2 "$f" | awk -F, '{dct=($12=="0x000000")?"dct=0":"dct=SET"; print $6, $13, "av_ds="$NF, dct}' \
    | sort | uniq -c | sort -rn | head
  echo "-- three rdma_write rows verbatim --"
  head -1 "$f"
  tail -n +2 "$f" | awk -F, '$6=="rdma_write"' | head -3
  echo "-- distinct lengths on rdma_write --"
  tail -n +2 "$f" | awk -F, '$6=="rdma_write"{print $11}' | sort -n | uniq -c | tail -6
}

leg rc      NVSHMEM_IBGDA_NUM_RC_PER_PE=2
leg dc_half NVSHMEM_IBGDA_NUM_RC_PER_PE=0 NVSHMEM_IBGDA_NUM_DCI=1
leg dc_full NVSHMEM_IBGDA_NUM_RC_PER_PE=0 NVSHMEM_IBGDA_NUM_DCI=1 NVSHMEM_IB_FORCE_GRH=1

echo
echo "######## done ########"
