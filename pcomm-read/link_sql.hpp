// The derived ("link") tables, as SQL over the raw Parquet export.
//
// These are the tables the UI reads: rank-space, identity-keyed, transport-
// neutral. The raw tables underneath them are per-rank, handle-keyed and
// tick-timed, and the gap between the two is this file.
//
// SQL rather than C++ accumulators, deliberately. `CsvAgg` in main.cpp computes
// comm_edges and mem_owner the other way, and it stays — the suite's goldens are
// written against it and it is the oracle these queries are verified against.
// But a join is what this work *is*, the UI queries through DuckDB anyway, and
// a query someone can read and run by hand is worth more here than a second set
// of accumulators nobody can inspect without a debugger.
//
// The one rule these queries follow, and it is the expensive lesson this project
// already paid for: **a wire id is only unique with its scope.** `qp_num` is
// allocated per HCA, so two ranks routinely draw the same one — keying an owner
// map on `qp_num` alone produced impossible self-edges and a 4-peer set for an
// 8-rank job (see the comment at CsvAgg in main.cpp). Every join below keys on
// the pair, `(lid, qp_num)`, never on the number alone.
#pragma once

namespace pcomm::link {

// Views the derived tables are built from. Created in order; each may reference
// the ones above it.
//
// Both transports are unioned at this layer rather than branched on above it.
// Under IBGDA the `ib_*` tables are empty and `mlx5_*` carries everything —
// Mlx5DeviceSend is declared as "the device-plane counterpart of IbPostSend, and
// deliberately the same shape ... so a reader can union the two". This is that
// reader. A consumer of comm_edges never learns which transport a run used,
// which is the point: the comm matrix is a matrix either way.
struct View {
    const char* name;
    const char* sql;
};

inline constexpr View kViews[] = {
    // (lid, qp_num) -> the rank that owns it. `claimants` is kept rather than
    // collapsed: two ranks claiming one (lid, qp_num) is a fabric-level
    // impossibility, so if it is ever > 1 the number is a finding about the
    // capture and not a tie to be broken quietly. min(rank) keeps the result
    // deterministic meanwhile; the C++ version's map silently kept whichever
    // rank was scanned last.
    {"_owner", R"sql(
CREATE OR REPLACE VIEW _owner AS
SELECT lid, qp_num, min(rank) AS rank, count(DISTINCT rank) AS claimants
FROM (
    SELECT src_lid AS lid, qp_num, rank FROM ib_modify_qp          WHERE src_lid <> 0
    UNION ALL
    SELECT src_lid AS lid, qp_num, rank FROM mlx5_devx_qp_connect  WHERE src_lid <> 0
)
GROUP BY lid, qp_num
)sql"},

    // (rank, qp_num) -> the remote endpoint it was connected to. Only the RTR
    // transition carries it, which is why state = 2 is the filter on the verbs
    // side; the DEVX connect record exists only for that transition, so it needs
    // no equivalent.
    {"_rtr", R"sql(
CREATE OR REPLACE VIEW _rtr AS
SELECT rank, qp_num, max(dest_lid) AS dest_lid, max(dest_qp_num) AS dest_qp_num
FROM (
    SELECT rank, qp_num, dest_lid, dest_qp_num FROM ib_modify_qp
      WHERE state = 2 AND dest_qp_num <> 0
    UNION ALL
    SELECT rank, qp_num, dest_lid, dest_qp_num FROM mlx5_devx_qp_connect
      WHERE dest_qp_num <> 0
)
GROUP BY rank, qp_num
)sql"},

    // --- node identity, so a device ordinal can name a rank -----------------
    //
    // Device ordinals are NODE-LOCAL and collide across nodes exactly as
    // qp_num collides across HCAs: rank 0 and rank 4 both run on `run_device 0`
    // and they are different GPUs. So every device lookup must be scoped by
    // node, and a node has to be identified first.
    //
    // Derived from LIDs, which is a FALLBACK and not the answer: ranks sharing
    // an HCA port share a node. It works wherever IB exists and fails exactly
    // where it is most needed — a pure-NVLink run (nccl_p2p, p2p_kernel) has no
    // LID at all, and those captures resolve to no node and therefore to
    // `dst_rank = -1` rather than to a guess. Recording node identity in the
    // capture is step 1 of the NVLink plan; this view is what makes steps 2-4
    // verifiable against the corpus that already exists.
    {"_rank_lid", R"sql(
CREATE OR REPLACE VIEW _rank_lid AS
SELECT DISTINCT rank, lid FROM (
    SELECT rank, src_lid AS lid FROM ib_modify_qp         WHERE src_lid <> 0
    UNION ALL
    SELECT rank, src_lid AS lid FROM mlx5_devx_qp_connect WHERE src_lid <> 0
)
)sql"},

    // One level of closure rather than `min(lid) per rank`: ranks on one node
    // need not use the identical set of ports, and keying on a rank's own
    // minimum would split a node whose ranks overlap only partially. Grouping
    // by "the lowest LID among every rank I share a port with" survives that.
    {"_node_by_lid", R"sql(
CREATE OR REPLACE VIEW _node_by_lid AS
SELECT a.rank, min(c.lid) AS node
FROM _rank_lid a
JOIN _rank_lid b ON b.lid  = a.lid
JOIN _rank_lid c ON c.rank = b.rank
GROUP BY a.rank
)sql"},

    // The recorded node identity wins; the LID inference is the fallback.
    //
    // In that order because the record is a *fact* and the LID grouping is an
    // inference that needs InfiniBand — which is absent from exactly the
    // captures that most need a node (a pure-NVLink run has no LID at all, so
    // every one of nccl_p2p's 1.18M peer accesses resolved to "unknown peer").
    // Older captures predate the record and still resolve through the fallback,
    // which is why it stays rather than being deleted once the record exists.
    //
    // node_hash = 0 means gethostname failed on that rank, and is treated as
    // absent rather than as a node — otherwise every failed rank across a job
    // would merge onto one fictional machine.
    {"_node", R"sql(
CREATE OR REPLACE VIEW _node AS
SELECT r.rank,
       coalesce(ni.node_hash, l.node) AS node
FROM (SELECT DISTINCT rank FROM _rank_lid
      UNION SELECT DISTINCT rank FROM node_identity) r
LEFT JOIN (SELECT rank, min(node_hash) AS node_hash
           FROM node_identity WHERE node_hash <> 0 GROUP BY rank) ni ON ni.rank = r.rank
LEFT JOIN _node_by_lid l ON l.rank = r.rank
WHERE coalesce(ni.node_hash, l.node) IS NOT NULL
)sql"},

    // (node, device) -> the rank that owns it.
    //
    // Fed from mem_object rather than only from gpu_access, and that is not a
    // detail: on nvshmem_multinode ranks 3 and 7 have ZERO gpu_access rows —
    // they are the node-crossing ranks and move nothing over NVLink — so a map
    // built from run_device alone cannot name their GPUs, and rank 2's stores
    // to device 3 would resolve to nothing. mem_object covers all eight ranks.
    //
    // `claimants` is kept for the same reason _owner keeps it: two ranks
    // claiming one (node, device) is a finding, not a tie to break quietly.
    {"_device_owner", R"sql(
CREATE OR REPLACE VIEW _device_owner AS
SELECT n.node, d.device, min(d.rank) AS rank, count(DISTINCT d.rank) AS claimants
FROM (
    SELECT DISTINCT rank, device     AS device FROM mem_object WHERE device <> 65535
    UNION
    SELECT DISTINCT rank, run_device AS device FROM gpu_access
) d
JOIN _node n ON n.rank = d.rank
GROUP BY n.node, d.device
)sql"},

    // NVLink traffic as rank-space edges.
    //
    // Direction follows the OP, not the row: a Store pushes data to the peer,
    // a Load pulls it from the peer, so the two produce opposite edges. Only
    // Store appears in the corpus today, which is precisely why the Load case
    // is written now rather than discovered by whichever capture first has one.
    // Atomic (2) counts as a write; Prefetch (3) as a read.
    //
    // The unit is an ACCESS, not a message — see the comm_edges comment.
    {"_nvlink_sends", R"sql(
CREATE OR REPLACE VIEW _nvlink_sends AS
SELECT
    CASE WHEN g.op IN (1, 2) THEN g.rank ELSE coalesce(o.rank, -1) END AS src_rank,
    CASE WHEN g.op IN (1, 2) THEN coalesce(o.rank, -1) ELSE g.rank END AS dst_rank,
    count(*)::BIGINT      AS sends,
    sum(g.size)::BIGINT   AS bytes
FROM gpu_access g
LEFT JOIN _node n         ON n.rank = g.rank
LEFT JOIN _device_owner o ON o.node = n.node AND o.device = g.mem_device
WHERE g.run_device <> g.mem_device
GROUP BY 1, 2
)sql"},

    // Traffic per local endpoint, both planes. `transport` survives into the
    // endpoint table so a run can still be *asked* which plane moved the bytes,
    // even though comm_edges no longer distinguishes them.
    {"_sends", R"sql(
CREATE OR REPLACE VIEW _sends AS
SELECT rank, qp_num, 'verbs' AS transport,
       count(*) AS sends, sum(length)::BIGINT AS bytes
FROM ib_post_send GROUP BY rank, qp_num
UNION ALL
SELECT rank, qp_num, 'devx' AS transport,
       count(*) AS sends, sum(length)::BIGINT AS bytes
FROM mlx5_device_send GROUP BY rank, qp_num
)sql"},

    // (rank, qp_num) -> the rank on the other end. Extracted from `endpoint`,
    // which computed it inline, because object_access needs the same mapping and
    // a kTable cannot reference another kTable — they are COPYed to Parquet, not
    // registered. Both callers now share one definition of "who is at the other
    // end of this queue pair", which is the join most likely to be got subtly
    // wrong: the key is the PAIR (lid, qp_num), since qp_num is allocated per
    // HCA and repeats across nodes.
    {"_peer_of_qp", R"sql(
CREATE OR REPLACE VIEW _peer_of_qp AS
SELECT r.rank, r.qp_num, o_peer.rank AS peer_rank
FROM _rtr r
LEFT JOIN _owner o_peer
       ON o_peer.lid = r.dest_lid AND o_peer.qp_num = r.dest_qp_num
)sql"},
};

// Derived tables, materialised and written as Parquet in this order.
struct Table {
    const char* name;
    const char* sql;
};

inline constexpr Table kTables[] = {
    // Every recorded touch of an application object, from whichever provider
    // saw it, normalised to an OFFSET inside that object and tagged with where
    // it came from.
    //
    // WHY NORMALISE. The two providers report addresses in different address
    // spaces, so the raw values cannot share an axis:
    //
    //   ib_post_send.remote_addr is in the PEER's address space. RDMA names the
    //     peer's real virtual address, so subtracting the peer's object base is
    //     exact. That is why the join is against the peer's mem_object and not
    //     the sender's — it is the one place a cross-rank object join is
    //     required rather than convenient, and it works because every rank is
    //     captured and bases are interned in one domain.
    //
    //   gpu_access.address is in the LOCAL process's mapping of the peer's
    //     buffer. Measured on nvshmem_p2p_fine-du04: object 32 has base
    //     26447710208 and size 4096, while its accesses run from base+137438953472
    //     to base+137438957564 — 128 GiB out, spanning exactly 4092. The extent
    //     is right and the anchor is not, and the mapping delta is not in the
    //     record, so the offset is taken from the first byte observed.
    //
    // Hence `exact`. A verbs offset is anchored to the object's true base; a p2p
    // offset is anchored to the lowest address seen and is only the true offset
    // when the workload touched the start of the buffer. Reporting these as one
    // number without saying which would be the kind of quiet averaging that
    // makes a plot untrustworthy — so the column is here, not in a panel.
    //
    // `source` is deliberately not collapsed either. A capture with both
    // providers loaded has two independent views of the same object, and being
    // able to see them apart is how you tell a provider that is missing traffic
    // from a workload that did not send any.
    {"object_access", R"sql(
WITH p2p AS (
    SELECT g.rank                                   AS owner_rank,
           g.object                                 AS object,
           g.address - min(g.address) OVER (PARTITION BY g.rank, g.object)
                                                    AS obj_offset,
           g.size                                   AS size,
           'p2p'                                    AS source,
           g.run_device::BIGINT                     AS actor,
           'device'                                 AS actor_kind,
           FALSE                                    AS exact
    FROM gpu_access g
),
verbs AS (
    SELECT e.peer_rank                              AS owner_rank,
           m.id                                     AS object,
           s.remote_addr - m.range_base             AS obj_offset,
           s.length                                 AS size,
           'verbs'                                  AS source,
           s.rank::BIGINT                           AS actor,
           'rank'                                   AS actor_kind,
           TRUE                                     AS exact
    FROM ib_post_send s
    JOIN _peer_of_qp e
      ON e.rank = s.rank AND e.qp_num = s.qp_num
    -- INNERMOST containing object, via LATERAL so the pick is made per send.
    -- Ranges nest: an nvshmem_malloc'd buffer sits inside the symmetric-heap
    -- region mapped once for all of them, so "any containing object" binds every
    -- send to the 512 MB heap instead of the 256 KB buffer. Same rule as
    -- ObjectRegistry::find in the engine; it has to be stated in both places
    -- because they answer the same question against different data.
    --
    -- LATERAL and not QUALIFY: there is no unique send id here (wr_id repeats),
    -- so a window partition collapses distinct sends — measured, 393228 rows
    -- became 131078.
    JOIN LATERAL (
        SELECT o.id, o.range_base
        FROM (SELECT DISTINCT rank, id, range_base, range_size FROM mem_object) o
        WHERE o.rank = e.peer_rank
          AND s.remote_addr >= o.range_base
          AND s.remote_addr <  o.range_base + o.range_size
        ORDER BY o.range_size
        LIMIT 1
    ) m ON TRUE
    -- remote_addr = 0 is a control send with no target buffer (41 of them on
    -- one rank of the case 07 capture). Left in, they drag the window to zero
    -- and every real access collapses into the last bin.
    WHERE s.remote_addr > 0
      AND e.peer_rank IS NOT NULL
)
SELECT * FROM p2p
UNION ALL
SELECT * FROM verbs
)sql"},


    // One row per local endpoint that exists, whether or not it moved bytes.
    // peer_rank is NULL rather than -1 here: an endpoint that was never
    // connected has no peer, which is a different statement from "we could not
    // resolve its peer", and NULL is the only value that does not assert one of
    // them. comm_edges below uses -1 because it must aggregate.
    {"endpoint", R"sql(
SELECT r.rank,
       r.qp_num,
       o_self.lid              AS lid,
       r.dest_lid              AS peer_lid,
       r.dest_qp_num           AS peer_qp_num,
       p.peer_rank             AS peer_rank,
       coalesce(s.transport, 'none')      AS transport,
       coalesce(sum(s.sends), 0)::BIGINT  AS sends,
       coalesce(sum(s.bytes), 0)::BIGINT  AS bytes
FROM _rtr r
LEFT JOIN _owner o_self ON o_self.rank = r.rank AND o_self.qp_num = r.qp_num
LEFT JOIN _peer_of_qp p ON p.rank = r.rank AND p.qp_num = r.qp_num
LEFT JOIN _sends s      ON s.rank = r.rank AND s.qp_num = r.qp_num
GROUP BY ALL
ORDER BY r.rank, r.qp_num
)sql"},

    // The communication matrix, in rank space, across every transport.
    //
    // dst_rank = -1 means the peer could not be resolved — kept as a row rather
    // than dropped, because it is the signal that a map has a hole, and a matrix
    // that silently omits unattributed traffic lies about its totals.
    //
    // **`transport` is a dimension and rows must never be merged across it.**
    // The measurements are not the same quantity:
    //
    //   verbs/devx — one row per MESSAGE the application asked to send; exact.
    //   nvlink     — one row per instrumented load/store INSTRUCTION, and only
    //                from kernels NVBit was told to instrument. Measured on the
    //                corpus: every peer access is a 4-byte store, so a whole
    //                NVSHMEM ring is 4.7 MB against 1.3 GB for one IB edge.
    //                Case 04 filters kernels to bound the capture and case 05
    //                records that polling makes the count non-repeatable, so
    //                this axis is a LOWER BOUND from a sampling-shaped filter.
    //
    // Adding those into one number produces a value that means nothing and
    // would rank a whole NVLink ring below a single IB message. `unit` is
    // carried so no consumer has to infer which it is holding.
    //
    // The oracle relationship survives by subsetting: the rows where
    // transport IN ('verbs','devx'), projected to (src,dst,sends,bytes), are
    // still byte-identical to CsvAgg's comm_edges — which is the only check
    // the derived layer has, and is kept executable rather than described.
    {"comm_edges", R"sql(
SELECT src_rank, dst_rank, transport, unit, sends, bytes FROM (
    SELECT s.rank               AS src_rank,
           coalesce(o.rank, -1) AS dst_rank,
           s.transport          AS transport,
           'messages'           AS unit,
           sum(s.sends)::BIGINT AS sends,
           sum(s.bytes)::BIGINT AS bytes
    FROM _sends s
    LEFT JOIN _rtr   r ON r.rank = s.rank AND r.qp_num = s.qp_num
    LEFT JOIN _owner o ON o.lid  = r.dest_lid AND o.qp_num = r.dest_qp_num
    GROUP BY 1, 2, 3
  UNION ALL
    SELECT src_rank, dst_rank, 'nvlink', 'accesses', sends, bytes
    FROM _nvlink_sends
)
ORDER BY transport, src_rank, dst_rank
)sql"},

    // What the graph could not account for. Empty is the expected state; every
    // row is a defect in the link, not in the workload. This table exists so
    // the UI can *show* a hole rather than render a confident matrix over one —
    // "a wrong matrix that looks confident is worse than no matrix".
    {"link_health", R"sql(
SELECT 'colliding_endpoints' AS finding,
       count(*)              AS n,
       'two ranks claim one (lid, qp_num) — impossible on a sane fabric' AS note
FROM _owner WHERE claimants > 1
UNION ALL
SELECT 'unresolved_sends',
       coalesce(sum(sends), 0)::BIGINT,
       'sends whose peer rank could not be resolved from any RTR transition'
FROM (
    SELECT s.sends
    FROM _sends s
    LEFT JOIN _rtr   r ON r.rank = s.rank AND r.qp_num = s.qp_num
    LEFT JOIN _owner o ON o.lid  = r.dest_lid AND o.qp_num = r.dest_qp_num
    WHERE o.rank IS NULL
)
UNION ALL
-- NVLink counted separately, because it fails for a different reason and needed
-- its own row: without it a capture with NO node identity at all reported a
-- clean bill of health while 100% of its GPU traffic was unattributed. Found on
-- nccl_p2p, which has no IB and therefore no LID to derive a node from.
SELECT 'unresolved_nvlink',
       coalesce(sum(sends), 0)::BIGINT,
       'NVLink accesses whose peer rank is unknown — this capture records no node identity'
FROM _nvlink_sends WHERE src_rank = -1 OR dst_rank = -1
UNION ALL
SELECT 'self_edges',
       coalesce(count(*), 0),
       'a rank sending to itself — the qp_num-collision signature'
FROM (
    SELECT s.rank
    FROM _sends s
    LEFT JOIN _rtr   r ON r.rank = s.rank AND r.qp_num = s.qp_num
    LEFT JOIN _owner o ON o.lid  = r.dest_lid AND o.qp_num = r.dest_qp_num
    WHERE o.rank = s.rank
)
)sql"},
};

// Define the whole derived layer as views on an open connection.
//
// `--link` materialises kTables to Parquet instead, because its output is a
// fixture other tools read. A UI holds the capture in memory and only ever
// queries, so a view is the same answer without the copy — and, more to the
// point, without a temp directory the server would have to own and clean up.
inline std::size_t define(duckdb_connection con) {
    std::size_t failed = 0;
    for (const auto& v : kViews)
        if (!duck::exec(con, v.sql, v.name))
            failed++;
    for (const auto& t : kTables)
        if (!duck::exec(con,
                        "CREATE OR REPLACE VIEW \"" + std::string(t.name) + "\" AS " + t.sql,
                        t.name))
            failed++;
    return failed;
}

}  // namespace pcomm::link
