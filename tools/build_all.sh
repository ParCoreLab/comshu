#!/usr/bin/env bash
# Build every configuration this machine can build — with tests ON — and run
# the tests. The gate that did not exist.
#
# Why it has to exist: `tests/` stopped compiling at 4dd6a69 (2026-07-27, the
# engine C ABI) and nobody found out for 39 commits. Not because anyone ignored
# a failure — because the failure was never produced. The only local build tree
# went stale on 2026-07-15, tests/hpc/run_on_mn5.sh configures *both* of its
# build dirs with -DPCOMM_BUILD_TESTS=OFF, ctest appears nowhere in the HPC
# pipeline, and there is no CI. Every build that ran was a build that excluded
# the broken files by construction.
#
# So the rule this script enforces is: the thing you did not build is the thing
# that is broken. It builds each configuration in its own tree, keeps going
# after a failure so you get the whole picture rather than the first casualty,
# and is **loud about what it skipped** — a skipped configuration reported
# quietly is how this repo got here in the first place.
#
#   tools/build_all.sh              # incremental, all buildable configs + tests
#   tools/build_all.sh --clean      # wipe the trees first (do this before a release)
#   tools/build_all.sh --quick      # core config only — what the pre-commit hook runs
#
# It does NOT run the HPC suite: that needs MareNostrum 5 and ~25 min of queue.
# tests/hpc/run_on_mn5.sh is still the thing that proves the goldens hold.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TREES="$ROOT/build-all"
JOBS="$(nproc 2>/dev/null || echo 4)"

clean=0 quick=0
while [ $# -gt 0 ]; do
  case "$1" in
    --clean) clean=1 ;;
    --quick) quick=1 ;;
    -h|--help) sed -n '2,26p' "$0"; exit 0 ;;
    *) echo "unknown arg: $1" >&2; exit 2 ;;
  esac
  shift
done

[ "$clean" -eq 1 ] && rm -rf "$TREES"
mkdir -p "$TREES"

bold() { printf '\033[1m%s\033[0m\n' "$*"; }
failures=() skips=()

# Build one configuration and run its tests. Never aborts the script: one broken
# provider must not hide the state of the others.
run_config() {   # <name> <cmake args...>
  local name="$1"; shift
  local dir="$TREES/$name" log="$TREES/$name.log"
  bold "== $name"
  if ! cmake -S "$ROOT" -B "$dir" -DCMAKE_BUILD_TYPE=RelWithDebInfo \
             -DPCOMM_BUILD_TESTS=ON "$@" > "$log" 2>&1; then
    echo "   configure FAILED — $log"; tail -15 "$log" | sed 's/^/   /'
    failures+=("$name (configure)"); return
  fi
  if ! cmake --build "$dir" -j"$JOBS" >> "$log" 2>&1; then
    echo "   build FAILED — $log"
    grep -E "error:|Error" "$log" | head -15 | sed 's/^/   /'
    failures+=("$name (build)"); return
  fi
  echo "   build ok"
  if ! (cd "$dir" && ctest --output-on-failure >> "$log" 2>&1); then
    echo "   tests FAILED — $log"
    sed -n '/The following tests FAILED/,$p' "$log" | head -20 | sed 's/^/   /'
    failures+=("$name (tests)"); return
  fi
  local n
  n=$(grep -oE "tests passed, [0-9]+ tests failed out of [0-9]+" "$log" | tail -1 |
      grep -oE "out of [0-9]+" | grep -oE "[0-9]+")
  echo "   tests ok (${n:-?} tests)"
}

# --- what can this machine actually build? ----------------------------------
# Probed, not assumed, and every "no" is reported at the end rather than being
# quietly not-done.
have_ibverbs=0
if [ -f /usr/include/infiniband/verbs.h ] || ldconfig -p 2>/dev/null | grep -q libibverbs; then
  have_ibverbs=1
else
  skips+=("ibverbs provider — no libibverbs (headers or shared lib) on this host")
fi

have_cuda=0
NVBIT="$ROOT/gpu-backends/nvbit/nvbit-1.7.7.1"
if ! command -v nvcc >/dev/null 2>&1; then
  skips+=("gpumem provider — no nvcc on PATH")
elif [ ! -d "$NVBIT" ]; then
  skips+=("gpumem provider — NVBit not unpacked at gpu-backends/nvbit/nvbit-1.7.7.1")
else
  # nvcc existing is not nvcc working: it rejects a host gcc newer than the CUDA
  # release knows about, which is the case on a rolling distro. Compile
  # something trivial and believe the result, so a host that cannot build CUDA
  # reports *why* it skipped instead of failing as though the code were broken.
  probe="$(mktemp -d)"
  echo '__global__ void k(){} int main(){return 0;}' > "$probe/probe.cu"
  if nvcc -c "$probe/probe.cu" -o "$probe/probe.o" > "$probe/err" 2>&1; then
    have_cuda=1
  else
    skips+=("gpumem provider — nvcc cannot compile here: $(grep -m1 -oE '#error[^"]*|error:.*' "$probe/err" | head -c 120)")
  fi
  rm -rf "$probe"
fi

# --- the configurations -----------------------------------------------------
# core is the one that must always work: header-only lib, engine, the pcomm binary,
# the launcher, every test, and the schema checks.
run_config core

if [ "$quick" -eq 0 ]; then
  [ "$have_ibverbs" -eq 1 ] && run_config ibverbs -DPCOMM_PROVIDER_IBVERBS=ON
  [ "$have_cuda" -eq 1 ] && run_config gpumem -DPCOMM_PROVIDER_GPUMEM=ON
  # Both providers in one process is the coexistence case the shared engine
  # exists for, so build it whenever both halves are available.
  if [ "$have_ibverbs" -eq 1 ] && [ "$have_cuda" -eq 1 ]; then
    run_config both -DPCOMM_PROVIDER_IBVERBS=ON -DPCOMM_PROVIDER_GPUMEM=ON
  fi
else
  skips+=("--quick: provider configurations not built")
fi

# --- verdict ----------------------------------------------------------------
echo
if [ ${#skips[@]} -gt 0 ]; then
  bold "SKIPPED (not built here — these are exactly where breakage hides):"
  for s in "${skips[@]}"; do echo "  - $s"; done
  echo "  Providers are built and the goldens checked on MN5:  tests/hpc/run_on_mn5.sh"
fi

if [ ${#failures[@]} -gt 0 ]; then
  echo
  bold "FAIL:"
  for f in "${failures[@]}"; do echo "  - $f"; done
  exit 1
fi
echo
bold "PASS — every configuration this host can build compiles, and its tests pass"
