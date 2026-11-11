#!/usr/bin/env bash
# The ucx_deep provider's record gate — UCX's TRANSPORT layer, with no cluster.
#
# WHAT IT IS DEFENDING. Everything this provider records comes through
# uct_iface_ops_t, the dispatch table in uct/api/tl.h. That is an installed
# public header, so no offset is guessed — but it is UCX's transport-implementor
# API and carries no ABI promise, so the failure mode this gate exists for is
# not "a row is missing", it is "a trampoline went into the wrong slot". Two of
# the assertions below are aimed squarely at that:
#
#   - uct_binding says which UCX was loaded and whether the layout guard let us
#     patch it. An empty uct_send with accepted=0 is a REFUSAL and a capture
#     must never let it read as an application that sent nothing.
#   - the put's remote_addr is compared against the address the workload printed
#     for its own buffer. A trampoline reading the wrong slot would still
#     produce rows; it would not produce that number.
#
# It runs BOTH UCX providers in one process, which is also the first exercise of
# `-p ucx,ucx_deep` co-running that PLAN-20260815-1540's definition of done
# asked for and never got.
#
# Skips loudly (exit 0) when the pieces are absent, the same bargain
# tests/ucx_records.sh makes.
set -uo pipefail

COMSHU=${1:?usage: ucx_deep_records.sh <comshu> <selftest> <engine-so> <ucx-so> <ucx-deep-so>}
SELFTEST=${2:?usage: ucx_deep_records.sh <comshu> <selftest> <engine-so> <ucx-so> <ucx-deep-so>}
ENGINE=${3:?usage: ucx_deep_records.sh <comshu> <selftest> <engine-so> <ucx-so> <ucx-deep-so>}
UCX_SO=${4:?usage: ucx_deep_records.sh <comshu> <selftest> <engine-so> <ucx-so> <ucx-deep-so>}
DEEP_SO=${5:?usage: ucx_deep_records.sh <comshu> <selftest> <engine-so> <ucx-so> <ucx-deep-so>}

for f in "$COMSHU" "$SELFTEST" "$ENGINE" "$UCX_SO" "$DEEP_SO"; do
    [ -x "$f" ] || [ -f "$f" ] || { echo "SKIP: missing $f"; exit 0; }
done

WORK=$(mktemp -d "${TMPDIR:-/tmp}/comshu-uct-XXXXXX")
trap 'rm -rf "$WORK"' EXIT

# UCX_TLS IS PINNED, and for case 21's reason: pinning is not tuning. This test
# asserts further down that the transport set is exactly `self`, and it did not
# pin it — so it was green only on a machine with no fabric, and the assertion
# was really measuring "does this host have a NIC". On MN5's login node, which
# has six active mlx5 HCAs, UCX also picks rc_mlx5 and ud_mlx5 and the workload
# reports three in-progress sends: two failures that say nothing about the
# provider. It went unseen because run_on_mn5.sh builds the UCX configuration
# with -DCOMSHU_BUILD_TESTS=OFF, so this gate had never once run on the cluster.
#
# Pinning makes the test measure what it already claims to. The rc_mlx5 and
# rendezvous paths are NOT lost by doing so — they are case 21's job, on
# hardware, in an allocation, where the thresholds are pinned for the same
# reason.
COMSHU_OUTPUT_DIR="$WORK/cap" COMSHU_TRACE_MODE=full UCX_TLS=self \
    LD_PRELOAD="$ENGINE $UCX_SO $DEEP_SO" "$SELFTEST" > "$WORK/run.out" 2>&1
rc=$?
if [ "$rc" -eq 77 ]; then
    echo "SKIP: no usable UCX transport on this host"
    sed 's/^/  /' "$WORK/run.out"
    exit 0
fi
if [ "$rc" -ne 0 ]; then
    cat "$WORK/run.out"; echo "FAIL: the UCX workload itself failed"; exit 1
fi

# THE PROVIDER MUST NOT HAVE COMPLAINED. Both of these are things it prints when
# it declines to do its job, and a gate that reads only the rows would call a
# refusal green because the refusal path emits a perfectly well-formed capture
# with an empty uct_send. stderr is captured into run.out above for exactly this
# — never redirected away.
if grep -qE "REFUSING|ucx_deep: BUG|shadow table full" "$WORK/run.out"; then
    sed 's/^/  /' "$WORK/run.out"
    echo "FAIL: ucx_deep declined to instrument, or hit its own bug path"
    exit 1
fi

"$COMSHU" export "$WORK/cap" --csv "$WORK/csv" > "$WORK/export.out" 2>&1 || {
    cat "$WORK/export.out"; echo "FAIL: export failed"; exit 1
}
if grep -q "unknown to this build" "$WORK/export.out"; then
    cat "$WORK/export.out"
    echo "FAIL: the reader does not know a record this build's provider wrote"
    exit 1
fi

fail=0
csv() { tail -n +2 "$WORK/csv/$1.csv" 2>/dev/null; }
expect() {  # <label> <expected> <actual>
    if [ "$2" != "$3" ]; then
        echo "FAIL: $1 — expected [$2], got [$3]"
        fail=1
    fi
}

