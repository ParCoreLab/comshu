#!/usr/bin/env bash
# run_on_a100.sh — one command, from a checkout of this repo: sync it to the
# A100 box, build the mlx5dv provider, run the IBGDA case, print the verdict.
#
#   tests/hpc/run_on_a100.sh                 # sync + build + run case 09
#   tests/hpc/run_on_a100.sh --no-build      # skip the rebuild
#   tests/hpc/run_on_a100.sh --cases "09"    # a subset (by NN_ prefix or name)
#
# The companion to run_on_mn5.sh, and separate from it because the two machines
# are disjoint in what they can test:
#
#   MN5   has SLURM and many nodes; CANNOT run IBGDA (no nvidia_peermem, no
#         PeerMappingOverride) — case 09 self-skips there.
#   a100  has nvidia_peermem + PeerMappingOverride=1 and a ConnectX-5, so it CAN
#         run IBGDA; has no scheduler and one node.
#
# So this is not a fallback for MN5 being busy. It is the only place
# device-initiated networking can be observed at all, and MN5 is the only place
# multi-node behaviour can. Neither board subsumes the other.
#
# Config via env:
#   PCOMM_A100=missa18@a100        ssh target (may be a ProxyJump alias)
#   PCOMM_SSH_OPTS=               extra ssh options, e.g.
#                                  '-o ProxyJump=mktips@100.64.0.8'.
#                                  For hosts where ~/.ssh/config cannot carry the
#                                  jump: on simurgh-base it is a read-only
#                                  symlink into the nix store, and tailnet
#                                  MagicDNS names do not resolve there, so the
#                                  configured `ProxyJump university-base` fails
#                                  and cannot be fixed locally.
#   PCOMM_A100_DIR=/DATA/$USER/pcomm   checkout on the remote. NOT $HOME —
#                                  the A100 box's root filesystem runs at 98%
#                                  and other people's jobs live there.
set -uo pipefail

REMOTE="${PCOMM_A100:-missa18@a100}"
RDIR="${PCOMM_A100_DIR:-/DATA/missa18/pcomm}"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SSH="ssh -o BatchMode=yes -o ConnectTimeout=20 ${PCOMM_SSH_OPTS:-}"

do_build=1
cases_filter="09"
while [ $# -gt 0 ]; do
  case "$1" in
    --no-build) do_build=0 ;;
    --cases)    cases_filter="$2"; shift ;;
    -h|--help)  sed -n '2,25p' "$0"; exit 0 ;;
    *) echo "run_on_a100.sh: unknown argument: $1" >&2; exit 2 ;;
  esac
  shift
done

rsh() { $SSH "$REMOTE" "bash -lc '$1'"; }

$SSH "$REMOTE" true 2>/dev/null || {
  echo "run_on_a100.sh: cannot reach $REMOTE." >&2
  echo "  If it needs a jump host, put it in ~/.ssh/config, or pass it as" >&2
  echo "  PCOMM_SSH_OPTS='-o ProxyJump=user@host' when that file is not yours" >&2
  echo "  to edit. This script does not guess one. (Overriding a configured" >&2
  echo "  ProxyJump on the command line is what made the first attempt fail" >&2
  echo "  with 'Host key verification failed'.)" >&2
  exit 1; }

