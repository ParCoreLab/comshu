#!/bin/bash
# run_on_mn5.sh — one command, from a checkout of this repo: sync it to
# MareNostrum 5, build everything (tools + ibverbs + gpumem/NVBit), run the whole
# integration suite, and print the board. Blocks until every case has a verdict.
#
#   tests/hpc/run_on_mn5.sh                    # sync + build + run all + check
#   tests/hpc/run_on_mn5.sh --no-build         # skip the rebuild
#   tests/hpc/run_on_mn5.sh --clean            # wipe remote build dirs first
#   tests/hpc/run_on_mn5.sh --cases "01 03"    # a subset (by NN_ prefix or name)
#   tests/hpc/run_on_mn5.sh --quick            # skip 05 (~440 s of NVBit cost)
#   tests/hpc/run_on_mn5.sh --per-case         # one job per case (old behaviour)
#   tests/hpc/run_on_mn5.sh --check-only       # just re-aggregate existing results
#   tests/hpc/run_on_mn5.sh --rsync            # sync uncommitted work (see below)
#
# Sync is over git: this pushes HEAD to the bare repo on the remote and the
# remote tree (a clone of it) resets to it, so every board is attributable to a
# commit. A dirty working tree is refused; --rsync copies it as-is instead.
#
# Config via env (defaults are the ParCore MN5 account):
#   PCOMM_REMOTE=koc296797@glogin1.bsc.es     ssh target (key auth assumed)
#   PCOMM_REMOTE_DIR=proj/pcomm               clone on the remote (rel. to $HOME)
#   PCOMM_BARE=kefah/git/pcomm                bare repo it clones from
#   PCOMM_OUT_ROOT=$HOME/pcomm-suite          where captures land (remote)
#
# Notes: the acc_debug QoS allows one submitted job at a time. By default every
# case now runs inside ONE allocation (suite/all_in_one.sbatch), so the run pays
# one queue wait instead of one per case — that scheduling overhead, not the
# workloads, was most of the old ~25-30 min. `--per-case` restores the old
# job-per-case loop, which is what you want when isolating a single case.
# Exit code is run_suite's: 0 when all PASS/XFAIL, non-zero on any real FAIL or
# missing result. XFAIL does not fail the run.
#
# Every verdict is pinned to the job id submitted by THIS run, via a manifest at
# $PCOMM_OUT_ROOT/.last-run.manifest — so a case whose job never produced output
# reports "no result" instead of quietly inheriting the previous run's PASS.
# --check-only re-reads that manifest, and so re-checks that same run.
set -uo pipefail

REMOTE="${PCOMM_REMOTE:-koc296797@glogin1.bsc.es}"
RDIR="${PCOMM_REMOTE_DIR:-proj/pcomm}"
# The bare repo the remote tree clones from, and the URL this machine pushes to.
# Both name the same repository, from the two sides of the ssh hop.
PCOMM_BARE="${PCOMM_BARE:-kefah/git/pcomm}"
PCOMM_REMOTE_GIT="${PCOMM_REMOTE_GIT:-$REMOTE:$PCOMM_BARE}"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# ConnectTimeout because the login nodes refuse connections often enough that
# this has cost two runs: without it the very first ssh (step 0, resolving
# $HOME) blocks forever on a dead login node, which looks exactly like a slow
# sync rather than an unreachable host. Fail in 20s and say so.
#
# The default moved alogin1 -> glogin1 on 2026-08-30. Both ACC login nodes went
# silent on 22/443/80 on 2026-08-29 and stayed that way, so the documented
# alogin1 -> alogin2 fallback was two names for the same outage. The GPP login
# nodes share the GPFS home, the Slurm controller and the `acc` partition, so
# they submit ACC jobs perfectly well — the only visible difference is that
# `sinfo` and `scontrol show node` answer "Access/permission denied" there,
# which looks like a wall and is not one: sbatch, squeue and sacct all work.
# glogin2 is the like-for-like fallback.
SSH="ssh -o BatchMode=yes -o ConnectTimeout=20"

do_build=1 do_clean=0 check_only=0 combined=1 quick=0 use_rsync=0
cases_filter=""
while [ $# -gt 0 ]; do
  case "$1" in
    --no-build)   do_build=0 ;;
    --clean)      do_clean=1 ;;
    --check-only) check_only=1; do_build=0 ;;
    --cases)      cases_filter="$2"; shift ;;
    --quick)      quick=1 ;;
    --per-case)   combined=0 ;;
    --rsync)      use_rsync=1 ;;
    -h|--help)    sed -n '2,33p' "$0"; exit 0 ;;
    *) echo "unknown arg: $1" >&2; exit 2 ;;
  esac
  shift
