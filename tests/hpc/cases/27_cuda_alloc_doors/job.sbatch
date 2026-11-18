#!/bin/bash
# Case 27 — cuda_alloc_doors: a cudaMalloc is an object whichever provider ran.
#
# PLAN-20261118-1930. The engine observes the CUDA driver's allocation family
# through three doors (libcomshu/src/cuda_alloc_doors.cpp): libcuda's own pointer
# CELLS, which every cudart reaches through cuGetProcAddress; DLSYM of the
# driver's symbol; and the LINK door, the engine's own definitions for code
# linked -lcuda. This case drives every allocation route once each and checks
# four things, because "the doors fire" is the least of them:
#
#   1. the static-cudart workload (nvcc's default) under `-p cupti` — a provider
#      that records no objects of its own — passes the case's assertions: every
#      route is exactly its object, every free closes exactly once, every device
#      allocation names its GPU. This capture is the case's verdict.
#   2. the SAME workload built against the SHARED cudart passes the same
#      assertions: how the runtime is linked must not change what is recorded.
#   3. each run's shutdown line shows the routes arrived through the doors they
#      should — dlsym for the dlsym route, link for the -lcuda routes, cells for
#      the runtime. "Armed" is not evidence; the tally is.
#   4. COMSHU_ALLOC_DOORS=0 records no CUDA allocation at all and the workload
#      still runs: the off switch is real.
#
# 2-4 can fail while 1 passes, so a failure in any of them is written into the
# case's result.json — the board must not go green on a quarter of the evidence.
#
#SBATCH --job-name=comshu-27-cuda-alloc-doors
#SBATCH --account=etur02
#SBATCH --qos=acc_debug
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=20
#SBATCH --gres=gpu:1
#SBATCH --time=00:10:00
#SBATCH --output=%x.%j.out

set -x
COMSHU_ROOT="${COMSHU_ROOT:-$HOME/proj/comshu}"
source "$COMSHU_ROOT/tests/hpc/suite/common.sh"
CASE_DIR="$COMSHU_ROOT/tests/hpc/cases/27_cuda_alloc_doors"
OUT=$(comshu_out cuda_alloc_doors)

# The workload's arguments, from meta.json's `workload` block — the SAME numbers
# the assertions read as params, so the two cannot drift.
ARGS=$(python3 - "$CASE_DIR/meta.json" <<'PY'
import json, sys
w = json.load(open(sys.argv[1]))["workload"]
print(" ".join(str(w[k]) for k in ["size_malloc", "size_managed", "size_async", "size_host",
                                   "size_hostalloc", "size_driver", "size_dlsym",
                                   "loop_iters", "loop_size", "pitch_width", "pitch_height"]))
PY
)

skip() {
  echo "=== 27_cuda_alloc_doors : SKIP ($1) ==="
  mkdir -p "$OUT.csv"
  printf '{"case":"27_cuda_alloc_doors","ok":true,"skipped":true,"reason":"%s","checks":0}\n' \
    "$1" > "$OUT.csv/result.json"
  exit 0
}
fail() {
  echo "=== 27_cuda_alloc_doors : FAIL ($1) ==="
  mkdir -p "$OUT.csv"
  printf '{"case":"27_cuda_alloc_doors","ok":false,"reason":"%s","checks":0}\n' \
    "$1" > "$OUT.csv/result.json"
  exit 1
}

# ---- prerequisites --------------------------------------------------------
comshu_load_runtime
command -v nvidia-smi >/dev/null 2>&1 || skip "no NVIDIA driver on this host"
cupti_so=""
for d in $(echo "$COMSHU_PROVIDER_PATH" | tr ':' ' '); do
  [ -e "$d/libcupti_trace.so" ] && cupti_so="$d/libcupti_trace.so"
done
[ -n "$cupti_so" ] || skip "this build has no libcupti_trace.so (configure with -DCOMSHU_CUPTI=ON)"

# ---- workload, built twice --------------------------------------------------
APP_STATIC="$OUT.allocdoors-static"
APP_SHARED="$OUT.allocdoors-shared"
mkdir -p "$(dirname "$APP_STATIC")"
comshu_load_build
command -v "${COMSHU_NVCC:-nvcc}" >/dev/null 2>&1 || fail "no nvcc (COMSHU_SDK=${COMSHU_SDK:-unset})"
for v in static shared; do
  app="$OUT.allocdoors-$v"
  ${COMSHU_NVCC:-nvcc} -O2 -cudart "$v" \
    -gencode arch=compute_80,code=sm_80 -gencode arch=compute_90,code=sm_90 \
    -Wno-deprecated-gpu-targets \
    "$CASE_DIR/allocdoors.cu" -o "$app" -lcuda -ldl || fail "workload ($v cudart) did not build"
