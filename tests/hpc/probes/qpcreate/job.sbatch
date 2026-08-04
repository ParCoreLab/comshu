#!/bin/bash
# PROBE — which symbol does perftest actually create its queue pair with?
#
# Not a suite case. It answers one question and then either becomes a hook or
# gets deleted.
#
# Case 08 has never captured a queue-pair entity. The standing explanation was
# "perftest uses mlx5dv_create_qp, which we do not hook". That hook now exists
# and the entity is still missing: job 44199402 ran -p ib,devx with both
# providers loaded, no engine error, and wrote ZERO 200-range streams while the
# ib provider worked normally beside it (1000 post_send, 1010 poll_cq).
#
# ib_send_bw's dynamic symbol table imports THREE candidate doors —
#   ibv_create_qp@IBVERBS_1.1, mlx5dv_create_qp@MLX5_1.3, rdma_create_qp@RDMACM_1.0
# — and guessing between them has now been wrong twice, so this measures.
# LD_DEBUG=bindings names both the caller and the definition each reference
# resolves to, which is what settled the equivalent question for IBGDA.
#
#SBATCH --job-name=pcomm-probe-qpcreate
#SBATCH --account=etur02
#SBATCH --qos=acc_debug
#SBATCH --nodes=2
#SBATCH --ntasks-per-node=1
#SBATCH --time=00:08:00
#SBATCH --output=%x.%j.out

set -x
PCOMM_ROOT="${PCOMM_ROOT:-$HOME/proj/pcomm}"
source "$PCOMM_ROOT/tests/hpc/suite/common.sh"
pcomm_load_runtime

OUT="${PCOMM_OUT_ROOT:-$HOME/pcomm-suite}/probe-qpcreate-${SLURM_JOB_ID:-local}"
rm -rf "$OUT"; mkdir -p "$OUT/bind"

nodes=($(scontrol show hostnames "$SLURM_JOB_NODELIST"))
server="${nodes[0]}"; client="${nodes[1]}"
devcmd='DEV=$(ibv_devinfo 2>/dev/null | awk "/hca_id/{d=\$2} /PORT_ACTIVE/{print d; exit}")'

# LD_DEBUG on the client only: one side is enough to see which door is used, and
# the output is large enough that both would just double it.
srun --nodes=1 --ntasks=1 --nodelist="$server" --overlap bash -c \
  "$devcmd; PCOMM_RANK=0 '$PCOMM' -p ib,devx -o '$OUT/cap' -- ib_send_bw -d \$DEV -n 20 -s 1024" &
sleep 4
srun --nodes=1 --ntasks=1 --nodelist="$client" --overlap bash -c \
  "$devcmd; LD_DEBUG=bindings LD_DEBUG_OUTPUT='$OUT/bind/b' PCOMM_RANK=1 '$PCOMM' -p ib,devx \
   -o '$OUT/cap' -- ib_send_bw -d \$DEV -n 20 -s 1024 $server"
wait

echo "=== which create-qp symbols were bound, and to what? ==="
for s in ibv_create_qp mlx5dv_create_qp rdma_create_qp rdma_create_qp_ex ibv_create_qp_ex; do
  echo "--- $s"
  grep -rh "\`$s'" "$OUT/bind"/ 2>/dev/null | sed 's/.*binding file //' | sort -u | head -4
done

echo "=== did the mlx5dv provider write anything? ==="
find "$OUT/cap" -name '2*.pcs' | head -5 || echo "  no 200-range streams"

echo "=== what the binary imports (static, for comparison) ==="
readelf --dyn-syms --wide "$(command -v ib_send_bw)" 2>/dev/null |
  grep -oE "(ibv|mlx5dv|rdma)_create_qp[a-z_]*@[A-Z0-9_.]+" | sort -u