done

say() { printf '\n\033[1m== %s\033[0m\n' "$*"; }
rsh() { $SSH "$REMOTE" "bash -lc '$*'"; }   # login shell → `module` is available

# --- 0. resolve remote paths ------------------------------------------------
# These must be concrete here, not left as remote-side defaults: the job needs
# them exported through sbatch, and `check` needs to look in the same place.
RHOME=$($SSH "$REMOTE" 'echo $HOME' | tr -d '\r') \
  || { echo "cannot reach $REMOTE" >&2
       echo "  try the other GPP login node: PCOMM_REMOTE=${REMOTE%%@*}@glogin2.bsc.es $0 $*" >&2
       echo "  (alogin1/alogin2 have been refusing ssh since 2026-08-29; the GPP" >&2
       echo "   login nodes submit acc_debug jobs just as well — same home, same" >&2
       echo "   controller, same partition.)" >&2
       exit 1; }
case "$RDIR" in /*) PROOT="$RDIR" ;; *) PROOT="$RHOME/$RDIR" ;; esac
OUT_ROOT="${PCOMM_OUT_ROOT:-$RHOME/pcomm-suite}"
MANIFEST_REMOTE="$OUT_ROOT/.last-run.manifest"
MANIFEST_LOCAL=$(mktemp)
trap 'rm -f "$MANIFEST_LOCAL"' EXIT

# Wait until a job reaches a terminal state. The point of sacct here is that it
# answers for finished jobs too, so "no longer in squeue" and "the query broke"
# stay distinguishable — the old loop read any empty line as success, meaning a
# dropped ssh silently skipped the wait and moved on to the next case.
wait_job() {
  local jid="$1" st sq errs=0 misses=0
  while :; do
    if ! st=$(rsh "sacct -j $jid -X -n -P -o State 2>/dev/null" | head -1 | tr -d '[:space:]'); then
      errs=$((errs+1))
      [ "$errs" -ge 15 ] && { echo "  !! sacct query failed ${errs}x for $jid — giving up" >&2; return 1; }
      sleep 20; continue
    fi
    errs=0
    case "$st" in
      COMPLETED|FAILED|TIMEOUT|NODE_FAIL|OUT_OF_MEMORY|PREEMPTED|BOOT_FAIL|DEADLINE|REVOKED|CANCELLED*)
        printf '\r  %s: %s            \n' "$jid" "$st"
        # TIMEOUT is how the xfail case reports its deadlock, so it is not an
        # error here — the verdict belongs to `check`, which reads result.json.
        return 0 ;;
      "")
        # sacct can lag for a few seconds after submit; believe squeue first.
        sq=$(rsh "squeue -h -j $jid -o %T 2>/dev/null" | tr -d '[:space:]') || sq=""
        if [ -n "$sq" ]; then misses=0; printf '\r  %s: %s        ' "$jid" "$sq"
        else
          misses=$((misses+1))
          [ "$misses" -ge 5 ] && { echo "  !! $jid unknown to both sacct and squeue" >&2; return 1; }
        fi ;;
      *) misses=0; printf '\r  %s: %s        ' "$jid" "$st" ;;
    esac
    sleep 20
  done
}

# --- 1. sync ----------------------------------------------------------------
# Over git, not rsync: the remote tree is a clone of the bare repo at
# $PCOMM_BARE, so what the suite runs is always a named commit. That matters
# because a verdict is evidence about a *version* — with rsync you could not
# say afterwards which tree produced a board, and pushing without rsyncing (or
# the reverse) silently tested the wrong thing.
#
# --rsync restores the old behaviour for iterating on uncommitted work; it is
# deliberately not the default, since a board from a tree nobody can name again
# is not a gate.
if [ "$check_only" -eq 0 ]; then
  if [ "$use_rsync" -eq 1 ]; then
    say "sync (rsync, working tree as-is) $REPO_ROOT → $REMOTE:$RDIR"
    $SSH "$REMOTE" "mkdir -p '$RDIR'"
    rsync -az --delete \
      --exclude '.git' --exclude 'build' --exclude 'build-*' \
      --exclude '.cache' --exclude '*.o' --exclude '*.pcs' \
      "$REPO_ROOT"/ "$REMOTE:$RDIR"/
  else
    if ! git -C "$REPO_ROOT" diff --quiet HEAD 2>/dev/null; then
      echo "run_on_mn5: working tree has uncommitted changes." >&2
      echo "  commit them (the pre-commit hook builds + tests), or pass --rsync" >&2
      echo "  to sync the tree as-is without a commit." >&2
      exit 2
    fi
    branch=$(git -C "$REPO_ROOT" rev-parse --abbrev-ref HEAD)
    head=$(git -C "$REPO_ROOT" rev-parse --short HEAD)
    # Detached HEAD — a `git worktree` pinned to a commit, or a bisect. That is
    # exactly how you isolate a regression ("did case 07 pass before this?"), so
    # it has to work: `rev-parse --abbrev-ref HEAD` answers the literal string
    # "HEAD" there, which is not a pushable refspec and made the push fail with
    # "not a full refname" *after* claiming it was syncing. Deploy such runs
    # under one throwaway ref instead, force-pushed because it moves anywhere.
    if [ "$branch" = "HEAD" ]; then
      branch=_detached
      refspec="HEAD:refs/heads/_detached"
      say "sync (git) detached@$head → $REMOTE:$RDIR (as $branch)"
    else
      refspec="$branch"
      say "sync (git) $branch@$head → $REMOTE:$RDIR"
    fi
    git -C "$REPO_ROOT" push --quiet --force "$PCOMM_REMOTE_GIT" "$refspec" \
      || { echo "push to $PCOMM_REMOTE_GIT failed" >&2; exit 1; }
    # Hard reset, not merge: the remote tree is a deploy target, never edited
    # there. Untracked build dirs survive (they are gitignored).
    rsh "cd $RDIR && git fetch --quiet origin && git reset --quiet --hard origin/$branch && git rev-parse --short HEAD" \
      || { echo "remote pull failed — is $RDIR a clone of $PCOMM_BARE?" >&2; exit 1; }
  fi
fi

# --- 2. build ---------------------------------------------------------------
if [ "$do_clean" -eq 1 ]; then
  say "clean remote build dirs"; rsh "cd $RDIR && rm -rf build build-gpumem build-ucx"
fi
if [ "$do_build" -eq 1 ]; then
  # DuckDB, pushed rather than fetched. MN5 has no outbound network, so
  # tools/get_duckdb.sh cannot run there — but the library is just a file, and
  # this end has already downloaded it. Without it the remote build has no
  # --parquet, no `link` and no `sql`, which means the suite's assertions can
  # never see the derived layer (sql_check.sh explains why that matters).
  #
  # .tools/ is gitignored, so the git sync above does NOT carry it; that is why
  # this is a separate rsync and not a consequence of the push. It was already
  # present on MN5 by some earlier manual step, which is exactly the kind of
  # undeclared dependency that works until someone runs --clean on a new account.
  # TWO gitignored things are needed, not one. Enabling PCOMM_DUCKDB also enters
  # pcomm-ui (CMakeLists: the UI is a DuckDB consumer by definition), and that
  # subdirectory refuses to configure without a built ui/dist. Both are untracked,
  # so neither rides along with the git push above and both must be sent here.
  DUCK_LOCAL="$REPO_ROOT/.tools/duckdb"
  if [ -e "$DUCK_LOCAL/libduckdb.so" ] && [ -e "$REPO_ROOT/ui/dist/index.html" ]; then
    if ! $SSH "$REMOTE" "test -e $PROOT/.tools/duckdb/libduckdb.so -a -e $PROOT/ui/dist/index.html"; then
      say "ship libduckdb + ui/dist to the remote (no network there to fetch them)"
      $SSH "$REMOTE" "mkdir -p $PROOT/.tools/duckdb $PROOT/ui/dist"
      rsync -az -e "$SSH" "$DUCK_LOCAL/libduckdb.so" "$DUCK_LOCAL/duckdb.h" "$DUCK_LOCAL/duckdb.hpp" \
            "$REMOTE:$PROOT/.tools/duckdb/" &&
      rsync -az --delete -e "$SSH" "$REPO_ROOT/ui/dist/" "$REMOTE:$PROOT/ui/dist/" \
        || echo "  (push failed — building without DuckDB; derived-layer assertions will skip)" >&2
    fi
  else
    [ -e "$DUCK_LOCAL/libduckdb.so" ] \
      || echo "  (no local .tools/duckdb — run tools/get_duckdb.sh for derived-layer assertions)" >&2
    [ -e "$REPO_ROOT/ui/dist/index.html" ] \
      || echo "  (no local ui/dist — run 'pnpm build' in ui/ for derived-layer assertions)" >&2
  fi
  # PCOMM_DUCKDB follows what is actually there, so a missing library degrades to
  # the old CSV-only behaviour instead of failing the configure.
  # BOTH must be present remotely, for the reason above: libduckdb alone turns
  # PCOMM_DUCKDB on, which pulls in pcomm-ui, which then fails the configure for
  # want of ui/dist — turning a missing optional into a broken build.
  DUCK_ON=$($SSH "$REMOTE" "test -e $PROOT/.tools/duckdb/libduckdb.so -a -e $PROOT/ui/dist/index.html && echo ON || echo OFF")

  # EVERY `cmake --build` BELOW IS CHECKED TOO, and that is a separate bug from
  # the module one. Each was written `cmake --build ... 2>&1 | tail -2`, and a
  # pipeline's status is its LAST stage — `tail` — which always succeeds. The
  # remote side is `bash -lc`, which does not inherit this script's `pipefail`,
  # so a failed build returned 0, the `&&` chain continued, and `cp` copied a
  # STALE .so into build/ with a fresh mtime. Caught on 2026-08-30: the nccl
  # step printed `gmake: *** No rule to make target 'nccl_trace'` and the run
  # went on to submit, exactly the "green suite reporting on a day-old binary"
  # hazard the gpumem step's own comment warns about, in a step that had no
  # guard for it.
  #
  # `pipefail` would have been the small fix and is the wrong one here: it would
  # also apply to `. tests/hpc/suite/common.sh` in the ucx and nccl steps, whose
  # probe pipelines are allowed to fail. Capturing to a variable scopes the
  # check to the build alone, and gives the failure 25 lines of context instead
  # of the 2 that `tail -2` leaves when it is the error you actually need.
  #
  # EVERY `module load` BELOW IS CHECKED, and it did not used to be. They were
  # all written `module load a b c >/dev/null 2>&1`, which throws away an exit
  # code Lmod sets correctly (rc=1 when any name is unknown) — and Lmod aborts
  # the WHOLE line on the first unknown name, so `cmake` silently never loaded.
  # The failure then surfaced three lines later as `cmake: command not found`,
  # which reads as a broken login node rather than as one bad module name. That
  # cost a cycle on 2026-08-30 when the run moved to the GPP login nodes, where
  # gcc/11.4.0 does not exist. Swallow the chatter, never the status.
  say "build tools + ibverbs provider (gcc-13.2, DuckDB=$DUCK_ON)"
  rsh "module load gcc/13.2.0 cmake >/dev/null 2>&1 || { echo \"module load gcc/13.2.0 cmake FAILED — run it by hand to see which name is unknown here\" >&2; exit 1; }; cd $RDIR &&
       cmake -B build -DPCOMM_BUILD_TESTS=OFF -DPCOMM_PROVIDER_IBVERBS=ON -DPCOMM_PROVIDER_MLX5DV=ON -DPCOMM_DUCKDB=$DUCK_ON >/dev/null &&
       bout=\$(cmake --build build -j 2>&1) || { echo \"\$bout\" | tail -25 >&2; exit 1; }; echo \"\$bout\" | tail -2" \
    || { echo 'tools/ibverbs build FAILED' >&2
         echo "  if the output above mentions variables that require the cache to be" >&2
         echo "  deleted, a previous configure picked a different compiler (nvc++ via" >&2
         echo "  nvidia-hpc-sdk). cmake resets the cache mid-build and that invocation" >&2
         echo "  finds no target. Re-run with --clean." >&2
         exit 1; }

  say "build gpumem provider (NVBit, gcc-11.4 host, CUDA C++20, sm_90)"
  # CMAKE_CUDA_COMPILER is pinned to the *versioned* 12.9 tree on purpose. The
  # SDK ships $SDK/compilers/bin/nvcc (CUDA 11.8, no c++20) and that is what
  # PATH resolves, so an unpinned configure silently downgrades the toolchain
  # and dies on -std=c++20. $SDK/cuda/bin/nvcc is 12.9 but its root lacks the
  # static libs, giving "cannot find -lcudadevrt"; only $SDK/cuda/12.9/bin/nvcc
  # has both. This used to work only because an old CMakeCache happened to hold
  # the right compiler — one `rm -rf build-gpumem` turned that into a blocker.
  #
  # The touch is a blunt fix for a real hazard: the CUDA target does not track
  # libpcomm/include headers, so editing one leaves gpumem_trace stale while
  # make reports success, and the stale .so then gets copied into build/ with a
  # fresh mtime. A green suite was reporting on a day-old binary.
  #
  # The gcc is chosen from a list rather than pinned, because gcc/11.4.0 exists
  # only in the ACC module tree and this script now defaults to a GPP login
  # node. The SDK path is left as the ACC one deliberately: /apps is shared
  # GPFS, the ACC tree is readable from GPP, and `module load nvidia-hpc-sdk`
  # sets NVHPC_ROOT to that same ACC path from either side — there is no GPP
  # copy to point at. If the fallback gcc is used the step says so, because
  # NVBit is a prebuilt object and the host compiler is not a free choice.
  rsh "for g in gcc/11.4.0 gcc/12.3.0; do module load \$g >/dev/null 2>&1 && { GCCMOD=\$g; break; }; done;
       [ -n \"\${GCCMOD:-}\" ] || { echo \"no usable gcc module for the NVBit host compiler\" >&2; exit 1; };
       [ \"\$GCCMOD\" = gcc/11.4.0 ] || echo \"  (gcc/11.4.0 not in this module tree; using \$GCCMOD)\" >&2;
       module load nvidia-hpc-sdk/25.7 cmake >/dev/null 2>&1 || { echo \"module load nvidia-hpc-sdk/25.7 cmake FAILED\" >&2; exit 1; }; cd $RDIR &&
       SDK=/apps/ACC/NVIDIA-HPC-SDK/25.7/Linux_x86_64/25.7 &&
       cmake -B build-gpumem -DPCOMM_BUILD_TESTS=OFF -DPCOMM_PROVIDER_IBVERBS=OFF -DPCOMM_PROVIDER_GPUMEM=ON -DCMAKE_CUDA_ARCHITECTURES=90 -DCMAKE_CUDA_HOST_COMPILER=g++ -DCMAKE_CUDA_COMPILER=\$SDK/cuda/12.9/bin/nvcc >/dev/null &&
       touch gpu-backends/nvbit/nvbit_backend.cu gpu-backends/nvbit/inject_funcs.cu providers/gpumem/gpumem_provider.cpp &&
       bout=\$(cmake --build build-gpumem --target gpumem_trace -j 2>&1) || { echo \"\$bout\" | tail -25 >&2; exit 1; }; echo \"\$bout\" | tail -2 &&
       mkdir -p build/providers/gpumem &&
       cp build-gpumem/providers/gpumem/libgpumem_trace.so build/providers/gpumem/ &&
       ls -la build/providers/*/*.so" \
    || { echo 'gpumem build FAILED' >&2
         echo "  if the output above mentions variables that require the cache to be" >&2
         echo "  deleted, a previous configure picked a different compiler (nvc++ via" >&2
         echo "  nvidia-hpc-sdk). cmake resets the cache mid-build and that invocation" >&2
         echo "  finds no target. Re-run with --clean." >&2
         exit 1; }

  # UCX, third and separate for the same reason gpumem is: it needs a toolchain
  # the other two do not. libucp lives in the *versioned* HPC-X tree, not under
  # the comm_libs/mpi symlink that PATH resolves — a configure pointed at the
  # symlink finds headers and no library. Paths verified by the probe this case
  # was promoted from (tests/hpc/probes/ucx_api_coverage/provider.sbatch).
  #
  # CMAKE_C/CXX_COMPILER ARE NOT OPTIONAL HERE, and dropping them is how this
  # step failed first: `module load nvidia-hpc-sdk` (needed for the HPC-X tree)
  # also puts nvc++ on PATH and exports CC/CXX, so cmake silently configures with
  # the NVIDIA compiler. The failure surfaces inside libstdc++ —
  # "identifier __is_convertible is undefined" in <type_traits> — which reads as
  # a broken toolchain rather than as the wrong compiler being chosen. Same
  # hazard common.sh already documents for CC in the CUDA cases.
  #
  # The provider is built OFF by default everywhere else and would otherwise
  # never appear in a capture: before this step, every ucx_* table in the whole
  # corpus was 0 rows, in every capture, while 13 record types and 29 interposed
  # doors sat in the tree fully tested and never exercised on a cluster.
  # BOTH UCX providers. ucx_deep is the UCT layer, and this is the machine it
  # exists for: on HPC-X/mlx5 it is the only provider that sees the data plane
  # at all, because ib_post_send and mlx5_device_send are both empty there. Its
  # local gate runs on the self transport and proves the record layer; rc_mlx5,
  # HW tag matching and rendezvous are only reachable here.
  say "build ucx + ucx_deep providers (UCP doors and the UCT ops table, gcc-13.2)"
  rsh "module load gcc/13.2.0 cmake nvidia-hpc-sdk/25.7 >/dev/null 2>&1 || { echo \"module load gcc/13.2.0 cmake nvidia-hpc-sdk/25.7 FAILED\" >&2; exit 1; }; cd $RDIR &&
       . tests/hpc/suite/common.sh &&
       UCX=\$(pcomm_find_ucx) &&
       echo \"  ucx: \$UCX\" &&
       cmake -B build-ucx -DPCOMM_BUILD_TESTS=OFF -DPCOMM_PROVIDER_IBVERBS=OFF -DPCOMM_PROVIDER_UCX=ON -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ -DUCX_UCP_LIB=\$UCX/lib/libucp.so -DUCX_INCLUDE_DIR=\$UCX/include >/dev/null &&
       bout=\$(cmake --build build-ucx --target ucx_trace ucx_deep_trace -j 2>&1) || { echo \"\$bout\" | tail -25 >&2; exit 1; }; echo \"\$bout\" | tail -2 &&
       mkdir -p build/providers/ucx build/providers/ucx_deep &&
       cp build-ucx/providers/ucx/libucx_trace.so build/providers/ucx/ &&
       cp build-ucx/providers/ucx_deep/libucx_deep_trace.so build/providers/ucx_deep/ &&
       ls -la build/providers/*/*.so" \
    || { echo 'ucx build FAILED' >&2
         echo "  if the output above mentions variables that require the cache to be" >&2
         echo "  deleted, a previous configure picked a different compiler (nvc++ via" >&2
         echo "  nvidia-hpc-sdk). cmake resets the cache mid-build and that invocation" >&2
         echo "  finds no target. Re-run with --clean." >&2
         exit 1; }

  # NCCL, fourth. Headers only — this one links against no libnccl, because it
  # reaches NCCL through the pnccl* profiling symbols the application has
  # already loaded (see providers/nccl/nccl_hooks.hpp). So the only thing this
  # step needs to find is nccl.h and cuda_runtime_api.h, both of which live in
  # the SDK that NCCL_HOME already resolves for the cases themselves.
  #
  # gcc-13.2 like ucx and for the same reason: the engine's libstdc++.
  say "build nccl provider (PROFAPI doors, gcc-13.2)"
  rsh "module load gcc/13.2.0 cmake nvidia-hpc-sdk/25.7 >/dev/null 2>&1 || { echo \"module load gcc/13.2.0 cmake nvidia-hpc-sdk/25.7 FAILED\" >&2; exit 1; }; cd $RDIR &&
       . tests/hpc/suite/common.sh &&
       echo \"  nccl: \$NCCL_HOME\" &&
       cmake -B build-nccl -DPCOMM_BUILD_TESTS=OFF -DPCOMM_PROVIDER_IBVERBS=OFF -DPCOMM_PROVIDER_NCCL=ON -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ -DNCCL_INCLUDE_DIR=\$NCCL_HOME/include -DCUDA_RT_INCLUDE_DIR=\$PCOMM_SDK/cuda/include >/dev/null &&
       bout=\$(cmake --build build-nccl --target nccl_trace -j 2>&1) || { echo \"\$bout\" | tail -25 >&2; exit 1; }; echo \"\$bout\" | tail -2 &&
       mkdir -p build/providers/nccl &&
       cp build-nccl/providers/nccl/libnccl_trace.so build/providers/nccl/ &&
       ls -la build/providers/*/*.so" \
    || { echo 'nccl build FAILED' >&2
         echo "  if the output above mentions variables that require the cache to be" >&2
         echo "  deleted, a previous configure picked a different compiler (nvc++ via" >&2
         echo "  nvidia-hpc-sdk). cmake resets the cache mid-build and that invocation" >&2
         echo "  finds no target. Re-run with --clean." >&2
         exit 1; }
