#!/usr/bin/env bash
# Fail if libpcomm/schema/generated/pcomm_generated.h is not what flatc produces
# from libpcomm/schema/pcomm.fbs today.
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
schema="$root/libpcomm/schema/pcomm.fbs"

flatc="${PCOMM_FLATC:-}"
if [[ -z "$flatc" ]]; then
    if [[ -x "$root/.tools/flatc" ]]; then flatc="$root/.tools/flatc"
    elif command -v flatc >/dev/null 2>&1; then flatc="$(command -v flatc)"
    fi
fi

if [[ -z "$flatc" ]]; then
    echo "generated-header check: SKIPPED — no flatc"
    echo "  looked at: \$PCOMM_FLATC, $root/.tools/flatc, PATH"
    echo "  expected on airgapped nodes (MN5); run tools/get_flatc.sh on a dev box."
    exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

if ! "$flatc" --cpp -o "$tmp/cpp" "$schema" || ! "$flatc" --python -o "$tmp/py" "$schema"; then
    echo "generated-header check: FAIL — flatc rejected libpcomm/schema/pcomm.fbs"
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
    echo "  [XX ] $1 — committed output is not what pcomm.fbs generates"
    echo "        regenerate: $4"
    sed -n '1,40p' "$tmp/drift.diff"
    lines=$(wc -l < "$tmp/drift.diff")
    if (( lines > 40 )); then echo "        ... $((lines - 40)) more diff lines"; fi
    rc=1
}

echo "generated-code check vs libpcomm/schema/pcomm.fbs ($("$flatc" --version))"
check_tree "generated/pcomm_generated.h" \
           "$root/libpcomm/schema/generated/pcomm_generated.h" \
           "$tmp/cpp/pcomm_generated.h" \
           "cd libpcomm/schema && flatc --cpp -o generated/ pcomm.fbs"
check_tree "generated-python/" \
           "$root/libpcomm/schema/generated-python" \
           "$tmp/py" \
           "cd libpcomm/schema && flatc --python -o generated-python/ pcomm.fbs"

if (( rc )); then echo "FAIL"; else echo "PASS — generated code is current"; fi
exit $rc