# ---- provenance -----------------------------------------------------------
# Which commit is this board about? run_on_mn5.sh answers that by pushing HEAD
# over git and refusing a dirty tree. This runner rsyncs from whatever the local
# checkout currently holds — and when that checkout is itself fed by syncthing,
# files arrive INDEPENDENTLY. A run has already executed a mixed tree: the
# provider .cpp had landed and the .sql assertion beside it in the same commit
# had not, so a fixed provider was judged by a stale assertion and the case went
# red for a reason that no longer existed.
#
# So the commit is printed with every board, a dirty tree is called out, and
# PCOMM_EXPECT_COMMIT lets a caller demand the tree it meant to test.
#
# THAT CHECK IS ABOUT THE LOCAL CHECKOUT AND NOTHING ELSE — it compares the tree
# this script is reading to the tree the caller meant, and says nothing about
# the binaries that will actually produce the capture. The second half is below:
# the same id is stamped into the build and re-checked inside every case, by
# suite/asserts/build_is_the_boards_commit.sql, against what the capture itself
# reports having been built from.
#
# `--short=12` matches cmake/write_build_id.cmake exactly. A caller's expected
# value may be any length, so that comparison stays prefix-tolerant; the one
# that reaches the assertion never is.
src_commit="$(git -C "$REPO_ROOT" rev-parse --short=12 HEAD 2>/dev/null || echo unknown)"
src_dirty="$(git -C "$REPO_ROOT" status --porcelain 2>/dev/null | head -1)"
echo "== source $src_commit${src_dirty:+  (DIRTY — this board is not attributable to a commit)}"
if [ -n "${PCOMM_EXPECT_COMMIT:-}" ] &&
   [ "${src_commit#$PCOMM_EXPECT_COMMIT}" = "$src_commit" ] &&
   [ "${PCOMM_EXPECT_COMMIT#$src_commit}" = "$PCOMM_EXPECT_COMMIT" ]; then
  echo "run_on_a100.sh: tree is at $src_commit, expected $PCOMM_EXPECT_COMMIT." >&2
  echo "  If this checkout is fed by syncthing, the files may still be arriving." >&2
  exit 1
fi

# The id the BUILD will stamp and the capture will be checked against. Tracked
# changes only, matching write_build_id.cmake's `--untracked-files=no`, so both
# sides agree on what dirty means; `$src_dirty` above is the looser human
# warning and includes untracked files.
#
# It has to be handed over explicitly because the sync below excludes `.git`:
# the remote holds a perfect copy of the source and no repository at all, so
# every build there has been stamping `commit=unknown` — on the one host where a
# hand-copied .so has already caused a false refutation. write_build_id.cmake
# has had the $PCOMM_COMMIT door for exactly this since it was written, and
# nothing walked through it.
build_commit="$src_commit"
git -C "$REPO_ROOT" diff --quiet HEAD 2>/dev/null || build_commit="$src_commit-dirty"
# Overrides whatever form the caller passed above: the assertion matches the
# stamped id exactly, so what reaches it must be the id this script computed,
# not a 7-character abbreviation of it that would fail against every capture.
export PCOMM_EXPECT_COMMIT="$build_commit"

# ---- sync -----------------------------------------------------------------
# rsync rather than the git round-trip run_on_mn5.sh uses: there is no bare repo
# on this host, and --delete is not optional — a stale check file left behind
# once produced a false failure on MN5 that took a re-run to explain.
echo "== sync -> $REMOTE:$RDIR"
rsh "mkdir -p $RDIR" || exit 1
rsync -az --delete --exclude build --exclude 'build-*' --exclude .git --exclude .tools \
      -e "$SSH" "$REPO_ROOT/" "$REMOTE:$RDIR/" || exit 1