fi

# --- 3. run the cases --------------------------------------------------------
# Default: one allocation, all cases inside it. acc_debug allows one *submitted*
# job at a time, which is why a job per case cost a queue wait per case.
all_cases() { rsh "cd $RDIR && for d in tests/hpc/cases/*/; do basename \$d; done"; }
CASES=$(all_cases)
if [ -n "$cases_filter" ]; then
  sel=""
  for f in $cases_filter; do
    m=$(echo "$CASES" | grep -E "^${f}(_|\$)") \
      || { echo "no case matches '$f'; known: $(echo $CASES)" >&2; exit 2; }
    sel="$sel $m"
  done
  CASES="$sel"
fi

# --quick drops case 05, whose ~440 s is fixed NVBit module-processing cost and
# is most of a full run. It is opt-in and reported as SKIP, never as a pass:
# dropping it stops checking the NCCL-under-NVBit path, which is a real loss.
SKIPPED_CASES=""
if [ "$quick" -eq 1 ]; then
  keep=""
  for c in $CASES; do
    case "$c" in 05_*) SKIPPED_CASES="$SKIPPED_CASES $c" ;; *) keep="$keep $c" ;; esac
  done
  CASES="$keep"
  [ -n "$SKIPPED_CASES" ] && echo "  --quick: skipping$SKIPPED_CASES"
