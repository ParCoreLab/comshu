#!/usr/bin/env bash
# Build the pcomm frontend into ui/dist, which pcomm-ui then embeds.
#
# WHY THIS EXISTS. ui/dist is a build artifact and is not committed, so the
# build has to produce it. It was committed for one day (05a7671) to make the
# spack +ui variant possible at all, and that is the wrong shape: Vite's hashed
# filenames never dedupe, so every frontend change added new blobs rather than
# replacing them, and a fresh build did not even reproduce the committed bundle
# byte for byte.
#
# cmake drives this script rather than calling npm itself because `npm ci`
# deletes node_modules and reinstalls from scratch. That has to happen only when
# the lockfile actually changed, and "only when" is easier to say here than in a
# custom command.
#
# npm rather than pnpm, which this used to use: a spack +ui build installs the
# frontend dependencies itself, and spack packages npm but has no pnpm at all
# (its node-js is even configured --without-npm, so npm is its own package).
# Measured before switching: three of the four output chunks are byte-identical
# between the two, and only the app chunk differs, by 725 bytes.
set -euo pipefail

ui="$(cd "$(dirname "${BASH_SOURCE[0]}")/../ui" && pwd)"
cd "$ui"

npm="${NPM:-npm}"
if ! command -v "$npm" >/dev/null 2>&1; then
    echo "build_ui.sh: no '$npm' on PATH — the UI cannot be built without it" >&2
    echo "             configure with -DPCOMM_UI=OFF to build without the UI" >&2
    exit 1
fi

# npm ci only when package-lock.json is newer than what npm last installed from.
# node_modules/.package-lock.json is npm's own record of that, so it is the file
# to compare against rather than a stamp of our own.
if [[ ! -f node_modules/.package-lock.json ]] ||
   [[ package-lock.json -nt node_modules/.package-lock.json ]]; then
    echo "build_ui.sh: installing frontend dependencies (npm ci)"
    "$npm" ci
fi

"$npm" run build
