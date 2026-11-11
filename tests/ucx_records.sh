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
#  4. THE REMOTE KEY. ucx_rma and ucx_atomic each intern a ucp_rkey_h, and until
#     ucx_rkey existed nothing recorded one being created — so the handle on
#     every one-sided row joined to nothing. Asserted as a join for the same
#     reason as the probe, and with its close, because the lifetime is what
#     keeps a reallocated rkey pointer from reading as the one before it.
#
# Skips loudly (exit 0) when the pieces are absent, the same bargain
# asserts_fire.sh makes with sqlite3.
set -uo pipefail

COMSHU=${1:?usage: ucx_records.sh <comshu-binary> <selftest-binary> <engine-so> <provider-so>}
SELFTEST=${2:?usage: ucx_records.sh <comshu-binary> <selftest-binary> <engine-so> <provider-so>}
ENGINE=${3:?usage: ucx_records.sh <comshu-binary> <selftest-binary> <engine-so> <provider-so>}
PROVIDER=${4:?usage: ucx_records.sh <comshu-binary> <selftest-binary> <engine-so> <provider-so>}

for f in "$COMSHU" "$SELFTEST" "$ENGINE" "$PROVIDER"; do
    [ -x "$f" ] || [ -f "$f" ] || { echo "SKIP: missing $f"; exit 0; }
done

WORK=$(mktemp -d "${TMPDIR:-/tmp}/comshu-ucx-XXXXXX")
trap 'rm -rf "$WORK"' EXIT

# The engine must be FIRST in LD_PRELOAD or it is not the dlsym resolver and the
# capture is silently incomplete — the provider warns about this itself.
COMSHU_OUTPUT_DIR="$WORK/cap" COMSHU_TRACE_MODE=full \
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