# --- the binding, which is why an empty uct_send is or is not a finding -------
expect "uct_binding rows" 1 "$(csv uct_binding | wc -l)"
expect "uct_binding accepted" 1 "$(csv uct_binding | awk -F, '{print $6}')"
expect "uct_binding not forced" 0 "$(csv uct_binding | awk -F, '{print $7}')"
# 50 slots x 8 bytes. Pinned because it is the one number that would move if the
# ops table gained or lost a member under us — the exact event the version guard
# is guessing about from a version string, checked here directly.
expect "sizeof(uct_iface_ops_t)" 400 "$(csv uct_binding | awk -F, '{print $5}')"
compiled=$(csv uct_binding | awk -F, '{print $3}')
runtime=$(csv uct_binding | awk -F, '{print $4}')
if [ "${compiled%.*}" != "${runtime%.*}" ]; then
    echo "NOTE: compiled against UCX $compiled, ran against $runtime — different minor versions."
    echo "  That is allowed inside the verified range and is worth seeing in the log."
fi

# --- the interfaces, and the fact only this family can state ------------------
# WHICH TRANSPORT carried the traffic. Nothing else in a capture says whether a
# UCX message left the node; comm_edges has been counting shared-memory traffic
# as fabric traffic for want of exactly this column.
ifaces=$(csv uct_iface | wc -l)
if [ "${ifaces:-0}" -lt 1 ]; then
    echo "FAIL: no uct_iface rows — uct_iface_open was never seen, so nothing was patched"
    fail=1
fi
# NOT an exact count: it is one interface per (transport, device) and this host's
# device list is not this test's business — a laptop with three network
# interfaces opens three tcp ifaces and a login node opens one.
self_row=$(csv uct_iface | awk -F, '$6=="self"')
if [ -z "$self_row" ]; then
    echo "FAIL: no interface named tl_name=self; the transport name is not being recorded"
    echo "  (params->mode.device.tl_name, gated by UCT_IFACE_PARAM_FIELD_DEVICE)"
    fail=1
else
    expect "self interface instrumented" "yes" "$(echo "$self_row" | awk -F, '{print $9}')"
    patched=$(echo "$self_row" | awk -F, '{print $10}')
    if [ "${patched:-0}" -lt 1 ]; then
        echo "FAIL: self interface reports $patched patched data slots"
        fail=1
    fi
fi
# Every interface must claim a device name too, or half the join is missing.
blank_dev=$(csv uct_iface | awk -F, '$7=="" {c++} END{print c+0}')
expect "interfaces with no dev_name" 0 "$blank_dev"

# --- referential integrity ---------------------------------------------------
# THE assertion this provider lives or dies by. A uct_send naming an endpoint no
# record created is the "entity referenced by events that nothing opened" defect
# ib_hooks.hpp spent a week on, and it is exactly what would happen if ep_create
# were hooked at the exported uct_ep_create symbol instead of at the ops slot —
# transports create endpoints of their own that never pass the public symbol.
orphan_ep=$(join -v1 -t, -1 1 -2 1 \
    <(csv uct_send | awk -F, '{print $4}' | sort -u) \
    <(csv uct_ep   | awk -F, '{print $3}' | sort -u) | tr '\n' ' ')
expect "uct_send rows naming an unknown endpoint" "" "$orphan_ep"
orphan_if=$(join -v1 -t, -1 1 -2 1 \
    <(csv uct_send  | awk -F, '{print $5}' | sort -u) \
    <(csv uct_iface | awk -F, '{print $3}' | sort -u) | tr '\n' ' ')
expect "uct_send rows naming an unknown interface" "" "$orphan_if"
orphan_ep_if=$(join -v1 -t, -1 1 -2 1 \
    <(csv uct_ep    | awk -F, '{print $4}' | sort -u) \
    <(csv uct_iface | awk -F, '{print $3}' | sort -u) | tr '\n' ' ')
expect "uct_ep rows naming an unknown interface" "" "$orphan_ep_if"

# --- entity accounting -------------------------------------------------------
# Balanced exactly, both ways. An entity opened and never closed stays live
# forever and the allocator re-uses the address, which is the defect
# ibv_close_device exists to prevent; a close with no open is the reverse and
# says a creation door is unhooked.
expect "uct_iface opens == closes" \
    "$(csv uct_iface | wc -l)" "$(csv entity_close | awk -F, '$4==800' | wc -l)"
expect "uct_ep opens == closes" \
    "$(csv uct_ep | wc -l)" "$(csv entity_close | awk -F, '$4==802' | wc -l)"

# --- the data plane itself ---------------------------------------------------
sends=$(csv uct_send | wc -l)
if [ "${sends:-0}" -lt 1 ]; then
    echo "FAIL: uct_send is EMPTY. The whole point of this provider is that this table"
    echo "  is non-empty on a stack where ib_post_send and mlx5_device_send are not."
    fail=1
