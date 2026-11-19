#!/usr/bin/env bash
# `comshu bake`, the two ways CR-31 found it could hurt:
#
#  1. IN PLACE. `comshu bake -o X X lib.so` opened X for writing — truncating it —
#     before copying X out of itself, and left a zero-length launcher behind.
#     It must now produce a working, larger X with the payload listed.
#  2. A CRAFTED FOOTER. The archive reader bounds-checked with sums that wrap:
#     toc_off + toc_len in 64 bits, nlen + 32 in 32 bits. Each must be refused,
#     not walked.
set -uo pipefail
COMSHU=${1:?usage: bake_rule.sh <comshu> <some.so>}
LIB=${2:?}
WORK=$(mktemp -d "${TMPDIR:-/tmp}/comshu-bake-XXXXXX")
trap 'rm -rf "$WORK"' EXIT
fail=0

cp "$COMSHU" "$WORK/c"
before=$(stat -c%s "$WORK/c")
"$COMSHU" bake -o "$WORK/c" "$WORK/c" "$LIB" > "$WORK/bake.out" 2>&1 || {
    cat "$WORK/bake.out"; echo "FAIL: in-place bake failed"; fail=1; }
after=$(stat -c%s "$WORK/c")
if [ "$after" -le "$before" ]; then
    echo "FAIL: in-place bake left $after bytes (host was $before)"; fail=1
elif ! "$WORK/c" bake --list "$WORK/c" 2>&1 | grep -q "$(basename "$LIB")"; then
    echo "FAIL: the baked-in-place launcher does not list its payload"; fail=1
else
    echo "ok: baked in place ($before -> $after bytes), runs and lists its payload"
fi
if ls -A "$WORK" | grep -q '\.bake-'; then
    echo "FAIL: a temporary was left beside the output"; fail=1
fi

# Footer: host_size, toc_off, toc_len (u64), count, version (u32), magic — LE.
craft() {  # <file> <toc_off> <toc_len> <count> [toc-bytes-hex]
    python3 - "$@" <<'PY'
import struct, sys
f, toc_off, toc_len, count = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])
toc = bytes.fromhex(sys.argv[5]) if len(sys.argv) > 5 else b""
body = b"\x7fELF" + b"\0" * 60 + toc
foot = struct.pack("<QQQII", 64, toc_off, toc_len, count, 2) + b"PCOMMBK1"
open(f, "wb").write(body + foot)
PY
}
# toc_off near 2^64: toc_off + toc_len wraps to a small number.
craft "$WORK/wrap64" $(( (1<<63) * 2 - 8 )) 16 1 2>/dev/null || craft "$WORK/wrap64" 18446744073709551608 16 1
# A real-looking TOC whose one name claims 0xFFFFFFF0 bytes: nlen + 32 wraps to 16.
craft "$WORK/wrap32" 64 36 1 "f0ffffff$(printf '00%.0s' $(seq 32))"
for f in wrap64 wrap32; do
    "$COMSHU" bake --list "$WORK/$f" > "$WORK/$f.out" 2>&1
    rc=$?
    if [ "$rc" -ge 128 ]; then
        echo "FAIL: --list on a crafted $f footer crashed (rc=$rc)"; fail=1
    else
        echo "ok: crafted $f footer refused without a crash (rc=$rc)"
    fi
done

[ "$fail" -eq 0 ] && echo "PASS: bake in place and crafted footers"
exit "$fail"
