#!/usr/bin/env bash
# Fail if libcomshu/schema/generated/comshu_generated.h is not what flatc produces
# from libcomshu/schema/comshu.fbs today.
#
# The header is committed on purpose — MN5's login node is airgapped and has no
# flatc, so the build must never need to run it. The cost of committing it is
# that it can go stale: edit the schema, forget to regenerate, and the
# conformance test happily validates the C++ records against yesterday's
# structs while reporting success. Nothing else notices, because the stale
# header is self-consistent.
#
# Where flatc is missing this skips loudly rather than failing — the airgapped
# build is the reason the header is committed in the first place. Any developer
# machine should have it: tools/get_flatc.sh.
set -uo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
schema="$root/libcomshu/schema/comshu.fbs"

# PRESENT AND EXECUTABLE IS NOT THE SAME AS RUNNABLE, which cost a real
# investigation. `.tools/flatc` is gitignored and ARCH-SPECIFIC: the copy sitting
# in a tree rsynced from another machine is a perfectly good `-x` file that
# cannot exec here — the one on MN5 and on du04 both want GLIBC 2.38 and neither
# host has it. The old test was `-x`, so the exec failure fell through to the
# "flatc rejected the schema" branch below and reported a SCHEMA defect on two
# machines whose schema was fine. Probe by running it, the same reasoning
# build_all.sh gives for probing a toolchain rather than assuming one.
flatc="${COMSHU_FLATC:-}"
runnable() { [[ -n "$1" ]] && "$1" --version >/dev/null 2>&1; }
if [[ -z "$flatc" ]]; then
    if runnable "$root/.tools/flatc"; then flatc="$root/.tools/flatc"
    elif command -v flatc >/dev/null 2>&1 && runnable "$(command -v flatc)"; then
        flatc="$(command -v flatc)"
    fi
fi

if ! runnable "$flatc"; then
    echo "generated-header check: SKIPPED — no flatc that runs on this host"
    echo "  looked at: \$COMSHU_FLATC, $root/.tools/flatc, PATH"
    if [[ -e "$root/.tools/flatc" ]] && ! runnable "$root/.tools/flatc"; then
        echo "  NOTE: $root/.tools/flatc exists but does not execute here —"
        echo "        it is arch-specific and was almost certainly synced from"
        echo "        another machine. Re-run tools/get_flatc.sh to rebuild it."
    fi
    echo "  expected on airgapped nodes (MN5); run tools/get_flatc.sh on a dev box."
    exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

if ! "$flatc" --cpp -o "$tmp/cpp" "$schema" || ! "$flatc" --python -o "$tmp/py" "$schema"; then
    echo "generated-header check: FAIL — flatc rejected libcomshu/schema/comshu.fbs"
    exit 1
fi

rc=0
# Both generated trees are committed for the same reason (no flatc on MN5), so
# both can go stale the same way. The Python bindings are what tools/read_pcs.py
# reads real captures with — stale ones would make that proof a proof of nothing.
check_tree() {   # <label> <committed path> <fresh path> <regen command>
    if diff -ur -x __pycache__ "$2" "$3" > "$tmp/drift.diff" 2>&1; then
        echo "  [ok ] $1"
        return
    fi
    echo "  [XX ] $1 — committed output is not what comshu.fbs generates"
    echo "        regenerate: $4"
    sed -n '1,40p' "$tmp/drift.diff"
    lines=$(wc -l < "$tmp/drift.diff")
    if (( lines > 40 )); then echo "        ... $((lines - 40)) more diff lines"; fi
    rc=1
}

echo "generated-code check vs libcomshu/schema/comshu.fbs ($("$flatc" --version))"
check_tree "generated/comshu_generated.h" \
           "$root/libcomshu/schema/generated/comshu_generated.h" \
           "$tmp/cpp/comshu_generated.h" \
           "cd libcomshu/schema && flatc --cpp -o generated/ comshu.fbs"
check_tree "generated-python/" \
           "$root/libcomshu/schema/generated-python" \
           "$tmp/py" \
           "cd libcomshu/schema && flatc --python -o generated-python/ comshu.fbs"

if (( rc )); then echo "FAIL"; else echo "PASS — generated code is current"; fi
exit $rc