# .tools is excluded above (it is 130 MB of archives and `--delete` would fight
# with anything already there), so DuckDB goes over as its own step and only
# when it is missing. It is what gives this host `--parquet` and `pcomm sql`,
# and therefore the derived-layer assertions — on the one machine that can run
# IBGDA at all, which is the only place several of them can ever be exercised.
DUCK_STATIC_LOCAL="$REPO_ROOT/.tools/duckdb-static"
DUCK_STATIC=OFF
if ls "$DUCK_STATIC_LOCAL"/*.a >/dev/null 2>&1; then
  if ! rsh "ls $RDIR/.tools/duckdb-static/*.a >/dev/null 2>&1"; then
    echo "== ship the DuckDB archive set (one time, ~130 MB)"
    rsh "mkdir -p $RDIR/.tools/duckdb-static"
    rsync -az -e "$SSH" "$DUCK_STATIC_LOCAL"/ "$REMOTE:$RDIR/.tools/duckdb-static/" \
      || echo "  (push failed — building without DuckDB)" >&2
  fi
  rsh "ls $RDIR/.tools/duckdb-static/*.a >/dev/null 2>&1" && DUCK_STATIC=ON
else
  echo "  (no local .tools/duckdb-static — run tools/get_duckdb.sh --static)" >&2
fi

# ---- build ----------------------------------------------------------------
if [ "$do_build" -eq 1 ]; then
  echo "== build (mlx5dv provider)"
  # `set -o pipefail` is not decoration. `cmake --build | tail -3` exits with
  # TAIL's status, so a failed build returned 0, the `||` below never fired, and
  # the runner went on to execute cases against the previous binary. That is
  # finding 14 of the round-2 studies, word for word, and it was still here in
  # this file after being fixed in wait_and_run_mn5.sh.
  rsh "set -o pipefail; export PCOMM_COMMIT=$build_commit; cd $RDIR &&
       cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DPCOMM_BUILD_TESTS=OFF -DPCOMM_PROVIDER_MLX5DV=ON -DPCOMM_DUCKDB=$DUCK_STATIC -DPCOMM_DUCKDB_STATIC=$DUCK_STATIC -DPCOMM_UI=OFF >/dev/null &&
       cmake --build build -j 2>&1 | tail -3" || {
    echo "run_on_a100.sh: build failed" >&2; exit 1; }
fi

# ---- is the build newer than the source it claims to be? -------------------
# L3 of PLAN-20260904-1900, and this runner needs it more than MN5 does: there
# is no commit here to be attributable to, `--no-build` is the normal way to
# iterate, and the checkout this rsyncs from is fed by syncthing — which is how
# a provider `.cpp` once ran against the `.sql` from a different commit.
#
# 120 s of grace because the mtimes being compared come from two clocks: rsync
# preserves this workstation's timestamps, the build is stamped by du04's.
rsh "bash $RDIR/tests/hpc/suite/check_build_freshness.sh $RDIR $RDIR/build 120" || {
  echo "run_on_a100.sh: refusing to run cases against a build older than the tree." >&2
  echo "  drop --no-build. (Only compiled files count — assertions and docs do not" >&2
  echo "  trip this.)" >&2
  exit 1; }

# ---- the launcher carries the providers ------------------------------------
# After the freshness check and never before: baking rewrites the launcher, and
# a step that touched the newest artifact on every run would make that check
# vacuous. A no-op when the payload already matches what was built.
echo "== provider payload"
rsh "cd $RDIR && bash tests/hpc/suite/bake_providers.sh $RDIR/build" || {
  echo "run_on_a100.sh: could not bake the providers into the launcher." >&2; exit 1; }

# ---- run ------------------------------------------------------------------
# No scheduler here, so a case's job.sbatch is executed directly — which works
# because #SBATCH lines are comments to bash, the same property all_in_one.sbatch
# already relies on.
for c in $(rsh "ls $RDIR/tests/hpc/cases" | grep -E "^(${cases_filter// /|})" ); do
  echo "== case $c"
  rsh "cd $RDIR &&
       export PCOMM_ROOT=$RDIR &&
       export PCOMM_OUT_ROOT=$RDIR/results &&
       export PCOMM_PROVIDER_PATH=$RDIR/build/providers/mlx5dv &&
       export PCOMM=$RDIR/build/pcomm/pcomm &&
       export PCOMM_EXPECT_COMMIT=$build_commit &&
       export PCOMM_PREFER_BAKED=1 &&
       bash tests/hpc/cases/$c/job.sbatch 2>&1 | tail -40"
done

echo
echo "== board"
rsh "for f in $RDIR/results/*.csv/result.json; do [ -e \"\$f\" ] && cat \"\$f\"; done"
