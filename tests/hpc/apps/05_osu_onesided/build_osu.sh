#!/bin/bash
# Build the OSU Micro-Benchmarks on MN5's LOGIN NODE. Not in the allocation:
# queue slots are the scarce resource and a configure+make is eight minutes of
# one, spent on something that needs no GPU and no second node.
#
#   # once, from the machine that HAS network — MN5 has none:
#   fetch/curl https://mvapich.cse.ohio-state.edu/download/mvapich/osu-micro-benchmarks-7.5.tar.gz
#   rsync -av osu-micro-benchmarks-7.5.tar.gz koc296797@alogin2.bsc.es:~/
#
#   # then, on alogin2 (login shell is tcsh — wrap it):
#   bash -lc 'bash ~/proj/pcomm/tests/hpc/apps/05_osu_onesided/build_osu.sh'
#
# sha256 of the tarball this study was written against:
#   1cf84ac5419456202757a757c5f9a4f5c6ecd05c65783c7976421cfd6020b3b3
set -euo pipefail

PCOMM_ROOT="${PCOMM_ROOT:-$HOME/proj/pcomm}"
TARBALL="${TARBALL:-$HOME/osu-micro-benchmarks-7.5.tar.gz}"
PREFIX="${PREFIX:-$HOME/osu-7.5}"
BUILD="${BUILD:-$HOME/build-osu-7.5}"
WANT_SHA=1cf84ac5419456202757a757c5f9a4f5c6ecd05c65783c7976421cfd6020b3b3

[ -f "$TARBALL" ] || { echo "!! no tarball at $TARBALL — rsync it from a networked machine" >&2; exit 1; }

# The hash is checked rather than trusted. A study whose oracle is derived from
# a source tree must be able to say WHICH source tree; a different 7.x would
# change the default window size or the iteration counts and every number in
# meta.json with it.
GOT="$(sha256sum "$TARBALL" | cut -d' ' -f1)"
[ "$GOT" = "$WANT_SHA" ] || {
  echo "!! $TARBALL hashes $GOT, expected $WANT_SHA" >&2
  echo "!! the oracle in meta.json was derived from the expected tree — refusing" >&2
  exit 1; }

# The SAME toolchain the job will run under. 01_gromacs paid for this lesson:
# a benchmark linked against one HPC-X and launched by another mixes two Open
# MPI releases in one job, and the UCX provider then interposes a libucp that
# the application's pml was not built against.
source "$PCOMM_ROOT/tests/hpc/suite/common.sh"
pcomm_load_runtime
command -v mpicc >/dev/null || { echo "!! no mpicc after pcomm_load_runtime" >&2; exit 1; }
echo "=== building against: $(command -v mpicc)"
mpicc -show 2>/dev/null || true

rm -rf "$BUILD"; mkdir -p "$BUILD"
tar xzf "$TARBALL" -C "$BUILD" --strip-components=1

cd "$BUILD"
./configure CC=mpicc CXX=mpicxx --prefix="$PREFIX"
make -j"$(nproc)"
make install

# Where 7.x puts them. Named explicitly so job.sbatch and this script cannot
# drift apart about the path.
ONESIDED="$PREFIX/libexec/osu-micro-benchmarks/mpi/one-sided"
for b in osu_put_bw osu_get_bw osu_acc_latency osu_fop_latency; do
  [ -x "$ONESIDED/$b" ] || { echo "!! $ONESIDED/$b missing after install" >&2; exit 1; }
done

echo "=== built, and this is the MPI they will actually load ==="
ldd "$ONESIDED/osu_put_bw" | grep -E 'libmpi|libucp|libucs' || true
echo "=== $ONESIDED"
ls -la "$ONESIDED"