fi
# Each of the three eager-short tagged sends must appear with its EXACT
# application byte count. This is the pair of facts that makes uct_send
# readable: UCX's eager header travels in the ops signature's uint64 `header`,
# so `payload` on an am_short is the application's bytes and nothing else.
#
# The fourth UCP send (24 bytes, ucp_tag_send_sync_nb) is deliberately NOT
# pinned: it came out as an am_bcopy, whose length is whatever the pack callback
# produced, and which protocol UCX picks for a synchronous send is a
# configuration fact rather than a property of this provider.
for n in 32 16 48; do
    got=$(csv uct_send | awk -F, -v n="$n" '$3=="am_short" && $6==n' | wc -l)
    if [ "${got:-0}" -lt 1 ]; then
        echo "FAIL: no am_short carrying $n bytes; the $n-byte ucp_tag_send has no wire row"
        fail=1
    fi
done
# uct_ep_am_short's header is a uint64 BY SIGNATURE, so this is 8 on every row
# or the field is being filled from the wrong argument.
bad_hdr=$(csv uct_send | awk -F, '$3=="am_short" && $7!=8' | wc -l)
expect "am_short rows with header != 8" 0 "$bad_hdr"
# No HW tag matching on a shared-memory transport, so every tag here must be 0.
# 0 is the honest value: these doors carry no tag at all, and a non-zero one
# would mean the field is being filled from an unrelated argument.
bad_tag=$(csv uct_send | awk -F, '$9!="0x0"' | wc -l)
expect "uct_send rows with a non-zero tag on a transport with no HW TM" 0 "$bad_tag"
# Likewise remote_addr on the doors that have none.
bad_ra=$(csv uct_send | awk -F, '$3 ~ /^am_/ && $8!="0x0"' | wc -l)
expect "am_* rows carrying a remote address" 0 "$bad_ra"
# Nothing here failed, so no row may claim it did.
bad_st=$(csv uct_send | awk -F, '$11!=0' | wc -l)
expect "uct_send rows with a non-zero status in a passing workload" 0 "$bad_st"

# --- THE cross-layer assertion, and the only exact one available locally ------
# The workload prints the address it registered and then puts 8 bytes at it. A
# provider reading the wrong slot of the ops table would still emit rows; it
# would not emit this address. It is also the object-heatmap key that the whole
# HPC-X class has been missing — ib_post_send.remote_addr's equivalent, for a
# stack where ib_post_send is empty.
target=$(awk '/rma_target/ {print $3}' "$WORK/run.out")
if [ -z "$target" ]; then
    echo "FAIL: the workload did not report its RMA target address"
    fail=1
else
    puts=$(csv uct_send | awk -F, '$3 ~ /^put_/')
    if [ -z "$puts" ]; then
        echo "FAIL: no put_* row — ucp_put_nbx reached the wire through no UCT put slot"
        fail=1
    else
        expect "put payload"     8          "$(echo "$puts" | awk -F, '{print $6}')"
        expect "put remote_addr" "$target"  "$(echo "$puts" | awk -F, '{print $8}')"
    fi
fi

# --- the two layers agree ----------------------------------------------------
# Every UCP send became at least one UCT operation. The inequality is the point:
# they are NOT the same counter, and PLAN-20260815-1540 is explicit that UCP
# call bytes and wire bytes must never be summed together. This asserts the
# direction of the relation without pretending it is an equality.
ucp_sends=$(csv ucx_tag_send | wc -l)
if [ "${sends:-0}" -lt "${ucp_sends:-0}" ]; then
    echo "FAIL: $sends uct_send rows under $ucp_sends ucx_tag_send rows — a UCP send that"
    echo "  reached the wire through no UCT operation at all is not possible"
    fail=1
fi
# And the two providers really did co-run in one process, which is the thing
# PLAN-20260815-1540's definition of done listed as UNTESTED.
if [ "${ucp_sends:-0}" -lt 1 ]; then
    echo "FAIL: ucx_tag_send is empty — the two UCX providers did not co-run"
    fail=1
fi

# --- which transport carried it, stated rather than assumed ------------------
# The headline capability. Resolve every send's interface to its transport name
# and require the answer to be a name, not a blank.
tls=$(join -t, -1 1 -2 1 \
    <(csv uct_send  | awk -F, '{print $5}' | sort -u) \
    <(csv uct_iface | awk -F, '{print $3","$6}' | sort -u) | cut -d, -f2 | sort -u | tr '\n' ' ')
expect "transports that carried this workload" "self " "$tls"

if [ "$fail" -ne 0 ]; then
    echo
    echo "--- uct_binding ---"; csv uct_binding
    echo "--- uct_iface ---";   csv uct_iface
    echo "--- uct_ep ---";      csv uct_ep
    echo "--- uct_send ---";    csv uct_send
    exit 1
fi

echo "ok: $ifaces interfaces (all closed), $(csv uct_ep | wc -l) lanes, $sends UCT operations"
echo "    under $ucp_sends UCP sends, all over transport '${tls% }', put lands at $target"
