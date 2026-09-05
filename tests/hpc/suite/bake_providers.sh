#!/bin/bash
# bake_providers.sh — put every provider this build produced inside the launcher.
#
#   bake_providers.sh [<build dir>] [<launcher>]
#
# WHY A SCRIPT AND NOT A CMAKE STEP. The libraries come from four build trees —
# NVBit pins gcc-11.4 for gpumem, UCX needs the HPC-X tree, NCCL its headers —
# and only the runner knows all four have finished. `cmake --install` can carry
# the targets of the tree it was given and no more, which is how the install
# bundle came to be missing four of seven providers.
#
# WHY IT RUNS ON EVERY RUN, not only after a build. Baking used to sit inside
# the runner's build block, so a `--no-build` run left yesterday's payload in
# place. Nothing on disk looked wrong — the disk copies are what a build tree
# loads — and the staleness would surface only on the machine you copied the
# single file to, which is the worst place to find out. So this checks first and
# rebakes only when the payload differs.
#
# THE CHECK IS BY CONTENT, and the no-op path leaves the binary's mtime alone.
# That matters twice over: providers are COPIED between build trees, so their
# mtimes describe the copy rather than the contents; and check_build_freshness.sh
# reads the newest artifact's mtime, so a bake that touched the launcher on every
# run would quietly make that guard vacuous.
set -uo pipefail
BUILD="${1:-build}"
PCOMM_BIN="${2:-$BUILD/pcomm/pcomm}"

[ -x "$PCOMM_BIN" ] || { echo "bake_providers: no launcher at $PCOMM_BIN" >&2; exit 2; }

# Everything the launcher can load: the engine, every provider, every backend.
# Globbed rather than listed for the reason common.sh gives about the provider
# path — a hand-maintained list of providers is a list kept in step by nobody,
# and the one it missed (ucx_deep) took a job to notice.
libs=()
for f in "$BUILD"/libpcomm/libpcomm_engine.so "$BUILD"/providers/*/*.so "$BUILD"/gpu-backends/*/*.so; do
  [ -e "$f" ] && libs+=("$f")
done
[ "${#libs[@]}" -gt 0 ] || { echo "bake_providers: nothing to bake under $BUILD" >&2; exit 2; }

# Stripped copies, and the strip is what makes the check meaningful as well as
# the payload small: `--strip-debug` is deterministic, so the same library
# always produces the same bytes and "did this change" stays a content question.
# It leaves both symbol tables alone, so interposition and NVBit are untouched.
stage="$BUILD/.bake"
rm -rf "$stage" && mkdir -p "$stage" || exit 2
cp "${libs[@]}" "$stage"/ || exit 2
strip --strip-debug "$stage"/*.so 2>/dev/null

if "$PCOMM_BIN" bake --check "$PCOMM_BIN" "$stage"/*.so >/dev/null 2>&1; then
  echo "  payload is already current ($(ls "$stage"/*.so | wc -l) file(s))"
  rm -rf "$stage"
  exit 0
fi

# Never write over the binary being executed: bake to a new file, then move it
# into place. `mv` within one directory is a rename, so the launcher is either
# the old one or the new one and never a half-written mixture.
"$PCOMM_BIN" bake -o "$stage/pcomm" "$PCOMM_BIN" "$stage"/*.so || exit 1
mv "$stage/pcomm" "$PCOMM_BIN" || exit 1
"$PCOMM_BIN" bake --list | tail -1
rm -rf "$stage"
