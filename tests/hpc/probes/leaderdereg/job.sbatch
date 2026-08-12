#!/bin/bash
# PROBE — two ib_mem_regions per NODE LEADER are opened and never closed.
#
# Not a suite case. It answers one question and then either becomes a fix or
# gets deleted.
#
# THE FINDING IT EXPLAINS. Job 44514759 (64 PEs, 16 nodes x 4,
# NVSHMEM_DISABLE_P2P=1, -p ib) is green on everything structural — the 126-edge
# star resolves exactly, zero unresolved peers, zero self-edges — and red on
# entity_close_accounted with a signature too regular to be a race:
#
#   32 unclosed ib_mem_region = exactly 2 on each of the 16 ranks with
#   rank % 4 == 0 (local rank 0 of each node), 0 on the other 48.
#   Per leader: one 40-byte host buffer and one 8 MiB host buffer, same PD,
#   access=3 (LOCAL_WRITE|REMOTE_WRITE), origin=reg_mr.
#
# THREE EXPLANATIONS, and reasoning cannot separate them:
#   (a) the owning library registers these and leaks them at exit;
#   (b) it deregisters them through a door we do not hook;
#   (c) it deregisters them after pcomm has already finalized.
#
# WHAT THIS PROBE CHANGES: exactly one variable, twice, in ONE allocation.
# Section A is the 64-rank job's configuration shrunk to 2 nodes. Section B is
# identical except NVSHMEM_DISABLE_P2P is absent. Same nodes, same binary, same
# ranks-per-node, back to back — because the last time a comparison here was
# drawn against a case that differed in more than one way, it was wrong and cost
# a job (see plans/PLAN-20260810-0854, "the case 04 comparison is not
# controlled").
#
#   A leaks and B does not  -> DISABLE_P2P creates the region; it is the
#                              intra-node-over-IB path, and (a) or (b) about a
#                              buffer that only exists in that configuration.
#   both leak               -> nothing to do with the flag. Case 06 (4 ranks,
#                              1 node, ib+p2p) passes entity_close_accounted, so
#                              the variable would then be node COUNT, not P2P.
#   neither leaks           -> it needs scale to appear, which is itself the
#                              finding and says the 16-node run must be reread.
#
# PCOMM_TRACE_MR_CALLERS=1 dladdr's the caller at each MR door, so if a region
# leaks this also NAMES the library that registered it — and whether that same
# library ever calls dereg_mr. That is what separates (a) from (b): a library
# under reg only is a leak; a library under neither is a door we do not hook.
#
# POSITIVE CONTROL: any [pcomm-mrcaller] line at all, and a nonzero unclosed
# count in at least one section. If BOTH sections report zero unclosed regions
# AND zero mrcaller lines, the diagnostic did not run and nothing here is a
# finding — an earlier probe in this directory mis-stated its own control.
#
#SBATCH --job-name=pcomm-probe-leaderdereg
#SBATCH --account=etur02
#SBATCH --qos=acc_debug
#SBATCH --nodes=2
#SBATCH --ntasks-per-node=4
#SBATCH --cpus-per-task=20
#SBATCH --gres=gpu:4
#SBATCH --time=00:20:00
#SBATCH --output=%x.%j.out

set -x
PCOMM_ROOT="${PCOMM_ROOT:-$HOME/proj/pcomm}"
source "$PCOMM_ROOT/tests/hpc/suite/common.sh"
pcomm_load_runtime

PCOMM_SM=$(nvidia-smi --query-gpu=compute_cap --format=csv,noheader 2>/dev/null | head -1 | tr -d '.')
PCOMM_SM=${PCOMM_SM:-90}
APP="$HOME/asym_patterns-sm$PCOMM_SM"
[ -x "$APP" ] || { echo "!! no $APP — run a 13-18 case or the scale job first" >&2; exit 1; }

export LD_LIBRARY_PATH="$NVSHMEM_HOME/lib:$NCCL_HOME/lib:$LD_LIBRARY_PATH"

ROOT="${PCOMM_OUT_ROOT:-$HOME/pcomm-suite}/probe-leaderdereg-${SLURM_JOB_ID:-local}"
rm -rf "$ROOT"; mkdir -p "$ROOT"

# One section. $1 = label, rest = extra -x flags for mpirun.
section() {
  label="$1"; shift
  out="$ROOT/$label"; mkdir -p "$out"
  mpirun -np 8 --map-by ppr:4:node -x PATH -x LD_LIBRARY_PATH \
    -x PCOMM_PROVIDER_PATH -x NVSHMEM_BOOTSTRAP=MPI \
    -x PCOMM_TRACE_MR_CALLERS=1 "$@" \
    "$PCOMM" run -p ib -o "$out/cap" -- "$APP" hub 4096 3 2>&1 | tee "$out/run.log"
  echo "[$label] mpirun exit=${PIPESTATUS[0]}"
  "$PCOMM" export "$out/cap" --csv "$out/cap.csv" || return 1
}

echo "=== SECTION A — with NVSHMEM_DISABLE_P2P=1 (the 64-rank job's config) ==="
section A -x NVSHMEM_DISABLE_P2P=1

echo "=== SECTION B — control, P2P left enabled ==="
section B

# The measurement, identical for both: per-rank count of ib_mem_region handles
# with no matching EntityClose. Written here rather than as a suite assertion
# because it must report the SHAPE (which ranks, what sizes), not pass/fail.
echo "=== unclosed ib_mem_region per rank ==="
python3 - "$ROOT" <<'PY'
import csv, os, sys
root = sys.argv[1]
for label in ("A", "B"):
    d = os.path.join(root, label, "cap.csv")
    mr, cl = os.path.join(d, "ib_mem_region.csv"), os.path.join(d, "entity_close.csv")
    if not (os.path.exists(mr) and os.path.exists(cl)):
        print(f"{label}: MISSING export ({d}) — section did not produce a capture")
        continue
    opens = {}
    for r in csv.DictReader(open(mr)):
        opens.setdefault(int(r["rank"]), {})[r["mr"]] = (r.get("size"), r.get("base"))
    closed = set()
    for r in csv.DictReader(open(cl)):
        if r["opener"] == "100":
            closed.add((int(r["rank"]), r["subject"]))
    print(f"--- section {label}")
    total = 0
    for rank in sorted(opens):
        un = [h for h in opens[rank] if (rank, h) not in closed]
        total += len(un)
        if un:
            sizes = ", ".join(str(opens[rank][h][0]) for h in un)
            print(f"  rank {rank:2d} (local {rank % 4}): {len(un)} unclosed  sizes: {sizes}")
    print(f"  total unclosed: {total}   leaders affected: "
          f"{sorted(r for r in opens if any((r, h) not in closed for h in opens[r]))}")
PY

echo "=== which library calls which MR door (both sections) ==="
for s in A B; do
  printf '%s: mrcaller lines %s\n' "$s" "$(grep -c 'pcomm-mrcaller' "$ROOT/$s/run.log" 2>/dev/null)"
  grep -h 'pcomm-mrcaller' "$ROOT/$s/run.log" 2>/dev/null | sort -u | sed "s/^/  $s /"
done

echo "=== reg-only libraries: registered through a hooked door, never deregistered ==="
for s in A B; do
  awk -v s="$s" '/pcomm-mrcaller/ {door=$2; who=$4; if (door=="dereg_mr") d[who]=1; else r[who]=1}
       END {n=0; for (w in r) if (!(w in d)) {print "  " s " REG-ONLY: " w; n++}
            if (n==0) print "  " s " (none — every registering library also deregisters)"}' \
    "$ROOT/$s/run.log" 2>/dev/null
done