fi
CASES=$(echo $CASES)   # normalise whitespace; it is passed through sbatch --export

run_rc=0
# Pushed after every submit rather than once at the end, so interrupting a run
# still leaves --check-only reading the right jobs.
push_manifest() {
  $SSH "$REMOTE" "mkdir -p '$OUT_ROOT' && cat > '$MANIFEST_REMOTE'" < "$MANIFEST_LOCAL" \
    || { echo "could not write run manifest" >&2; exit 1; }
}

# Every skipped case is named in the manifest, so `check` reports SKIP rather
# than falling back to newest-wins and inheriting an older run's PASS.
record_skips() { for c in $SKIPPED_CASES; do printf '%s SKIPPED\n' "$c" >> "$MANIFEST_LOCAL"; done; }

wall_t0=$(date +%s)
if [ "$check_only" -eq 0 ] && [ "$combined" -eq 1 ]; then
  : > "$MANIFEST_LOCAL"; record_skips; push_manifest
  # The case list goes over as a FILE, not as an --export value. Both separators
  # available there are taken: --export splits on commas, and a space would be
  # word-split by the `bash -lc '...'` wrapper rsh uses — which would silently
  # hand sbatch a stray argument instead of failing.
  printf '%s\n' $CASES | $SSH "$REMOTE" "mkdir -p '$OUT_ROOT' && cat > '$OUT_ROOT/.run-cases'" \
    || { echo "could not write the case list" >&2; exit 1; }
  # Ask for only as much walltime as the case set needs: a shorter request is
  # scheduled sooner on a debug QoS, and queue wait is the thing this is all
  # about. Case 05 alone is ~7.5 min; everything else together is under two.
  case " $CASES " in *" 05_"*) tlimit="00:40:00" ;; *) tlimit="00:15:00" ;; esac
  say "submit + wait: one allocation for [$CASES]"
  JID=$(rsh "cd $RDIR && sbatch --parsable --time=$tlimit --export=ALL,PCOMM_ROOT=$PROOT,PCOMM_OUT_ROOT=$OUT_ROOT,PCOMM_CASES_FILE=$OUT_ROOT/.run-cases tests/hpc/suite/all_in_one.sbatch" \
        | tr -d '[:space:]') && JID="${JID%%;*}" && [ -n "$JID" ] \
    || { echo "  !! submit FAILED" >&2
         for c in $CASES; do printf '%s %s\n' "$c" SUBMIT_FAILED >> "$MANIFEST_LOCAL"; done
         push_manifest; run_rc=1; JID=""; }
  if [ -n "$JID" ]; then
    echo "  job $JID  (walltime requested: $tlimit)"
    # Same job id for every case: each case still writes its own
    # <label>-<jobid>.csv, so a case the allocation never reached leaves no
    # result dir and reads as red rather than as a pass.
    for c in $CASES; do printf '%s %s\n' "$c" "$JID" >> "$MANIFEST_LOCAL"; done
    push_manifest
    wait_job "$JID" || run_rc=1
  fi
