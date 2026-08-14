#!/usr/bin/env bash
# Fetch the transport development headers into .tools/sysroot (gitignored), by
# extracting distro RPMs rather than installing them.
#
# WHY THIS EXISTS. simurgh-base has libibverbs.so.1 and libmlx5.so.1 — you can
# RUN a capture-adjacent binary here — but no -devel packages, so the providers
# cannot be COMPILED. Every provider change was therefore going straight to MN5
# to find out whether it builds, and MN5 is one job at a time. A compile error
# discovered in the queue costs a slot; discovered here it costs ten seconds.
#
# Extracted, not installed, for the reason get_duckdb.sh is not committed: this
# needs no root, touches nothing outside the tree, and the working tree is
# syncthing-mirrored so a system package would not follow it anyway. It had been
# living in a /tmp scratchpad and being rebuilt by hand every time that was
# cleaned, which is the state this script ends.
#
# THIS IS A BUILD GATE, NOT A RUNTIME. The headers are the distro's, not the
# cluster's; a successful compile here says the code is well-formed, not that it
# links against MN5's rdma-core. That still catches the overwhelming majority of
# provider mistakes, which are type and signature errors.
#
#   tools/get_devheaders.sh                    # rdma-core (default)
#   tools/get_devheaders.sh rdma libfabric ucx # and the transports above it
#   cmake -B build -DPCOMM_PROVIDER_IBVERBS=ON \
#         -DCMAKE_PREFIX_PATH=$PWD/.tools/sysroot/usr
set -euo pipefail

cd "$(dirname "$0")/.."
SYSROOT="$PWD/.tools/sysroot"

# Package sets per transport. rdma-core carries both verbs and mlx5dv, so the
# ibverbs and mlx5dv providers share one entry.
declare -A PKGS=(
    [rdma]="rdma-core-devel libibverbs"
    [libfabric]="libfabric-devel libfabric"
    [ucx]="ucx-devel ucx"
)

want=("${@:-rdma}")
mkdir -p "$SYSROOT"
tmp=$(mktemp -d "${TMPDIR:-/tmp}/pcomm-devheaders-XXXXXX")
trap 'rm -rf "$tmp"' EXIT

for group in "${want[@]}"; do
    if [ -z "${PKGS[$group]+x}" ]; then
        echo "unknown group '$group' — known: ${!PKGS[*]}" >&2
        exit 2
    fi
    echo "== $group: ${PKGS[$group]}"
    ( cd "$tmp" && dnf download --quiet ${PKGS[$group]} ) || {
        echo "dnf download failed for $group — no network, or the package names moved" >&2
        exit 1
    }
done

shopt -s nullglob
rpms=("$tmp"/*.rpm)
if [ ${#rpms[@]} -eq 0 ]; then
    echo "no rpms downloaded" >&2
    exit 1
fi
for rpm in "${rpms[@]}"; do
    ( cd "$SYSROOT" && rpm2cpio "$rpm" | cpio -idmu --quiet )
done

# The -devel packages ship .so as a symlink into the versioned runtime; both
# came out of the same extraction, so the links resolve inside the sysroot.
echo
echo "sysroot: $SYSROOT"
for h in infiniband/verbs.h infiniband/mlx5dv.h rdma/fabric.h ucp/api/ucp.h; do
    [ -f "$SYSROOT/usr/include/$h" ] && echo "  have $h"
done
echo
echo "configure with:"
echo "  cmake -B build -DCMAKE_PREFIX_PATH=$SYSROOT/usr \\"
echo "        -DPCOMM_PROVIDER_IBVERBS=ON -DPCOMM_PROVIDER_MLX5DV=ON"