"$COMSHU" export "$WORK/cap" --csv "$WORK/csv" > "$WORK/export.out" 2>&1 || {
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

# --- where the buffer lived -------------------------------------------------
#
# On the HPC-X stack UCX is often the ONLY layer that saw the traffic at all, so
# a question this record cannot answer is one the whole capture cannot answer.
# tests/hpc/apps/01_gromacs had to reach its central negative -- that UCX
# pipelined through host memory rather than using GPUDirect -- by INFERRING it
# from registration counts, because ucx_tag_send carried no memory type.
#
# This workload has no CUDA at all, and core::device_of uses RTLD_NOLOAD
# precisely so it never loads it into a process that declined it. So every send
# here must read kUnknownDevice (65535). The assertion is that the column is
# present and carries that value on every row -- a field wired up wrong shows as
# 0 (a real GPU ordinal, and a confident lie) or as an empty column.
#
# WHAT THIS CANNOT CHECK, measured rather than assumed: passing device_of the
# WRONG pointer still passes here. With no CUDA in the process every address
# answers kUnknownDevice, so this gate distinguishes "wired up" from
# "fabricated" and cannot distinguish "the buffer" from "any pointer". Proving
# that needs a process holding a real device allocation, which is hardware and
# belongs in tests/hpc. Mutating device_of(buffer) to a literal 0 turns this
# red; mutating it to device_of(nullptr) does not.
dev_col=$(csv ucx_tag_send | awk -F, '{print $11}' | sort -u | tr '\n' ' ')
expect "ucx_tag_send device is kUnknownDevice with no CUDA" "65535 " "$dev_col"
rma_dev=$(csv ucx_rma | awk -F, '{print $13}' | sort -u | tr '\n' ' ')
if [ -n "$(csv ucx_rma)" ]; then
    expect "ucx_rma device is kUnknownDevice with no CUDA" "65535 " "$rma_dev"
fi

# --- the remote key, which every RMA row named and nothing created -----------
#
# ucx_rma.rkey and ucx_atomic.rkey interned a ucp_rkey_h that no record opened,
# so the handle joined to nothing — the same defect ib_hooks.hpp spent a week on
# in the verbs provider. THE ASSERTION IS THE JOIN, not the row counts: two
# independent counts pass happily while the handles disagree, which is the only
# thing that makes an RMA row's target readable.
#
# Guarded on ucx_rma rather than asserted unconditionally, because the workload
# reaches the rkey doors only inside its RMA arm and that arm is itself
# conditional on the transport supporting registration. The invariant asserted
# is the implication: if an RMA row exists, its key must resolve.
if [ -z "$(csv ucx_rma)" ]; then
    echo "note: the self transport did not take the RMA arm — rkey rows not asserted"
else
    expect "ucx_rkey rows"      1 "$(csv ucx_rkey | wc -l)"
    expect "ucx_rkey_pack rows" 1 "$(csv ucx_rkey_pack | wc -l)"

    rkey_handle=$(csv ucx_rkey | awk -F, '{print $3}')
    rma_rkeys=$(csv ucx_rma | awk -F, '{print $8}' | sort -u)
    if [ -z "$rkey_handle" ]; then
        echo "FAIL: an RMA row names an rkey and no ucx_rkey row created one"
        fail=1
    elif [ "$rma_rkeys" != "$rkey_handle" ]; then
        echo "FAIL: rkey handles disagree — ucx_rma names [$rma_rkeys], ucx_rkey created $rkey_handle"
        echo "  that join is the only way an RMA row says which peer region it targeted"
        fail=1
    fi

    # The key must resolve to the endpoint it was unpacked for. That endpoint is
    # what carries the peer, via ucx_ep_connection — the chain that replaces the
    # cross-rank rkey-buffer join the public API cannot support.
    rkey_ep=$(csv ucx_rkey | awk -F, '{print $4}')
    the_ep=$(csv ucx_endpoint | awk -F, '{print $3}')
    if [ "$rkey_ep" != "$the_ep" ]; then
        echo "FAIL: ucx_rkey names endpoint $rkey_ep, but the only endpoint is $the_ep"
        fail=1
    fi

    # The lifetime, which is the whole reason this is an Entity: without a close,
    # a reallocated rkey pointer reads as the one before it.
    rkey_closes=$(csv entity_close | awk -F, '$4=="626" {print $3}')
    if [ "$rkey_closes" != "$rkey_handle" ]; then
        echo "FAIL: ucp_rkey_destroy left no close for $rkey_handle (opener 626 closes: [$rkey_closes])"
        fail=1
    fi

    # The exporting side joins the registration it exported. A pack row whose
    # memh matches no ucx_mem_map row would be naming a handle from nowhere.
    pack_memh=$(csv ucx_rkey_pack | awk -F, '{print $3}')
    if [ -z "$pack_memh" ] || ! csv ucx_mem_map | awk -F, '{print $3}' | grep -qx "$pack_memh"; then
        echo "FAIL: ucx_rkey_pack.memh $pack_memh matches no ucx_mem_map row"
        fail=1
    fi

    # A packed rkey is at least UCX's own dummy key (md_map + mem_type). Zero
    # would mean the size out-parameter was read before it was set, and the hash
    # beside it would then cover nothing.
    # $6, not $5: the row is rank,ticks,memh,context,buffer_hash,buffer_len.
    # Reading $5 compared the HASH against 9, `[` errored on the hex, and an
    # erroring test is a false one — so this check never ran.
    pack_len=$(csv ucx_rkey_pack | awk -F, '{print $6}')
    if [ -z "$pack_len" ] || [ "$pack_len" -lt 9 ]; then
        echo "FAIL: ucx_rkey_pack.buffer_len is $pack_len — smaller than UCX's own dummy key"
        fail=1
    fi
fi

# --- peer identity, which the self transport can check exactly ---------------
# The workload dials its OWN worker, so the endpoint's remote_prefix_hash must
# equal that worker's prefix_hash. That is the whole cross-rank join mechanism
# exercised in one process: on a real run the two sides live in different
# captures and there is nothing local to compare them against.
# TWO rows, and the number is the assertion. The workload asks for its address
# through BOTH doors — ucp_worker_query and ucp_worker_get_address — because a
# provider hooking one of them is indistinguishable from a provider hooking both
# until an application picks the other. That is what happened: OpenMPI's
# point-to-point path uses only ucp_worker_query, and three of the four studies
# in tests/hpc/apps/ independently found ucx_worker_address describing the wrong
# worker for every MPI application. This gate could not see it, because the
# workload called the door the provider happened to hook.
#
# THREE would be a different bug and is why the workload issues a third query
# with UCP_WORKER_ATTR_FIELD_ADDRESS *absent*. ucp_worker_query fills only what
# the caller asks for, so a provider that reads attr->address without checking
# the field mask hashes uninitialised stack — deterministically, and into a
# plausible-looking join key that matches nothing. The workload poisons the
# struct with 0xA5 first, so that bug cannot hide behind a zeroed field.
expect "ucx_worker_address rows" 2 "$(csv ucx_worker_address | wc -l)"
expect "ucx_ep_connection rows"  1 "$(csv ucx_ep_connection | wc -l)"
# The two doors describe ONE worker and one address, so they must agree. If they
# disagree, one of them is hashing a different byte range or a different buffer,
# and a capture would carry two irreconcilable identities for the same worker.
distinct_prefix=$(csv ucx_worker_address | awk -F, '{print $5}' | sort -u | wc -l)
expect "both address doors agree on prefix_hash" 1 "$distinct_prefix"
worker_prefix=$(csv ucx_worker_address | awk -F, '{print $5}' | sort -u)
ep_remote=$(csv ucx_ep_connection | awk -F, '{print $4}')
if [ -z "$worker_prefix" ] || [ "$worker_prefix" != "$ep_remote" ]; then
    echo "FAIL: peer identity does not resolve — worker prefix_hash $worker_prefix,"
    echo "  endpoint remote_prefix_hash $ep_remote, and this workload dials itself"
    echo "  so they must be equal. A cross-rank join would silently miss instead."
    fail=1
fi
# The two worker hashes must DIFFER: they cover different byte ranges, and equal
# values would mean one of them is not being computed over what it claims.
worker_full=$(csv ucx_worker_address | awk -F, '{print $4}' | sort -u)
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