elif [ "$check_only" -eq 0 ]; then
  : > "$MANIFEST_LOCAL"; record_skips; push_manifest
  for c in $CASES; do
    say "submit + wait: $c"
    # PCOMM_ROOT/PCOMM_OUT_ROOT go through --export so the job honours the same
    # PCOMM_REMOTE_DIR / PCOMM_OUT_ROOT this driver was given, instead of
    # falling back to common.sh's $HOME defaults.
    if ! jid=$(rsh "cd $RDIR && sbatch --parsable --export=ALL,PCOMM_ROOT=$PROOT,PCOMM_OUT_ROOT=$OUT_ROOT tests/hpc/cases/$c/job.sbatch"); then
      echo "  !! submit FAILED: $c" >&2
      # Record it anyway: a case missing from the manifest would fall back to
      # newest-wins in `check` and could inherit an older run's PASS.
      printf '%s %s\n' "$c" SUBMIT_FAILED >> "$MANIFEST_LOCAL"; push_manifest; run_rc=1; continue
    fi
    jid=$(printf '%s' "$jid" | tr -d '[:space:]'); jid="${jid%%;*}"   # "123;cluster"
    if [ -z "$jid" ]; then
      echo "  !! no job id for $c" >&2
      printf '%s %s\n' "$c" SUBMIT_FAILED >> "$MANIFEST_LOCAL"; push_manifest; run_rc=1; continue
    fi
    echo "  job $jid"
    printf '%s %s\n' "$c" "$jid" >> "$MANIFEST_LOCAL"; push_manifest
    wait_job "$jid" || run_rc=1
  done
