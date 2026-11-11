#!/bin/bash
# check_build_freshness.sh — refuse to run a build that is older than its source.
#
#   check_build_freshness.sh [<repo root>] [<build dir>] [<grace seconds>]
#
# L1 and L2 of PLAN-20260904-1900 are DETECTIVE: they read a capture and tell
# you afterwards, on a board you have already queued, run and paid for. This is
# the preventive half, and it is the one that makes `--no-build` mean "do not
# rebuild if nothing changed" instead of "trust me".
#
# THE CLAIM IS NARROW ON PURPOSE: *something* in this build is at least as new
# as the newest compiled input. The plan's first sketch compared every artifact
# against the newest source, and that is wrong here — providers live in
# separate targets, so editing providers/ucx/*.cpp relinks libucx_trace.so and
# legitimately leaves libibverbs_trace.so at yesterday's mtime. A guard that
# demands a full rebuild on every edit gets passed over within a week, and then
# nothing is checked at all; that is the same dynamic that got
# `ucx_edges_include_intranode` deleted rather than fixed.
#
# So the three layers divide the question and none of them subsumes another:
#
#   L3 (here)  did you build AT ALL since you last edited a compiled file?
#   L2 (assert images_are_no_older_than_the_engine)
#              were the objects that actually loaded built together?
#   L1 (assert build_is_the_boards_commit)
#              is the thing that ran the commit this board claims?
#
# ONLY COMPILED INPUTS COUNT. A commit that touches nothing but `.sql`
# assertions, `meta.json` or documentation must not force a rebuild — those are
# read from the tree at run time, and refusing them would make the guard fire on
# exactly the iteration loop it is meant to protect.
#
# ONE OPERATIONAL WRINKLE, measured while mutation-testing this on 2026-09-04:
# `git reset --hard` re-checks-out any file whose stat data has drifted — a bare
# `touch` on the deploy tree is enough — and stamps it with today's mtime even
# though the content never changed. So poking a source file on the remote costs
# one rebuild before `--no-build` is allowed again. That is the guard behaving
# correctly on a tree that genuinely has a source newer than its build; it is
# noted here so the next person does not read it as a bug.
#
# THE GRACE ARGUMENT IS ABOUT TWO CLOCKS, not about leniency. On the git path
# every mtime here is minted by the remote itself — `git reset --hard` touches
# the files it changes, the build follows on the same host — so the comparison
# is exact and the grace is 0. An rsync'd tree carries the WORKSTATION's mtimes
# into a build stamped by the cluster's clock, so a few seconds of skew between
# two NTP-synced machines would refuse a build that is perfectly fresh. Callers
# that rsync pass 120; the cost is a 2-minute window after an edit in which this
# layer is blind, which L1 and L2 cover on the git path and nothing covers on
# the rsync path. Say so rather than pretending otherwise.
#
# Exit 0 fresh, 1 stale, 2 nothing built.
set -u
ROOT="${1:-$PWD}"
BUILD="${2:-$ROOT/build}"
GRACE="${3:-0}"

# `build*` covers build/, build-gpumem/, build-ucx/, build-nccl/. .tools holds
# the pushed libduckdb, ui/dist a built bundle: both are inputs to nothing this
# checks and both are large.
newest() {
  find "$1" \
       \( -name 'build' -o -name 'build-*' -o -name '.git' -o -name '.tools' \
          -o -name 'node_modules' -o -name 'dist' -o -name '.cache' \) -prune -o \
       -type f \( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' -o -name '*.c' \
                  -o -name '*.cu' -o -name '*.cuh' -o -name '*.cmake' \
                  -o -name 'CMakeLists.txt' \) -printf '%T@ %p\n' 2>/dev/null |
    sort -rn | head -1
}

src="$(newest "$ROOT")"
[ -n "$src" ] || { echo "check_build_freshness: no compiled sources under $ROOT" >&2; exit 2; }

# What actually loads: the launcher, the engine, and every provider .so under
# the build directory the cases will use. The side build trees are not checked —
# their libraries are COPIED into build/providers/, and the copy is what runs.
art="$(find "$BUILD" -type f \( -name '*.so' -o -name 'comshu' \) -printf '%T@ %p\n' 2>/dev/null |
       sort -rn | head -1)"
if [ -z "$art" ]; then
  echo "!! nothing is built under $BUILD — there is no binary to run" >&2
  exit 2
fi

src_t="${src%% *}"; src_t="${src_t%%.*}"; src_f="${src#* }"
art_t="${art%% *}"; art_t="${art_t%%.*}"; art_f="${art#* }"

if [ "$src_t" -le "$((art_t + GRACE))" ]; then
  exit 0
fi

echo "!! this build is older than the tree it was built from." >&2
printf '!!   newest source   : %s  %s\n' "$(date -d "@$src_t" '+%F %T' 2>/dev/null || echo "$src_t")" \
       "${src_f#$ROOT/}" >&2
printf '!!   newest artifact : %s  %s\n' "$(date -d "@$art_t" '+%F %T' 2>/dev/null || echo "$art_t")" \
       "${art_f#$ROOT/}" >&2
# Name a few, because "something changed" sends the reader looking and the file
# list usually answers the question on its own (a header everything includes is
# a different situation from one provider's .cpp).
echo "!!   changed since that build:" >&2
find "$ROOT" \
     \( -name 'build' -o -name 'build-*' -o -name '.git' -o -name '.tools' \
        -o -name 'node_modules' -o -name 'dist' -o -name '.cache' \) -prune -o \
     -type f \( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' -o -name '*.c' \
                -o -name '*.cu' -o -name '*.cuh' -o -name '*.cmake' \
                -o -name 'CMakeLists.txt' \) -newer "$art_f" -print 2>/dev/null |
  head -8 | sed "s|^$ROOT/|!!     |" >&2
echo "!! run without --no-build. Only compiled files count here — a commit that" >&2
echo "!! touches assertions or docs will not trip this." >&2
exit 1
