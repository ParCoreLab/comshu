#!/bin/bash
#SBATCH --job-name=comshu-fabricshare
#SBATCH --account=etur02
#SBATCH --qos=acc_debug
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=2
#SBATCH --cpus-per-task=20
#SBATCH --gres=gpu:2
#SBATCH --time=00:20:00
#SBATCH --output=fabricshare-%j.out
#
# Validation for PeerMapping (record 526). See probe.cu for what is being tested
# and why it has to be synthetic.
#
# NOT PIPED THROUGH grep, deliberately. The last probe filtered mpirun's output
# and a crash under NVBit came back looking like an empty capture; the filter
# hid the only line that said what happened.
set -x
COMSHU_ROOT="${COMSHU_ROOT:-$HOME/proj/comshu}"
source "$COMSHU_ROOT/tests/hpc/suite/common.sh"
comshu_load_runtime

APP="$HOME/fabricshare_probe"
SRC="$COMSHU_ROOT/tests/hpc/probes/fabricshare/probe.cu"
if [ ! -x "$APP" ] || [ "$SRC" -nt "$APP" ]; then
  comshu_load_build
  OMPI_CXX=g++ nvcc -ccbin=mpic++ -O2 -lineinfo -gencode arch=compute_90,code=sm_90 \
    -Wno-deprecated-gpu-targets "$SRC" -o "$APP" -lcuda \
    || { echo "!! nvcc failed" >&2; exit 1; }
  comshu_load_runtime
fi

# Bare first: whether fabric sharing works between two processes on this machine
# is a question about the driver and the fabric manager, not about comshu.
echo "=== BARE ==="
mpirun -np 2 --map-by ppr:2:node -x PATH -x LD_LIBRARY_PATH "$APP"
echo "bare exit=$?"

OUT=$(comshu_out fabricshare); rm -rf "$OUT"; mkdir -p "$OUT"
echo "=== UNDER the tracer ==="
mpirun -np 2 --map-by ppr:2:node -x PATH -x LD_LIBRARY_PATH -x COMSHU_PROVIDER_PATH \
  -x COMSHU_KERNEL_NAME=poke -x COMSHU_VERBOSE=1 \
  "$COMSHU" run -p p2p -o "$OUT" -- "$APP"
echo "traced exit=$?"

echo "=== captured streams ==="; "$COMSHU" read "$OUT"
"$COMSHU" export "$OUT" --csv "$OUT.csv"
echo "=== peer_mapping rows (THE POINT) ==="
cat "$OUT.csv/peer_mapping.csv" 2>/dev/null || echo "  (no peer_mapping.csv — nothing was fabric-shared)"
echo "=== do the two ranks agree on the allocation id? ==="
awk -F, 'NR>1 {print $3}' "$OUT.csv/peer_mapping.csv" 2>/dev/null | sort | uniq -c
echo "  (a count of 2 on one id = the join works; two ids of 1 = it does not)"
