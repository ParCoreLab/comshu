#!/bin/bash
# PROBE — is the raw mlx5 CQE readable, and correct, after ibv_poll_cq returns?
#
# Not a suite case. It answers one question and then either becomes a record or
# gets deleted. See providers/ibverbs/cqe_probe.hpp for the full argument.
#
# THE QUESTION. object_access now attributes two-sided SENDs by recovering the
# receiver's buffer through wr_id — completion carries wr_id, ib_post_recv
# carries (wr_id, addr). That works on every capture in the corpus, but wr_id is
# chosen by the APPLICATION and recycled, and an SRQ receive can only be keyed on
# (rank, wr_id) because an SRQ WQE is not bound to a QP until a message lands.
#
# The hardware already knows the exact answer. Every mlx5 CQE carries
# `wqe_counter` — the index of the WQE the NIC consumed — and libmlx5 uses
# precisely that field to look the wr_id UP (`srq->wrid[wqe_ctr]`). We have been
# reconstructing the input of a table lookup from its output.
#
# So: can we just read it? The CQE lives in the driver's ring, and this provider
# hooks ibv_poll_cq on the way OUT, after the driver has consumed the entry and
# advanced its consumer index. The bytes should still be there — a ring entry is
# only overwritten on wrap — but that is a claim about somebody else's data
# structure and it is worth one job rather than one argument.
#
# HOW TO READ THE RESULT. Every CQE read is cross-checked against the ibv_wc the
# driver returned for the same completion, and the summary prints the rates:
#
#   read=N qpn_ok=N class_ok=N len_ok=N/N     -> the arithmetic is right and the
#                                                entry is intact. The cheap
#                                                version works; wqe_counter can
#                                                be recorded from this hook.
#   read=N qpn_ok=0                           -> we are reading the wrong slot.
#                                                Either the consumer index is not
#                                                where we think or the ring is
#                                                indexed differently.
#   read=0, cqs=N mapped=0                    -> mlx5dv_init_obj refused every
#                                                CQ. Not a disproof of the idea,
#                                                a failure to reach it.
#   qpn_ok high but len_ok low                -> right entry, wrong field offsets
#                                                (a struct-layout difference).
#
# TWO ARMS, BOTH REQUIRED, because they exercise different receive paths:
#   ib_send_bw  posts receives to a QP's own receive queue      (origin = qp)
#   NVSHMEM hub posts them to a shared receive queue            (origin = srq)
# The SRQ arm is the one that motivated this: it is where wr_id is the weaker
# key. A run that only exercised perftest would prove nothing about the case we
# actually need.
#
# POSITIVE CONTROL: any [pcomm-cqeprobe] line at all. A run with none did not
# execute the probe, and nothing it did not print is a finding. An earlier probe
# here mis-stated its own control and another reported a false positive because
# grep -c matched the filename rather than a line in it.
#
#   sbatch tests/hpc/probes/cqeread/job.sbatch
#
#SBATCH --job-name=pcomm-probe-cqeread
#SBATCH --account=etur02
#SBATCH --qos=acc_debug
#SBATCH --nodes=2
#SBATCH --ntasks-per-node=1
#SBATCH --gres=gpu:4
#SBATCH --time=00:15:00
#SBATCH --output=%x.%j.out

set -x
PCOMM_ROOT="${PCOMM_ROOT:-$HOME/proj/pcomm}"
source "$PCOMM_ROOT/tests/hpc/suite/common.sh"
pcomm_load_runtime

export PCOMM_CQE_PROBE=1

# --- arm 1: perftest, QP receives -------------------------------------------
OUT=$(pcomm_out probe_cqeread_qp); rm -rf "$OUT"; mkdir -p "$OUT"
nodes=($(scontrol show hostnames "$SLURM_JOB_NODELIST"))
server=${nodes[0]}; client=${nodes[1]}
echo "=== ARM 1 (QP receives): ib_send_bw server=$server client=$client"

devcmd='DEV=$(ibv_devinfo 2>/dev/null | awk "/hca_id/{d=\$2} /PORT_ACTIVE/{print d; exit}")'