fi

# --- 4. timing --------------------------------------------------------------
# Queue wait and job time are reported separately on purpose: queue wait is not
# ours to improve, so folding the two together makes any change to the suite
# itself unmeasurable. Submit→Start is the wait, Start→End is the run.
if [ "$check_only" -eq 0 ] && [ -n "${JID:-}" ]; then
  say "timing"
  wall=$(( $(date +%s) - wall_t0 ))
  t=$(rsh "sacct -j $JID -X -n -P -o Submit,Start,End,Elapsed 2>/dev/null" | head -1 | tr -d '\r')
  IFS='|' read -r t_sub t_start t_end t_elapsed <<< "$t"
  # The epoch conversion runs on the CLUSTER, not here. `date -d` is a GNU
  # extension and this script is driven from a FreeBSD workstation, where it
  # exits 1 with a usage block and left the whole timing section printing two
  # usage dumps and `arithmetic syntax error: operand expected` on every run.
  # Remote is also the more correct side: these are Slurm's own timestamps in
  # the cluster's timezone, and converting them anywhere else invites an offset.
  # Slurm formats them as 2026-08-30T11:07:12 — no spaces, so no quoting needed
  # inside the rsh wrapper, which cannot carry a single quote.
  if [ -n "${t_start:-}" ] && [ "$t_start" != "Unknown" ]; then
    qs=$(rsh "echo \$(( \$(date -d $t_start +%s) - \$(date -d $t_sub +%s) ))" | tr -d '\r')
    printf '  queue wait : %s s\n  job time   : %s (%s → %s)\n' "${qs:-?}" "$t_elapsed" "$t_start" "$t_end"
  fi
  printf '  wall clock : %s s (sync + build + queue + run)\n' "$wall"
  echo "  per-case breakdown: $OUT_ROOT/timings-$JID.txt"
  rsh "cat $OUT_ROOT/timings-$JID.txt 2>/dev/null" | sed 's/^/    /'
