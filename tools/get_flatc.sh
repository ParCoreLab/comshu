#!/usr/bin/env bash
# Build the pinned flatc into .tools/flatc (gitignored).
#
# The version is pinned to the one that generated
# libpcomm/schema/generated/pcomm_generated.h. flatc's output is not stable
# across releases, so an unpinned flatc turns tools/check_generated.sh into a
# diff against a different generator rather than against a stale header — the
# check would fail for the wrong reason, or worse, a regeneration with a newer
# flatc would silently rewrite the committed header.
#
# The binary is deliberately not committed (4 MB, arch-specific). The *headers*
# are, under third_party/flatbuffers — those are what the build needs, and they
# carry a version static_assert that catches a mismatched generator anyway.
#
# Needs network + cmake; neither exists on MN5, which is why nothing in the
# build depends on this script.
set -euo pipefail

VERSION="v25.12.19"          # keep in step with third_party/flatbuffers
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
dest="$root/.tools/flatc"
src="${PCOMM_FLATBUFFERS_SRC:-}"

if [[ -x "$dest" ]] && "$dest" --version | grep -q "${VERSION#v}"; then
    echo "flatc ${VERSION} already at $dest"
    exit 0
fi

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

if [[ -z "$src" ]]; then
    echo "cloning flatbuffers ${VERSION}"
    git clone --depth 1 --branch "$VERSION" https://github.com/google/flatbuffers.git "$work/src"
    src="$work/src"
fi

cmake -S "$src" -B "$work/build" -DCMAKE_BUILD_TYPE=Release \
      -DFLATBUFFERS_BUILD_TESTS=OFF -DFLATBUFFERS_BUILD_FLATLIB=OFF \
      -DFLATBUFFERS_BUILD_FLATHASH=OFF
cmake --build "$work/build" --target flatc -j "$(nproc)"

mkdir -p "$root/.tools"
cp "$work/build/flatc" "$dest"
echo "installed $("$dest" --version) at $dest"

# The vendored headers must come from the same tag, or the version static_assert
# in the generated header fires at build time.
echo "note: third_party/flatbuffers/include is vendored from ${VERSION}; if you"
echo "      bump VERSION here, refresh those headers and regenerate the schema."
