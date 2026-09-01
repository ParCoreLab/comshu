#!/bin/bash
# asserts_fire.sh — a mutation test for the cross-cutting assertions.
#
#   asserts_fire.sh            # runs every scenario, prints one line each
#
# WHY THIS EXISTS. suite/asserts/*.sql are the instruments that have found every
# missing hook in this project, and the failure mode of an instrument is not
# "wrong answer", it is "no answer". An assertion that has been relaxed until the
# board went green cannot be told apart from one that is passing, because both
# print nothing. So each relaxation is paired here with a synthetic capture that
# still contains the defect the assertion was written for, and the test fails if
# the assertion stays silent.
#
# The fixtures are hand-built CSV tables, not a recorded capture: a capture can
# only contain the bugs that happened, and the point is to inject the ones that
# did not. They are tiny on purpose — every column that is not load-bearing for
# the assertion under test is a constant.
#
# PORTABILITY. MN5's sqlite is 3.34 and rejects HAVING without GROUP BY, which
# 3.51 accepts; nothing here or in the assertions may rely on that. This script
# only runs the assertion files as-is, so it inherits whatever they use — which
# is one more reason to run it before submitting.
set -u
SUITE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SQL_ENGINE="${SQL_ENGINE:-sqlite3}"
command -v "$SQL_ENGINE" >/dev/null || { echo "asserts_fire: no $SQL_ENGINE — SKIPPED"; exit 0; }

fail=0

# Every table entity_close_accounted.sql unions over, so a scenario declares
# only the rows it cares about and the rest are present and empty. An absent
# table is a SQL error, which sql_check.sh would report as a violation — a pass
# for the wrong reason, and exactly what this file is here to prevent.
schema() {
  cat <<'SQL'
CREATE TABLE entity_close (rank INT, ticks INT, subject INT, opener INT);
CREATE TABLE ib_mem_region (rank INT, mr INT);
CREATE TABLE ib_queue_pair (rank INT, qp INT);
CREATE TABLE ib_completion_queue (rank INT, cq INT);
CREATE TABLE ib_shared_recv_queue (rank INT, srq INT);
CREATE TABLE ib_address_handle (rank INT, ah INT);
CREATE TABLE ib_device (rank INT, context INT);
CREATE TABLE mlx5_queue_pair (rank INT, qp INT);
CREATE TABLE mlx5_umem (rank INT, umem INT);
CREATE TABLE mlx5_uar (rank INT, uar INT);
CREATE TABLE mem_object (rank INT, id INT);
CREATE TABLE ucx_context (rank INT, context INT);
CREATE TABLE ucx_worker (rank INT, worker INT);
CREATE TABLE ucx_endpoint (rank INT, endpoint INT);
CREATE TABLE ucx_mem_map (rank INT, memh INT);
CREATE TABLE ucx_rkey (rank INT, rkey INT);
CREATE TABLE nccl_comm (rank INT, comm INT);
CREATE TABLE nccl_window (rank INT, window INT);
CREATE TABLE uct_iface (rank INT, iface INT);
CREATE TABLE uct_ep (rank INT, ep INT);
SQL
}

# run <name> <expect: quiet|fires> <rows-sql>
run() {
  name="$1"; expect="$2"; rows="$3"
  db="$(mktemp)"
  { schema; printf '%s\n' "$rows"; } | "$SQL_ENGINE" "$db"
  out="$("$SQL_ENGINE" -csv "$db" < "$SUITE_DIR/asserts/entity_close_accounted.sql" 2>&1)"
  rm -f "$db"
  if [ "$expect" = quiet ] && [ -n "$out" ]; then
    echo "  [XX ] $name — expected no rows, got:"; printf '%s\n' "$out" | sed 's/^/        /'; fail=1
  elif [ "$expect" = fires ] && [ -z "$out" ]; then
    echo "  [XX ] $name — the assertion did NOT fire on a real defect"; fail=1
  else
    echo "  [ok ] $name ($expect)"
  fi
}

echo "-- entity_close_accounted"

# The relaxation this file was added for: mlx5_queue_pair is an open record like
# any other, so a QP created through mlx5dv_create_qp and destroyed through
# ibv_destroy_qp is accounted. Note the close carries opener 102 (ib_queue_pair)
# while the open is a mlx5 row — the join is on subject, not on opener, and this
# scenario is what says so. Captures written before the engine tracked opened QP
# handles all look like this, so it stays.
run "mlx5dv-created QP, destroyed through ibverbs, is accounted" quiet "
INSERT INTO mlx5_queue_pair VALUES (0, 7);
INSERT INTO entity_close VALUES (0, 10, 7, 102);"