fi

# --- 5. board ---------------------------------------------------------------
say "suite board"
echo "  results: $OUT_ROOT   manifest: $MANIFEST_REMOTE"
# Captured rather than streamed, so the banner below can count what is on the
# board. Printed verbatim immediately after, so nothing is hidden by capturing.
board="$(rsh "cd $RDIR && PCOMM_ROOT=$PROOT PCOMM_OUT_ROOT=$OUT_ROOT PCOMM_RUN_MANIFEST=$MANIFEST_REMOTE bash tests/hpc/suite/run_suite.sh check")"
rc=$?
printf '%s\n' "$board"
[ "$run_rc" -eq 0 ] || rc=$(( rc == 0 ? 1 : rc ))   # a bad submit/wait is a red run
echo
# Name the skips in the banner. "SUITE GREEN" over a board containing a case
# that never ran overstates what was tested — case 09 skips on MN5 for want of
# IBGDA, and a green banner should not quietly absorb that.
n_skip=$(printf '%s\n' "$board" | grep -c ' SKIP ')
if [ $rc -eq 0 ]; then
  echo "SUITE GREEN (PASS/XFAIL)"
  [ "$n_skip" -gt 0 ] && echo "  note: $n_skip case(s) SKIPPED on this host — green here covers less than the full suite"
else
  echo "SUITE RED (exit $rc)"
fi
exit $rc
