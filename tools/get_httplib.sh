#!/usr/bin/env bash
# Fetch the pinned cpp-httplib header into .tools/httplib (gitignored).
#
# `pcomm ui` needs to serve a page and answer queries over HTTP. cpp-httplib is
# a single header with no dependencies and an MIT licence, which is the whole
# reason it is chosen over anything that would drag a build system with it.
#
# Same shape as tools/get_duckdb.sh and for the same reasons: not committed, not
# a build dependency by default, and not needed on MN5 — the UI runs where the
# user is, and reaches a cluster capture through a port-forward, not by being
# built there.
set -euo pipefail

VERSION="v0.29.0"
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
dest="$root/.tools/httplib"

if [[ -f "$dest/httplib.h" ]]; then
    have=$(grep -oE 'CPPHTTPLIB_VERSION "[^"]+"' "$dest/httplib.h" | head -1 | cut -d'"' -f2 || true)
    if [[ "v$have" == "$VERSION" ]]; then
        echo "cpp-httplib ${VERSION} already at $dest"
        exit 0
    fi
fi

command -v curl >/dev/null || { echo "curl is required" >&2; exit 1; }
mkdir -p "$dest"
url="https://raw.githubusercontent.com/yhirose/cpp-httplib/${VERSION}/httplib.h"
echo "fetching ${url}"
curl -fsSL "$url" -o "$dest/httplib.h"
echo "cpp-httplib ${VERSION} -> $dest/httplib.h"