# ...and the same QP as a current engine records it: the mlx5dv provider
# registered the handle at create, so the close names 210, the door that made it.
# Also quiet — which is the point of joining on subject and not on opener.
run "mlx5dv-created QP closed with its own opener (210) is accounted" quiet "
INSERT INTO mlx5_queue_pair VALUES (0, 7);
INSERT INTO entity_close VALUES (0, 10, 7, 210);"

# ...and the relaxation does not blind the clause. Same shape, but nothing
# opened this one: still the signature of a creation door we do not hook.
run "QP close naming a handle NO table opened still fires" fires "
INSERT INTO mlx5_queue_pair VALUES (0, 7);
INSERT INTO entity_close VALUES (0, 10, 9, 102);"

# The defect that produced this investigation: a CQ created through a door we
# do not hook (mlx5dv_create_cq, before it was hooked) and destroyed through
# ibv_destroy_cq. Both of its signatures, because the allocator decides which
# one a given CQ shows up as.
run "CQ close naming a fresh handle nothing opened fires" fires "
INSERT INTO ib_completion_queue VALUES (0, 3);
INSERT INTO entity_close VALUES (0, 10, 3, 104);
INSERT INTO entity_close VALUES (0, 20, 99, 104);"

run "CQ closed twice, opened once, fires" fires "
INSERT INTO ib_completion_queue VALUES (0, 3);
INSERT INTO entity_close VALUES (0, 10, 3, 104);
INSERT INTO entity_close VALUES (0, 20, 3, 104);"

# The other direction: a destruction door we do not hook.
run "region opened and never closed fires" fires "
INSERT INTO ib_mem_region VALUES (0, 5);"

# Address reuse is legitimate and must NOT fire. A handle is an interned
# pointer, so a freed CQ's address comes back as the SAME handle; opens and
# closes both increment and the per-handle totals stay equal. This is why the
# clauses count rather than window — and this row is what would catch a
# "fix" that made the assertion window-based and lost the count.
run "handle reused three times, closed three times, is quiet" quiet "
INSERT INTO ib_completion_queue VALUES (0, 3), (0, 3), (0, 3);
INSERT INTO entity_close VALUES (0, 10, 3, 104), (0, 20, 3, 104), (0, 30, 3, 104);"

# Handles are per-process, so rank 3's handle 3 is not rank 0's. A close on a
# rank that opened nothing is a violation even when another rank did open it.
run "same handle, different rank, is not a match" fires "
INSERT INTO ib_completion_queue VALUES (0, 3);
INSERT INTO entity_close VALUES (0, 10, 3, 104);
INSERT INTO entity_close VALUES (1, 10, 3, 104);"

# The measured exemptions, kept honest: ib_device and mem_object are exempt from
# the never-closed clause ONLY, and are still fully covered by the other two.
run "unclosed ib_device is exempt (measured; see the assertion)" quiet "
INSERT INTO ib_device VALUES (0, 1);"
run "ib_device closed twice still fires" fires "
INSERT INTO ib_device VALUES (0, 1);
INSERT INTO entity_close VALUES (0, 10, 1, 120), (0, 20, 1, 120);"

# UCX. These four openers were absent from the assertion's `opens` CTE for the
# whole life of the UCX provider, which turned every one of case 19's closes
# into a spurious "names an entity no open record created" — 24 rows on a
# capture whose opens and closes matched exactly. That is this file's stated
# failure mode ("a pass for the wrong reason") arriving as its mirror: noise for
# the wrong reason, loud enough to hide a real defect behind it.
run "UCX context opened and closed is quiet" quiet "
INSERT INTO ucx_context VALUES (0, 1);
INSERT INTO entity_close VALUES (0, 10, 1, 600);"

run "UCX worker opened and closed is quiet" quiet "
INSERT INTO ucx_worker VALUES (0, 2);
INSERT INTO entity_close VALUES (0, 10, 2, 602);"

# The defect the noise was hiding, in miniature. UCX reaches its own public close
# symbols through one another and all four doors are interposed, so one teardown
# emitted two or three EntityClose rows — measured on job 44698048, endpoint 4
# closed three times 1.9 us and 0.8 us apart. With the openers absent this could
# not be seen: the closes-outnumber-opens clause had no denominator to compare
# against.
run "UCX endpoint closed three times by nested doors fires" fires "
INSERT INTO ucx_endpoint VALUES (0, 4);
INSERT INTO entity_close VALUES (0, 10, 4, 604), (0, 11, 4, 604), (0, 12, 4, 604);"

