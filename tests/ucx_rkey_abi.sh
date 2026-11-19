#!/usr/bin/env bash
# CR-05: ucp_ep_rkey_unpack_internal reaches libucp INTACT, on every signature
# UCX has shipped, and its key is recorded only where the version says which
# argument holds it.
#
# The defect this guards: the interposer declared seven arguments, UCX 1.23 has
# eight (`uct_flags` was inserted before rkey_p), and the eighth — rkey_p itself
# — was never forwarded. libucp then wrote the unpacked key through whatever
# qword sat in that stack slot. Nothing failed to build and nothing failed
# loudly; a record gate on the real UCX here (1.19) cannot see it.
#
# So this runs a stand-in libucp (tests/ucx_rkey_abi/fake_ucp.cpp) in each shape,
# behind the real provider, and asserts both halves:
#   - the call: the driver gets its key back, and nothing else was written;
#   - the record: exactly one ucx_rkey row where the layout is verified, none
#     where it is not (1.23.0 is ambiguous: its rc1/rc2 took seven).
set -uo pipefail

COMSHU=${1:?usage: ucx_rkey_abi.sh <comshu> <engine-so> <provider-so> <driver> <fake7-dir> <fake8-dir>}
ENGINE=${2:?}; PROVIDER=${3:?}; DRIVER=${4:?}; FAKE7=${5:?}; FAKE8=${6:?}

WORK=$(mktemp -d "${TMPDIR:-/tmp}/comshu-ucx-abi-XXXXXX")
trap 'rm -rf "$WORK"' EXIT
fail=0

run() {  # <label> <libdir> <version> <driver-nargs> <expected ucx_rkey rows>
    local label=$1 dir=$2 ver=$3 nargs=$4 want=$5 cap="$WORK/$1"
    if ! FAKE_UCX_VERSION=$ver LD_LIBRARY_PATH="$dir:${LD_LIBRARY_PATH:-}" \
         COMSHU_OUTPUT_DIR="$cap" COMSHU_TRACE_MODE=full \
         LD_PRELOAD="$ENGINE $PROVIDER" "$DRIVER" "$nargs" > "$WORK/$label.out" 2>&1; then
        sed 's/^/  /' "$WORK/$label.out"
        echo "FAIL: $label — the call did not reach libucp intact"; fail=1; return
    fi
    "$COMSHU" export "$cap" --csv "$cap.csv" > "$WORK/$label.export" 2>&1 || {
        cat "$WORK/$label.export"; echo "FAIL: $label — export failed"; fail=1; return; }
    local got
    got=$(tail -n +2 "$cap.csv/ucx_rkey.csv" 2>/dev/null | wc -l)
    if [ "$got" != "$want" ]; then
        echo "FAIL: $label — ucx_rkey rows: expected $want, got $got"; fail=1; return
    fi
    if [ "$want" = 0 ] && ! grep -q "unverified for this version" "$WORK/$label.out"; then
        echo "FAIL: $label — an unverified version recorded nothing, and did not say so"; fail=1
        return
    fi
    echo "ok: $label ($ver, $nargs args) — forwarded intact, $got ucx_rkey row(s)"
}

run seven_1.19.0  "$FAKE7" 1.19.0 7 1
run seven_1.22.0  "$FAKE7" 1.22.0 7 1
run eight_1.23.2  "$FAKE8" 1.23.2 8 1
run eight_1.23.0  "$FAKE8" 1.23.0 8 0   # ambiguous: forwarded, not recorded
run eight_1.24.0  "$FAKE8" 1.24.0 8 0   # newer than anything read

[ "$fail" -eq 0 ] && echo "PASS: ucp_ep_rkey_unpack_internal forwarded intact on 7- and 8-argument UCX"
exit "$fail"