done
comshu_load_runtime

echo "=== bare run (no tracer) ==="
timeout 120 "$APP_STATIC" $ARGS || fail "workload does not run on this host without comshu"

export COMSHU_RANK=0  # one process, no launcher: see case 23 for why it must say so

extra=()  # failures in checks 2-4, folded into result.json at the end

# traced <out> <app> [env...] — run, keep stderr, export, derive.
traced() {
  local out="$1" app="$2"; shift 2
  rm -rf "$out" "$out.csv" "$out.pq"; mkdir -p "$out"
  env "$@" COMSHU_VERBOSE=1 timeout 120 "$COMSHU" run -p cupti -o "$out" -- "$app" $ARGS \
    2> >(tee "$out.stderr" >&2)
  local rc=$?
  echo "traced exit=$rc ($out)"
  [ $rc -eq 0 ] || return $rc
  "$COMSHU" export "$out" --csv "$out.csv"
  comshu_derive "$out"
}

# The door tally an engine prints at exit:
#   [comshu pid N] cuda allocation doors: cell=X dlsym=Y link=Z calls, comshu's own=W ...
door() {  # <stderr file> <cell|dlsym|link>
  sed -n "s/.*cuda allocation doors:.* $2=\([0-9]*\).*/\1/p" "$1" | sort -n | tail -1
}

# Floors, not equalities, except where the workload alone decides the count:
#   dlsym  exactly the workload's one cuMemAlloc_v2 + one cuMemFree_v2 by dlsym
#   link   at least its cuMemAlloc, cuMemFree, two cuMemMap and one cuMemUnmap —
#          a library in the process linked -lcuda may add its own
#   cell   at least the 100 calls of the loop alone; cudart's exact count of
#          driver calls per runtime call is the runtime's business
check_doors() {  # <label> <stderr file>
  local c d l
  c=$(door "$2" cell); d=$(door "$2" dlsym); l=$(door "$2" link)
  echo "doors ($1): cell=${c:-?} dlsym=${d:-?} link=${l:-?}"
  [ "${d:-x}" = "2" ]      || extra+=("$1: dlsym door saw ${d:-nothing}, expected 2")
  [ "${l:-0}" -ge 5 ]      || extra+=("$1: link door saw ${l:-nothing}, expected >= 5")
  [ "${c:-0}" -ge 100 ]    || extra+=("$1: cell door saw ${c:-nothing}, expected >= 100")
}

# ---- 1. static cudart: the verdict --------------------------------------------
echo "=== traced run, static cudart ==="
traced "$OUT" "$APP_STATIC" || fail "traced run (static cudart) did not complete"
"$COMSHU" read "$OUT"
check_doors static "$OUT.stderr"

# ---- 2. shared cudart, same assertions ----------------------------------------
echo "=== traced run, shared cudart ==="
if traced "$OUT-shared" "$APP_SHARED"; then
  check_doors shared "$OUT-shared.stderr"
  bash "$COMSHU_ROOT/tests/hpc/suite/sql_check.sh" "$OUT-shared.csv" "$CASE_DIR" ||
    extra+=("shared cudart: the case's assertions failed (see $OUT-shared.csv)")
else
  extra+=("shared cudart: traced run did not complete")
fi

# ---- 4. the off switch ----------------------------------------------------------
echo "=== traced run, COMSHU_ALLOC_DOORS=0 ==="
if traced "$OUT-off" "$APP_STATIC" COMSHU_ALLOC_DOORS=0; then
  n=$(grep -c ',CuMem' "$OUT-off.csv/mem_object.csv" 2>/dev/null)
  echo "COMSHU_ALLOC_DOORS=0: ${n:-0} CuMem* objects"
  [ "${n:-0}" = "0" ] || extra+=("COMSHU_ALLOC_DOORS=0 still recorded $n CUDA allocations")
else
  extra+=("COMSHU_ALLOC_DOORS=0: the workload did not run with the doors disarmed")
fi

# ---- verdict: the static capture's assertions, plus everything above ----------
bash "$COMSHU_ROOT/tests/hpc/suite/sql_check.sh" "$OUT.csv" "$CASE_DIR"
rc=$?
if [ ${#extra[@]} -gt 0 ]; then
  printf '  [XX ] %s\n' "${extra[@]}"
  python3 - "$OUT.csv/result.json" "${extra[@]}" <<'PY'
import json, sys
p = sys.argv[1]
r = json.load(open(p))
r["ok"] = False
r["extra_failures"] = sys.argv[2:]
json.dump(r, open(p, "w"))
PY
  echo "=== 27_cuda_alloc_doors : FAIL (${#extra[@]} check(s) beyond the assertions) ==="
  exit 1
fi
exit $rc