run "UCX endpoint closed through a door we do not hook still fires" fires "
INSERT INTO entity_close VALUES (0, 10, 99, 604);"

# The remote key, 626, added with the record rather than after it — which is the
# whole lesson of the four paragraphs above, learned again on job 45177565. That
# run recorded eight rkeys, closed all eight correctly, and reported all eight as
# orphans because the CTE had no ucx_rkey line. Delete that line again and the
# first TWO of these turn red at once — measured, not assumed.
#
# The reuse case is the one that matters for THIS entity specifically. A
# ucp_rkey_h is a heap pointer UCX frees and reallocates, so an rkey address
# genuinely recurs within a run — and unlike the endpoint doors, that recurrence
# is legitimate. It must stay quiet when opens and closes are balanced, and fire
# only on a genuine imbalance.
run "UCX rkey unpacked and destroyed is quiet" quiet "
INSERT INTO ucx_rkey VALUES (0, 7);
INSERT INTO entity_close VALUES (0, 10, 7, 626);"

run "UCX rkey address reused across two lifetimes is quiet" quiet "
INSERT INTO ucx_rkey VALUES (0, 7), (0, 7);
INSERT INTO entity_close VALUES (0, 10, 7, 626), (0, 20, 7, 626);"

run "UCX rkey destroyed twice in one lifetime fires" fires "
INSERT INTO ucx_rkey VALUES (0, 7);
INSERT INTO entity_close VALUES (0, 10, 7, 626), (0, 20, 7, 626);"

run "UCX rkey destroyed through a door we do not hook still fires" fires "
INSERT INTO entity_close VALUES (0, 10, 99, 626);"

# NCCL, pinned the same way and for the same reason. These four exist so that
# the openers added to the CTE with the provider stay added: delete the
# nccl_comm line from `opens` and the first two of these turn red immediately,
# instead of the provider quietly producing a capture full of spurious rows.
run "NCCL communicator opened and closed is quiet" quiet "
INSERT INTO nccl_comm VALUES (0, 1);
INSERT INTO entity_close VALUES (0, 10, 1, 900);"

run "NCCL window registered and deregistered is quiet" quiet "
INSERT INTO nccl_window VALUES (0, 5);
INSERT INTO entity_close VALUES (0, 10, 5, 908);"

# ncclCommDestroy and ncclCommAbort are alternatives, not nested doors, so the
# provider suppresses neither. If a stack ever does reach one through the other,
# this is the row that says so rather than a silent double count.
run "NCCL comm destroyed and aborted fires" fires "
INSERT INTO nccl_comm VALUES (0, 1);
INSERT INTO entity_close VALUES (0, 10, 1, 900), (0, 11, 1, 900);"

# The signal the whole assertion exists for, on the newest provider: a
# communicator built through a creation door we do not hook — ncclCommSplit,
# ncclCommInitAll — must still be reported.
run "NCCL comm closed through a door we do not hook still fires" fires "
INSERT INTO entity_close VALUES (0, 10, 77, 900);"

# ucx_deep's two, pinned the same way — and these are not hypothetical. The
# provider landed emitting EntityClose for both entity types and adding neither
# to the CTE, so case 21's first run (job 45114566) reported 296 iface closes and
# 168 endpoint closes as unmatched while opens and closes agreed exactly on both
# ranks. That is the third time this list has been left behind by a provider, and
# these four rows are what turn the fourth time into a local red instead of a
# cluster job's worth of noise.
run "UCT interface opened and closed is quiet" quiet "
INSERT INTO uct_iface VALUES (0, 3);
INSERT INTO entity_close VALUES (0, 10, 3, 800);"

run "UCT endpoint opened and closed is quiet" quiet "
INSERT INTO uct_ep VALUES (0, 7);
INSERT INTO entity_close VALUES (0, 10, 7, 802);"

# An iface pointer IS reusable — UCX closes one and opens another at the same
# address — so two opens and two closes of one handle is the legitimate shape,
# and only an excess of closes is a defect. This is the pair that says the
# running count may reach zero and must not go below it.
run "UCT interface reopened at the same address is quiet" quiet "
INSERT INTO uct_iface VALUES (0, 3), (0, 3);
INSERT INTO entity_close VALUES (0, 10, 3, 800), (0, 11, 3, 800);"

run "UCT interface closed more often than opened fires" fires "
INSERT INTO uct_iface VALUES (0, 3);
INSERT INTO entity_close VALUES (0, 10, 3, 800), (0, 11, 3, 800);"

