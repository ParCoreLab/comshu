#!/usr/bin/env bash
# The UCX provider's first regression gate, and the first test of it that does
# not need a cluster.
#
# Everything the provider recorded used to be checked by reading MN5 job output
# by hand — an allocation, a queue wait and two nodes to discover that a record
# was missing. UCX itself needs none of that: the self transport works in one
# process, so tests/ucx_selftest.cpp drives a fixed workload here and this asserts
# the rows that came out.
#
# What it is really defending, in order of how expensive each was to find:
#
#  1. THE DOUBLE COUNT. UCX implements its legacy doors on top of the modern
#     ones, and because our interposers are global, ucp_tag_send_nb produced a
#     `nb` row AND an `nbx` row for one send — the ibv_reg_mr/iova2 defect
#     exactly. That is why the per-door counts below are exact equality and not
#     "at least": a >= assertion passes happily on a 2x inflation.
#  2. THE PROBE JOIN. ucp_tag_msg_recv_nb carries a message handle and no tag,
#     so its row is meaningless unless ucx_tag_probe can supply one. Asserted as
#     a real join, not as two independent row counts.
#  3. Balanced entity lifetimes, which is what the extra close doors were for.
#
# Skips loudly (exit 0) when the pieces are absent, the same bargain
# asserts_fire.sh makes with sqlite3.
set -uo pipefail

PCOMM=${1:?usage: ucx_records.sh <pcomm-binary> <selftest-binary> <engine-so> <provider-so>}
SELFTEST=${2:?usage: ucx_records.sh <pcomm-binary> <selftest-binary> <engine-so> <provider-so>}
ENGINE=${3:?usage: ucx_records.sh <pcomm-binary> <selftest-binary> <engine-so> <provider-so>}
PROVIDER=${4:?usage: ucx_records.sh <pcomm-binary> <selftest-binary> <engine-so> <provider-so>}

for f in "$PCOMM" "$SELFTEST" "$ENGINE" "$PROVIDER"; do
    [ -x "$f" ] || [ -f "$f" ] || { echo "SKIP: missing $f"; exit 0; }
done

WORK=$(mktemp -d "${TMPDIR:-/tmp}/pcomm-ucx-XXXXXX")
trap 'rm -rf "$WORK"' EXIT

# The engine must be FIRST in LD_PRELOAD or it is not the dlsym resolver and the
# capture is silently incomplete — the provider warns about this itself.
PCOMM_OUTPUT_DIR="$WORK/cap" PCOMM_TRACE_MODE=full \
    LD_PRELOAD="$ENGINE $PROVIDER" "$SELFTEST" > "$WORK/run.out" 2>&1
rc=$?
if [ "$rc" -eq 77 ]; then
    echo "SKIP: no usable UCX transport on this host"
    sed 's/^/  /' "$WORK/run.out"
    exit 0
fi
if [ "$rc" -ne 0 ]; then
    cat "$WORK/run.out"; echo "FAIL: the UCX workload itself failed"; exit 1
fi

"$PCOMM" export "$WORK/cap" --csv "$WORK/csv" > "$WORK/export.out" 2>&1 || {
    cat "$WORK/export.out"; echo "FAIL: export failed"; exit 1
}
# A capture the reader does not fully understand is a failure here, not a
# warning: this test exists to catch exactly that.
if grep -q "unknown to this build" "$WORK/export.out"; then
    cat "$WORK/export.out"
    echo "FAIL: the reader does not know a record this build's provider wrote"
    exit 1
fi

fail=0
csv() { tail -n +2 "$WORK/csv/$1.csv" 2>/dev/null; }

expect() {  # <label> <expected> <actual>
    if [ "$2" != "$3" ]; then
        echo "FAIL: $1 — expected $2, got $3"
        fail=1
    fi
}

# --- the doors, counted exactly ---------------------------------------------
# Exact, because the failure this guards against inflates rather than deletes.
# Four sends, not three: the probe arm below issues a legacy send of its own, so
# `nb` is expected twice. Written out because an off-by-one here is invisible —
# the numbers all look plausible.
expect "ucx_tag_send rows"            4 "$(csv ucx_tag_send | wc -l)"
expect "ucx_tag_send door=nbx"        1 "$(csv ucx_tag_send | cut -d, -f3 | grep -cx nbx)"
expect "ucx_tag_send door=nb"         2 "$(csv ucx_tag_send | cut -d, -f3 | grep -cx nb)"
expect "ucx_tag_send door=sync_nb"    1 "$(csv ucx_tag_send | cut -d, -f3 | grep -cx sync_nb)"
expect "ucx_tag_recv rows"            4 "$(csv ucx_tag_recv | wc -l)"
expect "ucx_tag_recv door=msg_recv_nb" 1 "$(csv ucx_tag_recv | cut -d, -f3 | grep -cx msg_recv_nb)"
expect "ucx_tag_probe rows"           1 "$(csv ucx_tag_probe | wc -l)"