srun --nodes=1 --ntasks=1 --nodelist="$server" --overlap bash -c \
  "$devcmd; PCOMM_CQE_PROBE=1 PCOMM_RANK=0 '$PCOMM' run -p ib -o '$OUT' -- ib_send_bw -d \$DEV -n 1000 -s 8192" &
SRV=$!
sleep 6
srun --nodes=1 --ntasks=1 --nodelist="$client" --overlap bash -c \
  "$devcmd; PCOMM_CQE_PROBE=1 PCOMM_RANK=1 '$PCOMM' run -p ib -o '$OUT' -- ib_send_bw -d \$DEV -n 1000 -s 8192 $server"
CLI=$?; wait $SRV; SRV_RC=$?
echo "arm1 client_rc=$CLI server_rc=$SRV_RC"

# --- arm 2: NVSHMEM hub, SRQ receives ---------------------------------------
# Same binary and build discipline as case 16 — see its job.sbatch for why the
# arch is in the name and why LD_LIBRARY_PATH is exported after the build.
PCOMM_SM=$(nvidia-smi --query-gpu=compute_cap --format=csv,noheader 2>/dev/null | head -1 | tr -d '.')
PCOMM_SM=${PCOMM_SM:-90}
APP="$HOME/asym_patterns-sm$PCOMM_SM"

if [ ! -x "$APP" ] || [ "$PCOMM_ROOT/tests/hpc/asym_patterns.cu" -nt "$APP" ]; then
  pcomm_load_build
  OMPI_CXX=g++ nvcc -ccbin=mpic++ -O2 -rdc=true \
    -gencode arch=compute_$PCOMM_SM,code=sm_$PCOMM_SM -Wno-deprecated-gpu-targets \
    "$PCOMM_ROOT/tests/hpc/asym_patterns.cu" -o "$APP" \
    -I"$NVSHMEM_HOME/include" -L"$NVSHMEM_HOME/lib" \
    -Xlinker --disable-new-dtags -Xlinker -rpath -Xlinker "$NVSHMEM_HOME/lib" \
    -lnvshmem_host -lnvshmem_device -lcudart \
    || { echo "!! nvcc failed — refusing to run a stale or missing binary" >&2; exit 1; }
fi
pcomm_load_runtime
export LD_LIBRARY_PATH="$NVSHMEM_HOME/lib:$NCCL_HOME/lib:$LD_LIBRARY_PATH"

OUT2=$(pcomm_out probe_cqeread_srq); rm -rf "$OUT2"; mkdir -p "$OUT2"
echo "=== ARM 2 (SRQ receives): NVSHMEM hub, one rank per node"
mpirun -np 2 --map-by ppr:1:node -x PATH -x LD_LIBRARY_PATH \
  -x PCOMM_PROVIDER_PATH -x PCOMM_CQE_PROBE -x NVSHMEM_BOOTSTRAP=MPI \
  "$PCOMM" run -p ib -o "$OUT2" -- "$APP" hub 4096 3
echo "arm2 mpirun exit=$?"

# --- the reading -------------------------------------------------------------
# Printed here rather than left to the reader's grep, because a probe whose
# result needs reconstructing from a 3000-line job log is one that gets
# misquoted. These greps run against THIS job's own output file.
LOG="$SLURM_SUBMIT_DIR/pcomm-probe-cqeread.$SLURM_JOB_ID.out"
sync; sleep 2
echo "=== POSITIVE CONTROL: probe lines present? ==="
grep -c '\[pcomm-cqeprobe\]' "$LOG" || echo "0 — PROBE DID NOT RUN, nothing below is a finding"
echo "=== per-rank summaries ==="
grep '\[pcomm-cqeprobe\] rank=' "$LOG" | sort -u
echo "=== sample CQE rows ==="
grep '\[pcomm-cqeprobe\] rank=.* cqe idx=' "$LOG" | head -24
echo "=== any mismatch at all ==="
grep -E '\[pcomm-cqeprobe\].*(QPN-MISMATCH|CLASS-BAD|unusable|not resolvable|WITHOUT mlx5dv)' "$LOG" | head -20 \
  || echo "  (none)"