# The signal the assertion exists for, on the layer where it is most likely:
# ucx_deep learns of an interface by patching the ops table at iface_open, so an
# interface UCX creates through any other path is exactly the missing creation
# door this row is here to name.
run "UCT interface closed through a door we do not hook still fires" fires "
INSERT INTO entity_close VALUES (0, 10, 88, 800);"

# ---------------------------------------------------------------------------
# comm_edges_scope_agrees_with_topology
#
# Its own schema and its own runner, because the harness above is written around
# entity_close_accounted's tables and this assertion shares none of them. Two
# relations is all it needs: the matrix, and the per-rank node ordinal it must
# agree with.
scope_schema() {
  cat <<'SQL'
CREATE TABLE comm_edges (src_rank INT, dst_rank INT, transport TEXT, unit TEXT,
                         scope TEXT, sends INT, bytes INT);
CREATE TABLE rank_identity (rank INT, node_ix INT, node_hash INT, device INT,
                            devices INT, label TEXT);
SQL
}

# run_scope <name> <expect: quiet|fires> <rows-sql>
run_scope() {
  name="$1"; expect="$2"; rows="$3"
  db="$(mktemp)"
  { scope_schema; printf '%s\n' "$rows"; } | "$SQL_ENGINE" "$db"
  out="$("$SQL_ENGINE" -csv "$db" < "$SUITE_DIR/asserts/comm_edges_scope_agrees_with_topology.sql" 2>&1)"
  rm -f "$db"
  if [ "$expect" = quiet ] && [ -n "$out" ]; then
    echo "  [XX ] $name — expected no rows, got:"; printf '%s\n' "$out" | sed 's/^/        /'; fail=1
  elif [ "$expect" = fires ] && [ -z "$out" ]; then
    echo "  [XX ] $name — the assertion did NOT fire on a real defect"; fail=1
  else
    echo "  [ok ] $name ($expect)"
  fi
}

echo
echo "-- comm_edges_scope_agrees_with_topology"

# Two ranks on one node, called intra-node. The shape OSU produced on its self
# edges and GROMACS produced 83,892 times.
run_scope "same node, called intra-node, is quiet" quiet "
INSERT INTO rank_identity VALUES (0,0,11,0,1,'n0'),(1,0,11,1,1,'n0');
INSERT INTO comm_edges VALUES (0,1,'ucx','datatype-units','intra-node',5,500);"

# Two ranks on different nodes, called network.
run_scope "different nodes, called network, is quiet" quiet "
INSERT INTO rank_identity VALUES (0,0,11,0,1,'n0'),(1,1,22,0,1,'n1');
INSERT INTO comm_edges VALUES (0,1,'ucx','datatype-units','network',5,500);"

# CLAUSE 1 — the defect the column was added for. Before `scope` existed every
# such edge read as fabric and there was no way to say otherwise.
run_scope "same node but called network fires" fires "
INSERT INTO rank_identity VALUES (0,0,11,0,1,'n0'),(1,0,11,1,1,'n0');
INSERT INTO comm_edges VALUES (0,1,'ucx','datatype-units','network',5,500);"

# CLAUSE 2 — the mirror. Bytes between two nodes left one of them, whatever
# transport carried them.
run_scope "different nodes but called intra-node fires" fires "
INSERT INTO rank_identity VALUES (0,0,11,0,1,'n0'),(1,1,22,0,1,'n1');
INSERT INTO comm_edges VALUES (0,1,'ucx','datatype-units','intra-node',5,500);"

# CLAUSE 3 —'unknown' with nothing unknown about it. This is the CASE falling
# through, which is how a third value becomes a dumping ground.
run_scope "unknown scope with both ranks placed and a resolved peer fires" fires "
INSERT INTO rank_identity VALUES (0,0,11,0,1,'n0'),(1,0,11,1,1,'n0');
INSERT INTO comm_edges VALUES (0,1,'ucx','datatype-units','unknown',5,500);"

# ...and the deliberate NON-assertion. An unresolved peer is exactly what
# 'unknown' is for, and a capture with no node identity at all is mostly this.
# Firing here would make the assertion useless on the nccl_p2p shape.
run_scope "unknown scope on an unresolved peer is quiet" quiet "
INSERT INTO rank_identity VALUES (0,0,11,0,1,'n0');
INSERT INTO comm_edges VALUES (0,-1,'nvlink','accesses','unknown',5,500);"

[ $fail -eq 0 ] && echo "=== asserts_fire : PASS ===" || echo "=== asserts_fire : FAIL ==="
exit $fail