# --- lifecycle, which the extra close doors exist to balance -----------------
expect "ucx_context rows"  1 "$(csv ucx_context | wc -l)"
expect "ucx_worker rows"   1 "$(csv ucx_worker | wc -l)"
expect "ucx_endpoint rows" 1 "$(csv ucx_endpoint | wc -l)"
# The endpoint is closed through ucp_ep_close_nb, which was NOT hooked until the
# door sweep. If that regresses, the close vanishes and this catches it.
closes=$(csv entity_close | wc -l)
if [ "${closes:-0}" -lt 3 ]; then
    echo "FAIL: expected at least 3 entity_close rows (context, worker, endpoint), got $closes"
    fail=1
fi

# --- the probe join, asserted as a join -------------------------------------
# Two independent row counts would pass even if the handles disagreed, which is
# the whole thing that makes a msg_recv row readable.
msg_handle=$(csv ucx_tag_recv | awk -F, '$3=="msg_recv_nb" {print $10}')
# NOTE the field numbers: every export carries a leading `rank` column that the
# Table<> `cols` string does not list, so these are one further right than the
# header reads. Getting this wrong compares a message handle against a worker
# handle and fails with two plausible-looking numbers.
probe_handle=$(csv ucx_tag_probe | awk -F, '{print $4}')
probe_tag=$(csv ucx_tag_probe | awk -F, '{print $5}')
probe_len=$(csv ucx_tag_probe | awk -F, '{print $6}')
if [ -z "$msg_handle" ] || [ "$msg_handle" = "0" ]; then
    echo "FAIL: the msg_recv row carries no message handle, so its tag is unrecoverable"
    fail=1
elif [ "$msg_handle" != "$probe_handle" ]; then
    echo "FAIL: probe/msg_recv handles disagree — recv says $msg_handle, probe says $probe_handle"
    echo "  the join in UcxTagRecv.message is the only way that row recovers its tag"
    fail=1
else
    expect "probe sender_tag" "0xabcd" "$probe_tag"
    expect "probe length (the one true byte count)" "48" "$probe_len"
fi

# --- peer identity, which the self transport can check exactly ---------------
# The workload dials its OWN worker, so the endpoint's remote_prefix_hash must
# equal that worker's prefix_hash. That is the whole cross-rank join mechanism
# exercised in one process: on a real run the two sides live in different
# captures and there is nothing local to compare them against.
expect "ucx_worker_address rows" 1 "$(csv ucx_worker_address | wc -l)"
expect "ucx_ep_connection rows"  1 "$(csv ucx_ep_connection | wc -l)"
worker_prefix=$(csv ucx_worker_address | awk -F, '{print $5}')
ep_remote=$(csv ucx_ep_connection | awk -F, '{print $4}')
if [ -z "$worker_prefix" ] || [ "$worker_prefix" != "$ep_remote" ]; then
    echo "FAIL: peer identity does not resolve — worker prefix_hash $worker_prefix,"
    echo "  endpoint remote_prefix_hash $ep_remote, and this workload dials itself"
    echo "  so they must be equal. A cross-rank join would silently miss instead."
    fail=1
fi
# The two worker hashes must DIFFER: they cover different byte ranges, and equal
# values would mean one of them is not being computed over what it claims.
worker_full=$(csv ucx_worker_address | awk -F, '{print $4}')
if [ "$worker_full" = "$worker_prefix" ]; then
    echo "FAIL: addr_hash and prefix_hash are identical ($worker_full) — one of"
    echo "  them is not hashing the range it claims, so collision detection is blind"
    fail=1
fi

# A msg_recv row must NOT invent a tag: 0 is the honest value there, and a
# non-zero one would mean somebody filled the field by guessing.
msg_tag=$(csv ucx_tag_recv | awk -F, '$3=="msg_recv_nb" {print $8}')
expect "msg_recv tag is not invented" "0x0" "$msg_tag"

if [ "$fail" -ne 0 ]; then
    echo
    echo "--- ucx_tag_send ---";  csv ucx_tag_send
    echo "--- ucx_tag_recv ---";  csv ucx_tag_recv
    echo "--- ucx_tag_probe ---"; csv ucx_tag_probe
    exit 1
fi

echo "ok: 4 sends over 3 doors, 4 receives, probe->msg_recv join intact (tag $probe_tag, $probe_len bytes),"
echo "    peer identity resolves ($worker_prefix)"
