#!/usr/bin/env bash
# Fetch the pinned prebuilt libduckdb into .tools/duckdb (gitignored).
#
# DuckDB is what makes `pcomm export --parquet` and `pcomm sql` possible: it writes
# Parquet natively, so the reader needs no Arrow, and it is the same engine the
# UI queries with. One dependency covers the writer, the query layer and the UI
# server; see plans/PLAN-20260807-1718-ui-backbone-todo.md.
#
# NOT committed, and not a build dependency by default. The library is ~50 MB
# and architecture-specific, and the working tree is syncthing-mirrored, so
# committing it would replicate it to every host forever — the same reasoning
# that keeps flatc out of the tree (tools/get_flatc.sh) and the capture corpus
# out of ~/pockt (tests/hpc/corpus/README.md).
#
# **MN5 has no network and no cmake outside `module load`** — verified
# 2026-08-09, not merely inherited from get_flatc.sh's comment. That is why
# PCOMM_DUCKDB defaults to OFF: the suite builds and runs `pcomm export --csv` on
# MN5 exactly as before, and nothing in the cluster path depends on this script.
# Parquet is produced where the UI runs, which is not the cluster.
#
#   tools/get_duckdb.sh          # fetch into .tools/duckdb
#   cmake -B build -DPCOMM_DUCKDB=ON
set -euo pipefail

VERSION="v1.5.5"
ASSET="libduckdb-linux-amd64.zip"

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
dest="$root/.tools/duckdb"

# The C API, not the C++ one. duckdb.hpp across a prebuilt .so is an ABI bet on
# the vendor's compiler matching ours; duckdb.h is a stable C boundary, and
# everything we ask of DuckDB is "execute this SQL string", which the C API does
# without ceremony.
if [[ -f "$dest/duckdb.h" && -f "$dest/libduckdb.so" ]]; then
    have=$(grep -oE 'DUCKDB_VERSION "[^"]+"' "$dest/duckdb.h" 2>/dev/null | head -1 | cut -d'"' -f2 || true)
    if [[ "$have" == "$VERSION" ]]; then
        echo "libduckdb ${VERSION} already at $dest"
        exit 0
    fi
    echo "replacing libduckdb ${have:-unknown} with ${VERSION}"
fi

command -v curl >/dev/null || { echo "curl is required" >&2; exit 1; }
command -v unzip >/dev/null || { echo "unzip is required" >&2; exit 1; }

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

url="https://github.com/duckdb/duckdb/releases/download/${VERSION}/${ASSET}"
echo "fetching ${url}"
curl -fsSL "$url" -o "$work/duckdb.zip"
unzip -q -o "$work/duckdb.zip" -d "$work/x"

mkdir -p "$dest"
cp "$work/x/duckdb.h" "$work/x/libduckdb.so" "$dest/"
[[ -f "$work/x/duckdb.hpp" ]] && cp "$work/x/duckdb.hpp" "$dest/"

echo "libduckdb ${VERSION} -> $dest"
ls -la "$dest"
