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
    // Per (rank, domain), the line that carries a raw tick onto the wall clock.
    //
    // This view exists because **TSC is not comparable across nodes**, and the
    // timeline is the first consumer that puts every rank on one axis. Measured
    // on scale_hubspoke_ib-44542856: the lowest tick any rank reports is
    // 101546585120773 while a rank-0 anchor sits at 1677281742642415 — a 16x
    // gap that is not 8 hours of runtime, it is two hosts that booted at
    // different times. Plotting raw ticks on a shared axis would scatter a
    // 7-second job across a fake day.
    //
    // Two anchors per rank is what the capture writes, and two points is
    // exactly what a line needs, so this is a fit and not an approximation.
    // ns_per_tick comes out at ~0.49998728 (a 2.0 GHz TSC) and differs in the
    // 8th significant digit BETWEEN RANKS — which is why the fit is per-rank
    // and a shared constant would drift ranks apart over a long run.
    //
    // The ELSE 1.0 branch is for a capture with one anchor (or two at the same
    // tick): it keeps the arithmetic finite rather than producing inf, and such
    // a rank is counted by link_health's timeline_unanchored companion instead
    // of being quietly plotted at a lie.
    //
    // **t0 and w0 stay INTEGERS.** Only the slope is a DOUBLE, and it is only
    // ever applied to a delta. See the arithmetic note on timeline_span: a
    // wall-clock ns held in a float64 has a 256 ns ulp, which silently rounds
    // every sub-microsecond span in this capture to zero width.
    {"_clock", R"sql(
CREATE OR REPLACE VIEW _clock AS
SELECT rank,
       domain,
       min(local_ticks) AS t0,
       min(wall_ns)     AS w0,
       CASE WHEN max(local_ticks) > min(local_ticks)
            THEN (max(wall_ns)::DOUBLE - min(wall_ns)::DOUBLE)
               / (max(local_ticks)::DOUBLE - min(local_ticks)::DOUBLE)
            ELSE 1.0 END AS ns_per_tick,
       count(*)         AS anchors
FROM clock_anchor
GROUP BY rank, domain
)sql"},

    // A work request's time IN FLIGHT: posted at `a`, retired at `b`.
    //
    // This is the join that turns two lanes of unrelated marks into one
    // operation with a latency. A completion names the work request it retires
    // by (qp_num, wr_id) — the same pair ib_post_send stamped — so that is the
    // key, and `gen` is what makes it exact rather than approximate: wr_id is
    // caller-chosen and routinely recycled as a buffer index, so numbering both
    // sides within (rank, qp_num, wr_id) by time and joining on the number
    // matches the Nth post to the Nth completion. Completions on a queue pair
    // retire in the order they were posted, which is what makes that correct
    // and not merely plausible. Verified against the pftrace exporter's
    // independent C++ implementation of the same rule on
    // scale_hubspoke_ib_kt-44552862: both pair 5,830,890 of 5,830,890 sends.
    //
    // RECEIVE COMPLETIONS ARE EXCLUDED, and the bit test is the whole reason
    // this is not a two-line join. A receive completion carries the RECEIVER's
    // wr_id, which names an ib_post_recv and never a send; pairing without the
    // IBV_WC_RECV check links sends to whatever unrelated receive happened to
    // reuse the number. (The opcode of a FAILED completion is undefined per the
    // verbs spec, so a status<>0 row can in principle land on either side of
    // the test. They are kept — a failed send still retires its work request,
    // and dropping them would leave the send unpaired and the lane a lie.)
    //
    // **MATERIALISED, not a view, and that is a measured decision.** The two
    // window functions over 5.8 M rows cost 257 ms on that capture against a
    // 28 ms baseline. _tl_raw feeds the Timeline, which re-queries on every pan
    // and zoom behind a 90 ms debounce, so leaving this a view would put a
    // quarter-second of window functions on the critical path of a drag. Paid
    // once at capture load instead.
    // Queue pairs whose completions can be attributed to a specific send.
    //
    // FIFO matching is exact rather than approximate because a reliable-
    // connected QP returns completions IN POSTING ORDER — a transport
    // guarantee, which is why a constant wr_id costs nothing. It holds only
    // while EVERY work request is signalled: an unsignalled WR produces no CQE
    // and is retired silently by the next completion, so Nth-to-Nth matching
    // then attributes a completion to a send it did not retire.
    //
    // Equal counts are the only evidence a capture offers, because ib_post_send
    // records no send_flags and ib_queue_pair no sq_sig_all. Measured:
    // scale_hubspoke_ib_kt-44552862 matches on all 862 queue pairs;
    // kt-combined-44546852 posts 3,570 and completes 1,800 on every one of its
    // 32, and had been producing confident and wrong latencies.
    // `sq_sig_all` is the queue pair's own declaration that it signals every
    // work request — see ib_qp_signalling. It does not change the decision
    // below (counts still rule), it explains it: a mismatch on a queue pair
    // that declared sq_sig_all = 0 is certainly selective signalling, while a
    // mismatch on one that declared 1 can only be a run that stopped with work
    // in flight. LEFT JOIN because captures written before that record exists
    // have none, and unknown must read as unknown rather than as 0.
    {"_wr_pairable", R"sql(
CREATE OR REPLACE VIEW _wr_pairable AS
SELECT s.rank, s.qp_num, s.n AS sends, c.n AS comps, g.sq_sig_all,
       f.n AS signalled
FROM      (SELECT rank, qp_num, count(*) AS n FROM ib_post_send GROUP BY 1, 2) s
FULL JOIN (SELECT rank, qp_num, count(*) AS n FROM ib_poll_cq
           WHERE (opcode & 128) = 0 GROUP BY 1, 2) c
       USING (rank, qp_num)
LEFT JOIN (SELECT rank, qp_num, max(sq_sig_all) AS sq_sig_all
           FROM ib_qp_signalling GROUP BY 1, 2) g
       USING (rank, qp_num)
-- How many work requests this queue pair asked to be completed. NULL only when
-- the queue pair posted nothing; a queue pair with sq_sig_all = 1 typically
-- reports 0 here and is pairable by the count branch anyway, since the QP
-- signals everything regardless of what its work requests asked for.
LEFT JOIN (SELECT rank, qp_num, count(*) AS n FROM ib_post_send
           WHERE (send_flags & 2) <> 0 GROUP BY 1, 2) f
       USING (rank, qp_num)
)sql"},

    // The SIGNALLED sends of a selectively signalling queue pair, numbered.
    //
    // This is the row set that pairs against completions when not every work
    // request completes. An unsignalled WR produces no CQE and is retired by
    // the next signalled one, so the completions line up one-for-one with the
    // SIGNALLED sends — in posting order, by the same transport guarantee that
    // makes the ordinary case work. Numbering them here gives the flight lane
    // the same shape it has everywhere else.
    //
    // NO JOIN: send_flags is a column of ib_post_send, so this is a filter.
    // It was briefly a companion record joined on (rank, qp_num, wr_id, ticks),
    // where the ticks were load-bearing — wr_id is caller-chosen and routinely
    // constant, so the natural key alone is a cartesian product. Putting the
    // field on the row it describes removes the join and that hazard with it.
    {"_wr_signalled", R"sql(
CREATE OR REPLACE VIEW _wr_signalled AS
SELECT rank, qp_num, wr_id, length, opcode,
       temporal_a_domain AS domain, temporal_a_ticks AS a,
       row_number() OVER (PARTITION BY rank, qp_num
                          ORDER BY temporal_a_ticks) AS gen
FROM ib_post_send
WHERE (send_flags & 2) <> 0
)sql"},

    {"_wr_flight", R"sql(
CREATE OR REPLACE TABLE _wr_flight AS
WITH s AS (
    SELECT rank, qp_num, wr_id, length, opcode,
           temporal_a_domain AS domain, temporal_a_ticks AS a,
           row_number() OVER (PARTITION BY rank, qp_num, wr_id
                              ORDER BY temporal_a_ticks) AS gen
    FROM ib_post_send
), c AS (
    SELECT rank, qp_num, wr_id, status, opcode AS wc_opcode,
           temporal_a_ticks AS a,
           row_number() OVER (PARTITION BY rank, qp_num, wr_id
                              ORDER BY temporal_a_ticks) AS gen
    FROM ib_poll_cq
    WHERE (opcode & 128) = 0
)
-- wc_opcode is carried rather than discarded because it is what makes the
-- exclusion above CHECKABLE. tests/flow_pairing_agree.sh asserts no paired row
-- has the IBV_WC_RECV bit set: dropping the WHERE leaves the pair COUNT
-- unchanged on any capture where a wr_id has one send and one completion, so a
-- count comparison alone cannot see a send matched to the wrong completion.
-- It also answers "which completions failed" without a second join.
SELECT s.rank, s.qp_num, s.wr_id, s.length, s.opcode, s.domain,
       s.a AS a, c.a AS b, c.status, c.wc_opcode
FROM s JOIN c
  ON  c.rank   = s.rank
  AND c.qp_num = s.qp_num
  AND c.wr_id  = s.wr_id
  AND c.gen    = s.gen
-- Only queue pairs that complete everything they post — see _wr_pairable. A
-- queue pair using unsignalled work requests contributes NO rows on this
-- branch; it is picked up by the UNION below when the capture recorded which
-- of its work requests were signalled, and by nothing at all when it did not.
JOIN _wr_pairable p
  ON  p.rank = s.rank AND p.qp_num = s.qp_num AND p.sends = p.comps
-- A completion cannot precede the send it retires. One that appears to means
-- the capture began mid-flight and the first completion has no post in this
-- trace; drawing it would give the lane a negative-width span. Dropped here and
-- COUNTED in link_health.wr_flight_backwards, on the same trade the clock join
-- makes: a hole you can see beats a chart that looks complete.
WHERE c.a >= s.a

UNION ALL

-- The selectively signalling branch. Same rule, different left-hand side: the
-- completions of such a queue pair line up with its SIGNALLED sends, not with
-- all of them, so _wr_signalled supplies the rows and the generation.
--
-- The latency this produces belongs to the signalled work request, and it
-- BOUNDS the unsignalled ones posted before it rather than describing them —
-- they were retired by this same completion and the capture cannot say when
-- each individually landed. That is a real limit of the transport, not of the
-- instrument: the NIC never reported them.
SELECT s.rank, s.qp_num, s.wr_id, s.length, s.opcode, s.domain,
       s.a AS a, c.a AS b, c.status, c.wc_opcode
FROM _wr_signalled s
JOIN (
    SELECT rank, qp_num, status, opcode AS wc_opcode,
           temporal_a_ticks AS a,
           row_number() OVER (PARTITION BY rank, qp_num
                              ORDER BY temporal_a_ticks) AS gen
    FROM ib_poll_cq
    WHERE (opcode & 128) = 0
) c
  ON  c.rank   = s.rank
  AND c.qp_num = s.qp_num
  AND c.gen    = s.gen
-- Same guards as the branch above: only where the signalled count matches the
-- completion count, and never a completion that precedes its own send.
-- `sends <> comps` is what keeps the two branches DISJOINT, and it is not
-- decoration. A queue pair that completes everything AND marks every work
-- request signalled satisfies both conditions, so without this every one of its
-- sends appears in the flight lane twice — measured on the fixture the moment
-- send_flags moved onto ib_post_send and qp 7 started reporting signalled = 4
-- alongside sends = comps = 4.
JOIN _wr_pairable p
  ON  p.rank = s.rank AND p.qp_num = s.qp_num
  AND p.sends <> p.comps
  AND p.signalled = p.comps
WHERE c.a >= s.a
)sql"},

    // Posts the flight lane could not pair, per queue pair, WITH THE QUEUE
    // PAIR'S OWN ACCOUNT OF WHY.
    //
    // This exists because link_health's wr_flight_unretired was a single global
    // number, and on job 45330280 (NCCL over ibverbs, 16 ranks) it read 69,470
    // — 65.6% of every post in the capture — while the other fifteen rows of the
    // rubric were zero. Nothing was wrong. NCCL selectively signals, its queue
    // pairs SAID SO at creation via sq_sig_all = 0, the rubric's own
    // wr_flight_selective_signalling_qps row recorded that they said so, and
    // then the unretired count reported the consequence as a defect anyway.
    //
    // A rubric whose contract is "n > 0 is a finding" must not count what the
    // capture can already explain. So the split is by evidence, not by guess:
    // sq_sig_all = 0 is a declaration the queue pair made, not an inference from
    // the counts. NULL is left on the defect side — a capture written before
    // ib_qp_signalling existed has no declaration and must not be quietly
    // absolved by its own age.
    //
    // sum(unretired) over BOTH severities equals the old global number, which is
    // the property tests/wr_flow_rule.sh asserts: the split reclassifies, it
    // does not lose rows.
    {"_wr_unretired", R"sql(
CREATE OR REPLACE VIEW _wr_unretired AS
SELECT p.rank, p.qp_num, p.sq_sig_all,
       coalesce(p.sends, 0) - coalesce(f.n, 0) AS unretired
FROM _wr_pairable p
LEFT JOIN (SELECT rank, qp_num, count(*) AS n FROM _wr_flight GROUP BY 1, 2) f
       USING (rank, qp_num)
)sql"},

    // Every event stream in one shape, tick-timed. Split from timeline_span so
    // the union is written once and the clock join once; link_health needs the
    // same rows to count what the join drops.
    //
    // Interval streams carry a and b; instant streams leave b at 0, which is
    // why t1 is greatest(b, a) downstream rather than b. Streams absent from a
    // capture contribute nothing — duck_mount mounts them as empty tables with
    // the right columns, so this union needs no per-provider branch, which is
    // the whole reason that policy exists.
    {"_tl_raw", R"sql(
CREATE OR REPLACE VIEW _tl_raw AS
    SELECT rank, 'ib_post_send'     AS stream, temporal_a_domain AS domain,
           temporal_kind AS kind, temporal_a_ticks AS a, temporal_b_ticks AS b,
           qp_num AS ep, length::BIGINT  AS bytes FROM ib_post_send
UNION ALL
    SELECT rank, 'ib_poll_cq',     temporal_a_domain, temporal_kind,
           temporal_a_ticks, temporal_b_ticks,
           qp_num, byte_len::BIGINT   FROM ib_poll_cq
UNION ALL
    -- The one lane that is a JOIN rather than a stream: a work request from its
    -- post to its completion. Every other lane says an event happened; this one
    -- says how long it took, which is the question a timeline gets opened to
    -- answer. It needs no special case downstream because it arrives in exactly
    -- the same shape — an interval on a rank in a clock domain — and _wr_flight
    -- is materialised so this union stays as cheap as the others.
    -- 2 = TimeKind::Interval, hardcoded because this lane is SYNTHESISED: it
    -- is a join of two measured instants, not a stream with a temporal of its
    -- own, and [post, completion] is a real measured duration.
    SELECT rank, 'ib_wr_flight',   domain, 2::UTINYINT, a, b,
           qp_num, length::BIGINT     FROM _wr_flight
UNION ALL
    SELECT rank, 'ib_post_recv',   temporal_a_domain, temporal_kind,
           temporal_a_ticks, temporal_b_ticks,
           qp_num, length::BIGINT     FROM ib_post_recv
UNION ALL
    SELECT rank, 'ib_recv_cqe',    temporal_a_domain, temporal_kind,
           temporal_a_ticks, temporal_b_ticks,
           qp_num, byte_len::BIGINT   FROM ib_recv_cqe
UNION ALL
    SELECT rank, 'mlx5_device_send', temporal_a_domain, temporal_kind,
           temporal_a_ticks, temporal_b_ticks,
           qp_num, length::BIGINT     FROM mlx5_device_send
UNION ALL
    SELECT rank, 'mlx5_doorbell',  temporal_a_domain, temporal_kind,
           temporal_a_ticks, temporal_b_ticks,
           qp_num, NULL::BIGINT       FROM mlx5_doorbell
UNION ALL
    -- kind = 3 (Window) since 2026-08-17: an access is BOUNDED, not measured,
    -- and drawing its window as a slice would assert the access took that long.
    -- This lane is the reason `kind` exists here at all.
    --
    -- NARROWED TO THE ISSUING KERNEL'S SPAN when one is recorded, and the
    -- difference is three orders of magnitude, not a refinement. What the
    -- producer can bound at drain time is [launch enqueue, host drain]: sound,
    -- but "drain" is in practice the end-of-run flush, measured at 1.94 s wide
    -- around a 48 us kernel on hubspoke-44713195. The kernel's own %globaltimer
    -- entry/exit span is the real bound, and the producer cannot apply it —
    -- see GpuAccess::launch in core/records.hpp for the ordering reason. Here it
    -- can: the whole capture is present, so the join is just a join.
    --
    -- COALESCE, not a filter: a row with no matching span keeps its TSC window
    -- and stays in the lane. Kernel timing can be off (PCOMM_KERNEL_TIMING=0),
    -- the launch path can be one we do not intercept (CUDA graphs), or the span
    -- can have failed to close — and an access is still an access. Degrading per
    -- row is the same rule the corpus follows per stream.
    --
    -- The DOMAIN comes from whichever side won, which is why this is sound
    -- across two clocks without any conversion here: both ends of a narrowed row
    -- are globaltimer in the kernel's domain, both ends of a fallback row are
    -- host TSC in domain 0, and timeline_span joins _clock per (rank, domain).
    -- Mixing the two ends of ONE row across domains would be the bug; this never
    -- does that.
    SELECT g.rank, 'gpu_access',
           coalesce(k.temporal_a_domain, g.temporal_a_domain),
           g.temporal_kind,
           coalesce(k.temporal_a_ticks, g.temporal_a_ticks),
           coalesce(k.temporal_b_ticks, g.temporal_b_ticks),
           g.run_device, g.size::BIGINT
    FROM gpu_access g
    LEFT JOIN (
        -- One row per (rank, launch). gpu_kernel is already unique on that pair,
        -- but source = 2 (HostSubmit) rows must not be the match: they are the
        -- launch API call, 9-13 ms ahead of execution, and narrowing to one
        -- would move the window off the kernel entirely — the same reason they
        -- are excluded from the gpu_kernel lane below.
        SELECT rank, correlation, temporal_a_domain, temporal_a_ticks, temporal_b_ticks
        FROM gpu_kernel WHERE source <> 2 AND correlation <> 0
    ) k ON k.rank = g.rank AND k.correlation = g.launch
UNION ALL
    -- Compute, beside the comm. This is the row that lets a timeline answer
    -- "was that rank stalled or working" instead of only "that rank went
    -- quiet", and it is the reason gpu_kernel exists at all.
    --
    -- It arrives on its OWN clock domain (1 for CUPTI, 2+gpu for a device
    -- globaltimer) and needs no special case here: _clock fits per
    -- (rank, domain), so the join below places it exactly as it places TSC.
    --
    -- source = 2 (HostSubmit) is EXCLUDED. Those rows are the launch API call,
    -- not the execution, and they trail the real thing by 9-13 ms against
    -- kernels lasting tens of microseconds — drawn on a timeline they would be
    -- confident fiction. They stay in the raw table for anyone studying launch
    -- latency; they must not reach a lane labelled "kernel".
    SELECT rank, 'gpu_kernel',     temporal_a_domain, temporal_kind,
           temporal_a_ticks, temporal_b_ticks,
           device, NULL::BIGINT      FROM gpu_kernel WHERE source <> 2
)sql"},

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

    // --- the UCX lane: rank space without a queue pair ----------------------
    //
    // Everything above resolves a peer through (lid, qp_num), which exists only
    // if the traffic went through a verbs door. On the HPC-X stack it did not:
    // the 2026-08-27 door sweep found libucp and libuct_ib import no
    // ibv_post_send at all (PLAN-20260815-1540 Future work 1), and a probe with
    // -p ib,ucx,mlx5dv captured the whole DEVX control plane while
    // mlx5_device_send stayed empty against 24,452 tagged sends. So for every
    // MPI-over-UCX application _sends was empty, and comm_edges had ZERO ROWS
    // over captures holding tens of GB of correctly recorded traffic
    // (tests/hpc/apps/, three studies independently).
    //
    // UCX carries its own rank correspondence and it is the ONLY join in these
    // records that crosses a rank: this rank's endpoint remembers the prefix
    // hash of the address it dialled, and the rank that owns that address wrote
    // the same hash for its own worker. See UcxEpConnection in ucx_records.hpp.
    //
    // prefix_hash -> the rank that owns it, WITH the evidence that it is
    // unambiguous. UcxWorkerAddress records two hashes precisely so a prefix
    // collision is visible rather than a confident wrong join; that comment says
    // a reader can say so, and this is the reader saying so. Two workers sharing
    // a prefix are two workers this join cannot tell apart, whether they are on
    // one rank or several, so both counts have to be 1 for the row to resolve.
    {"_ucx_rank_of_prefix", R"sql(
CREATE OR REPLACE VIEW _ucx_rank_of_prefix AS
SELECT prefix_hash,
       min(rank)                  AS rank,
       count(DISTINCT rank)       AS claimants,
       count(DISTINCT addr_hash)  AS addrs
FROM ucx_worker_address
GROUP BY prefix_hash
)sql"},

    // (rank, endpoint) -> the peer rank, kept only where the prefix resolves to
    // exactly one worker on exactly one rank. An ambiguous prefix produces no
    // row here, so its traffic falls to dst_rank = -1 below and is COUNTED as
    // unresolved rather than attributed to whichever rank sorted first.
    {"_ucx_peer_of_ep", R"sql(
CREATE OR REPLACE VIEW _ucx_peer_of_ep AS
SELECT c.rank, c.endpoint, p.rank AS peer_rank
FROM ucx_ep_connection c
JOIN _ucx_rank_of_prefix p ON p.prefix_hash = c.remote_prefix_hash
WHERE p.claimants = 1 AND p.addrs = 1
)sql"},

    // Outbound UCX operations per (rank, endpoint), in DATATYPE UNITS.
    //
    // NOT BYTES, and the column must never be summed with the verbs lane's.
    // UcxTagSend.count is documented "raw UCP datatype count, not universally
    // bytes" — with a contiguous 8-bit datatype, which is what an MPI stack
    // normally builds, it is bytes; with any other it is not, and nothing in the
    // capture says which. comm_edges carries `unit` so a consumer holding this
    // number knows what it is holding.
    //
    // Three doors, and the direction of each is the reason it is here:
    //   tag_send  two-sided send      -> out
    //   am_send   active message      -> out (payload only; header_length is
    //                                   control traffic and UcxAmSend keeps the
    //                                   two separate deliberately)
    //   rma put   local -> remote     -> out
    //   rma get   remote -> local     -> IN, so it is emitted with the endpoints
    //                                   swapped, not dropped and not counted as
    //                                   outbound. A get moves bytes across the
    //                                   same link in the other direction.
    //
    // status_class = 1 (Error) is excluded everywhere: the call was rejected, so
    // no bytes moved. Immediate (0) and Request (2) both did or will.
    //
    // NUMERIC, not the decoded name. This layer mounts the RAW streams, where an
    // enum is the integer that went on disk — `_nvlink_sends` above says the
    // same thing by writing `g.op IN (1, 2)`. The CSV export decodes to
    // 'immediate'/'put' for a human; nothing here ever sees those strings, and a
    // predicate written against them matches no row and silently empties the
    // lane. Values are pinned in providers/ucx/ucx_records.hpp:
    // StatusClass{Immediate=0, Error=1, Request=2}, RmaOp{Put=0, Get=1}.
    {"_ucx_sends", R"sql(
CREATE OR REPLACE VIEW _ucx_sends AS
SELECT rank, endpoint, sum(n)::BIGINT AS sends, sum(units)::BIGINT AS units, inbound
FROM (
    SELECT rank, endpoint, count(*) AS n, sum("count")::HUGEINT AS units, FALSE AS inbound
    FROM ucx_tag_send WHERE status_class <> 1 GROUP BY rank, endpoint
    UNION ALL
    SELECT rank, endpoint, count(*), sum("count")::HUGEINT, FALSE
    FROM ucx_am_send  WHERE status_class <> 1 GROUP BY rank, endpoint
    UNION ALL
    SELECT rank, endpoint, count(*), sum("count")::HUGEINT, FALSE
    FROM ucx_rma      WHERE status_class <> 1 AND op = 0 GROUP BY rank, endpoint
    UNION ALL
    SELECT rank, endpoint, count(*), sum("count")::HUGEINT, TRUE
    FROM ucx_rma      WHERE status_class <> 1 AND op = 1 GROUP BY rank, endpoint
)
GROUP BY rank, endpoint, inbound
)sql"},

    // --- naming what was interned ------------------------------------------
    //
    // (rank, domain, handle) -> the string, WITH the evidence that it is one
    // string. Same shape and same reason as _ucx_rank_of_prefix above: the join
    // reports its own ambiguity instead of resolving it silently.
    //
    // Ambiguity is possible here and is not hypothetical. A REPLAYED capture
    // nests one rank-* directory per pass (out/ib/rank-0, out/gpumem/rank-0);
    // both load as rank 0 with independent dictionaries, and handles restart at
    // 1 in every domain of every rank. So handle 3 can be "rc_mlx5" in one pass
    // and a kernel name in the other, and a consumer that took min(text) would
    // get a confident wrong name on exactly the captures that are hardest to
    // check by eye.
    //
    // `texts = 1` is therefore the condition every caller must apply, and the
    // callers below do it in the SELECT rather than in the JOIN so the ROW
    // survives with a NULL name — unresolvable traffic stays counted, which is
    // the rule comm_edges' dst_rank = -1 follows for the same reason.
    {"_intern_name", R"sql(
CREATE OR REPLACE VIEW _intern_name AS
SELECT rank, domain, handle,
       min(text)            AS text,
       count(DISTINCT text) AS texts
FROM intern_text
GROUP BY rank, domain, handle
)sql"},

    // --- the UCT layer: which transport actually carried it ------------------
    //
    // (rank, iface) -> the transport and device it was opened on. `domain = 2`
    // is InternDomain::String (rec/intern_table.hpp: Pc, Pointer, String,
    // KernelName, TypeName) — a NUMBER for the same reason `status_class <> 1`
    // is a number: this layer mounts the RAW streams, where an enum is the
    // integer that went on disk.
    //
    // An iface pointer is reusable after close, so (rank, iface) is grouped and
    // not assumed unique, and `tl_names` reports it when one pointer carried two
    // transports across a run.
    {"_uct_iface_named", R"sql(
CREATE OR REPLACE VIEW _uct_iface_named AS
SELECT i.rank,
       i.iface,
       min(CASE WHEN t.texts = 1 THEN t.text END) AS tl_name,
       min(CASE WHEN d.texts = 1 THEN d.text END) AS dev_name,
       count(DISTINCT t.text)                     AS tl_names,
       max(i.patched_ops)                         AS patched_ops
FROM uct_iface i
LEFT JOIN _intern_name t ON t.rank = i.rank AND t.domain = 2 AND t.handle = i.tl_name
LEFT JOIN _intern_name d ON d.rank = i.rank AND d.domain = 2 AND d.handle = i.dev_name
GROUP BY i.rank, i.iface
)sql"},

    // UCT operations per (rank, iface, op), split by WHERE THE BYTES WENT.
    //
    // The three sums are three different quantities and the record declaration
    // is explicit that they must not be added:
    //
    //   out         Am*/Put*/TagEager*  — payload LEFT this node during the call
    //   in          Get*                — payload was REQUESTED; it arrives later
    //   advertised  TagRndvZcopy        — payload was OFFERED for the peer to
    //                                     pull, and only `header` bytes went out
    //
    // Collapsing them into one "bytes" column would count a rendezvous
    // advertisement as a transfer and then count the transfer again when the
    // peer's Get* records it — the same buffer billed twice, in opposite
    // directions. Three columns is what stops a consumer doing that by accident.
    //
    // `status >= 0` is the error filter: ucs_status_t is UCS_OK = 0,
    // UCS_INPROGRESS = 1, and every error is NEGATIVE. UCS_ERR_NO_RESOURCE (-6)
    // in particular is UCX's "retry later" and means the operation did not
    // happen at all — counting it would inflate a busy transport's totals in
    // proportion to how busy it was.
    {"_uct_sends", R"sql(
CREATE OR REPLACE VIEW _uct_sends AS
SELECT rank, iface, op,
       count(*)::BIGINT AS ops,
       sum(CASE WHEN op IN (7, 8, 9, 13) THEN 0 ELSE payload END)::HUGEINT AS out_bytes,
       sum(CASE WHEN op IN (7, 8, 9)     THEN payload ELSE 0 END)::HUGEINT AS in_bytes,
       sum(CASE WHEN op = 13             THEN payload ELSE 0 END)::HUGEINT AS advertised_bytes,
       sum(header)::HUGEINT AS header_bytes
FROM uct_send
WHERE status >= 0
GROUP BY rank, iface, op
)sql"},

    // Every UCT operation with its transport NAMED and CLASSIFIED.
    //
    // A VIEW rather than part of transport_mix, because `pcomm link` COPYs each
    // derived table's SQL straight to Parquet and never creates it as a
    // relation — so link_health cannot say `FROM transport_mix`, and a second
    // copy of this CASE expression is one copy and one bug the day a transport
    // is added to one list and not the other. Views are created first and by
    // name, which is the only place two derived tables can share a definition.
    //
    // `scope` is a claim about the TRANSPORT UCX CHOSE, deliberately narrower
    // than "did it leave the node". self/sysv/posix/cma/knem/xpmem move bytes
    // with no network stack; cuda_ipc/cuda_copy/rocm_* move them over the local
    // bus. `tcp` counts as 'network' even though a same-node pair can use it:
    // it went through the kernel network stack, which is a fact, where "left
    // the node" would be a guess.
    //
    // Anything else — a transport not in either list, or an iface whose name did
    // not resolve — is 'unknown', and is NEVER folded into one of the other two.
    // A transport added to UCX after this list was written must read as unknown
    // rather than be guessed into 'network', because the guess is the one that
    // makes link_health's intra-node caveat silently report zero.
    //
    // `gdr_copy` JOINED THE INTRA-NODE LIST on 2026-08-31, and it was the first
    // name this list has been caught missing on real hardware: the GROMACS
    // round-2 run (job 45235734) put 9,348 operations into 'unknown', which is
    // the list working — it refused to guess — and also a hole, since nothing
    // downstream can use an unknown. It is not a guess to place: gdr_copy is
    // GDRCopy mapping the GPU's BAR1 into the host address space so the CPU can
    // read and write device memory directly. No network stack, no fabric, one
    // machine by construction. `rocm_gdr` is its AMD counterpart and is added by
    // the same symmetry that already puts rocm_ipc and rocm_copy here without
    // this project ever having owned an AMD GPU.
    //
    // What this does NOT license is sweeping the rest of UCX's transport
    // namespace in from a manual. ugni and the other names absent here stay
    // unknown until something in a capture makes them concrete, because a list
    // extended by reading documentation is a list nobody has tested.
    {"_uct_scoped", R"sql(
CREATE OR REPLACE VIEW _uct_scoped AS
SELECT s.rank,
       f.tl_name,
       f.dev_name,
       CASE
         WHEN f.tl_name IN ('self','sysv','posix','mm','cma','knem','xpmem',
                            'cuda_ipc','cuda_copy','gdr_copy',
                            'rocm_ipc','rocm_copy','rocm_gdr')            THEN 'intra-node'
         WHEN f.tl_name IN ('rc_verbs','rc_mlx5','dc_mlx5','ud_verbs','ud_mlx5',
                            'ib','srd','tcp')                             THEN 'network'
         ELSE 'unknown'
       END AS scope,
       s.op, s.ops, s.out_bytes, s.in_bytes, s.advertised_bytes, s.header_bytes
FROM _uct_sends s
LEFT JOIN _uct_iface_named f ON f.rank = s.rank AND f.iface = s.iface
)sql"},

    // --- the two layers, joined on the tag -----------------------------------
    //
    // THE ONLY JOIN BETWEEN THE 600s AND THE 800s, and it exists because of one
    // measurement: `uct_send.tag` carries the APPLICATION's tag. MN5 job
    // 45115011 recorded `ucx_tag_send.tag` and `uct_send.tag` as the same
    // values, 20 sends each, on both ranks — 0x650000000000 and 0x660000000000
    // being Open MPI's encoding of the workload's own MPI tags 101 and 102 with
    // the source rank below. Until that was measured there was no defensible
    // way to say which wire operations a UCP send became, and PLAN-20260815-1540
    // Stage 3 was blocked on exactly this.
    //
    // PER (rank, tag), NOT PER MESSAGE. An application reuses a tag across many
    // sends, so this pairs FLOWS and not individual operations; pairing
    // individual ones would need an ordering assumption between two layers that
    // buffer independently, and inventing one would produce confident wrong
    // pairs. A flow is what the capture can honestly support.
    //
    // THE UCT SIDE IS SELECTED BY DOOR, NOT BY `tag <> 0`. UctSend's declaration
    // says `tag` is 0 on every non-tag door because those doors carry no tag at
    // all — so a `tag <> 0` filter would also throw away a genuine tag-0
    // message, and silently. ops 10-13 are TagEagerShort/Bcopy/Zcopy and
    // TagRndvZcopy: on those, and only those, the field means something.
    {"_ucp_tag_flow", R"sql(
CREATE OR REPLACE VIEW _ucp_tag_flow AS
SELECT rank, tag,
       count(*)::BIGINT              AS ucp_sends,
       sum("count")::HUGEINT         AS ucp_units
FROM ucx_tag_send WHERE status_class <> 1
GROUP BY rank, tag
)sql"},

    {"_uct_tag_flow", R"sql(
CREATE OR REPLACE VIEW _uct_tag_flow AS
SELECT rank, tag,
       count(*)::BIGINT      AS uct_ops,
       sum(payload)::HUGEINT AS uct_payload,
       count(DISTINCT op)    AS doors
FROM uct_send WHERE status >= 0 AND op IN (10, 11, 12, 13)
GROUP BY rank, tag
)sql"},

    // The receives that arrive WITHOUT a tag, and the probes that hold it.
    //
    // `ucp_tag_msg_recv_nb` takes a `ucp_tag_message_h` and no tag at all, so
    // UcxTagRecv writes `tag` and `tag_mask` as 0 on that door and carries the
    // message handle instead. The tag was not lost — the probe that produced the
    // handle was handed it in `ucp_tag_recv_info_t`, which is why ucx_hooks.hpp
    // hooks the two TOGETHER. Until now nothing joined them, so the record's own
    // instruction ("join it to ucx_tag_probe.message to recover the tag AND the
    // true delivered length") described a join that existed nowhere: a
    // msg_recv row had a tag in principle and not in practice.
    //
    // `seq` exists because a receive has no unique key. Two receives on one rank
    // can share every recorded field, so the row identity a QUALIFY needs is
    // manufactured here rather than assumed of `message` or of the ticks.
    {"_ucx_msg_recv", R"sql(
CREATE OR REPLACE VIEW _ucx_msg_recv AS
SELECT rank, worker, message, "count" AS offered, status_class,
       temporal_a_ticks AS a,
       row_number() OVER (PARTITION BY rank ORDER BY temporal_a_ticks) AS seq
FROM ucx_tag_recv
WHERE door = 4 AND message <> 0 AND status_class <> 1
)sql"},

    // THE JOIN IS TEMPORAL, AND THAT IS NOT A REFINEMENT. `ucp_tag_message_h` is
    // a pointer into UCX's request mpool and is RECYCLED — observed directly
    // while the records were being designed, with UCX printing its own
    // mpool-reuse warning in the same run. Matching on the handle value alone
    // pairs a receive with a long-dead probe the moment the pool wraps, and the
    // result looks exactly like a correct pairing. "The most recent probe with
    // this handle at or before this receive" is the rule UcxTagRecv states, and
    // this is where it is implemented.
    //
    // `removed = 1` is NOT required. A probe with remove=0 leaves the message
    // queued and cannot be the one `ucp_tag_msg_recv_nb` consumes, but a caller
    // may probe repeatedly before removing, and taking the LATEST at or before
    // the receive already selects the removing probe when one exists. Filtering
    // on it would instead drop the row entirely whenever the removing probe was
    // not captured, turning a recoverable tag into silence.
    {"_ucx_probe_of_recv", R"sql(
CREATE OR REPLACE VIEW _ucx_probe_of_recv AS
SELECT r.seq, r.rank, r.worker, r.message, r.offered, r.a,
       p.sender_tag, p.length AS delivered, p.temporal_a_ticks AS probe_a
FROM _ucx_msg_recv r
JOIN ucx_tag_probe p
  ON p.rank = r.rank AND p.worker = r.worker AND p.message = r.message
 AND p.temporal_a_ticks <= r.a
QUALIFY row_number() OVER (PARTITION BY r.rank, r.seq
                           ORDER BY p.temporal_a_ticks DESC) = 1
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
    UNION ALL
    -- The port's own LID, which needs no traffic to exist. The two branches
    -- above both require the rank to have TRANSITIONED a queue pair, so a rank
    -- that opened a device and posted nothing had no LID anywhere and fell out
    -- of _node_by_lid entirely — a hole shaped exactly like the rank you are
    -- investigating when you ask why it went quiet. See ib_port.
    --
    -- Non-ACTIVE ports are included deliberately: a LID on a down port still
    -- names the rank, and excluding it would restore the hole in the one case
    -- where the port state is the answer.
    SELECT rank, lid FROM ib_port WHERE lid <> 0
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
    // rank -> the device it drives, and NULL when that question has no single
    // answer. A rank can own several GPUs (the 4-GPU ring is one process), and
    // naming one of them would be a guess that reads exactly like a fact. The
    // ibverbs cases put one GPU on each node precisely so this resolves.
    // FOUR SOURCES, AND IT USED TO READ TWO. `rank_identity.device` came back
    // NULL on the llm.c and Quantum ESPRESSO captures while the answer was
    // sitting in the same file: those runs recorded `nccl_comm.cuda_dev` and
    // `gpu_kernel.device` and this view looked at neither. Ten fields across
    // nine tables carry a device ordinal; the question is which of them state
    // THIS RANK'S OWN GPU rather than where some buffer happened to live.
    //
    // The two added here are the two that say it outright:
    //   nccl_comm.cuda_dev   the device the communicator is bound to, from
    //                        NCCL itself. The strongest statement available.
    //   gpu_kernel.device    the device a kernel of ours executed on. Emitted
    //                        by the ENGINE (libpcomm/src/engine.cpp), so it
    //                        needs no GPU backend at all — this is the reason
    //                        the old pair was wrong to be a pair: gpu_access is
    //                        NVBit-only, so device resolution silently depended
    //                        on an instrument most captures do not run.
    //
    // DELIBERATELY NOT ADDED: ucx_tag_send.device, ucx_rma.device,
    // ucx_am_send.device, mlx5_umem.device, mlx5_devx_qp.device. Every one of
    // those is the residency of a BUFFER, which is usually this rank's GPU and
    // is not the same claim — a host buffer reports 65535, and under peer
    // access a buffer can legitimately live on another rank's device. Folding
    // them in would turn "which GPU does this rank drive" into "which GPUs has
    // this rank touched", and the `devices > 1` guard below would then read as
    // ambiguity where there is none. mem_object is the same class and is kept
    // only because it predates this note and case 07 depends on it.
    //
    // cuda_dev is int32 and negative when NCCL has not bound one yet; the rest
    // are uint16 with 65535 for unknown. Both are filtered, and the cast makes
    // the UNION agree on a type rather than letting DuckDB widen it silently.
    // TWO TIERS, and the weak one is consulted only for a rank the strong one
    // cannot answer. The first version of this fix excluded buffer-residency
    // fields on principle — where a buffer LIVES is not the same claim as which
    // GPU a rank DRIVES — and the principle is right while the exclusion was
    // wrong, which the Quantum ESPRESSO round-2 run settled (job 45237045):
    //
    //   ucx_tag_send.device   rank 0..3 -> dev 0,1,2,3 ; rank 4..7 -> dev 0,1,2,3
    //                         ~14k sends each, one device per rank, no ambiguity
    //   mem_object.device     65535 (unknown) on all 1386 rows
    //   gpu_kernel/nccl_comm  absent — those providers were not in the run
    //
    // So on a `-p ucx,ucx_deep` capture the buffer fields are the ONLY evidence
    // there is, and they are exactly right. Excluding them bought the principle
    // and paid for it with a NULL, which is the worse answer: it says "unknown"
    // where the capture plainly knows.
    //
    // The tiers keep the principle where it bites. Weak evidence can never
    // OVERRIDE or DILUTE strong evidence — a rank with any strong row ignores
    // its weak rows entirely, so a buffer that legitimately lives on a peer's
    // GPU under peer access cannot turn a known device into `devices = 2`. It
    // only fills a hole that would otherwise stay empty.
    //
    // And the `devices = 1` guard still does its job inside each tier: a rank
    // whose sends really did come off two GPUs reports devices = 2 and a NULL
    // device, which is the honest answer rather than a coin flip.
    {"_rank_device", R"sql(
CREATE OR REPLACE VIEW _rank_device AS
WITH strong AS (
    SELECT DISTINCT rank, device     AS device FROM mem_object WHERE device <> 65535
    UNION
    SELECT DISTINCT rank, run_device AS device FROM gpu_access
    UNION
    SELECT DISTINCT rank, device     AS device FROM gpu_kernel WHERE device <> 65535
    UNION
    SELECT DISTINCT rank, cuda_dev::USMALLINT AS device FROM nccl_comm WHERE cuda_dev >= 0
),
weak AS (
    SELECT DISTINCT rank, device FROM ucx_tag_send WHERE device <> 65535
    UNION
    SELECT DISTINCT rank, device FROM ucx_rma      WHERE device <> 65535
    UNION
    SELECT DISTINCT rank, device FROM ucx_am_send  WHERE device <> 65535
),
evidence AS (
    SELECT rank, device FROM strong
    UNION ALL
    SELECT w.rank, w.device FROM weak w
    WHERE NOT EXISTS (SELECT 1 FROM strong s WHERE s.rank = w.rank)
)
SELECT rank,
       CASE WHEN count(DISTINCT device) = 1 THEN min(device) END AS device,
       count(DISTINCT device)                                    AS devices
FROM evidence
GROUP BY rank
)sql"},

    // The same four sources, for the same reason. These two views must agree on
    // what counts as evidence of a rank's device, or a rank can resolve here and
    // not there — which is how a peer lookup returns a rank the identity table
    // says owns nothing.
    // (rank, ordinal) -> a device key that survives CUDA_VISIBLE_DEVICES.
    //
    // An ordinal is what every record carries and it is only meaningful inside
    // the process that emitted it: CUDA_VISIBLE_DEVICES renumbers per rank, so
    // rank 4's "device 0" and rank 5's "device 0" can be different cards on one
    // node. gpu_device (528) is the translation, and this view is where it is
    // applied — once, so every consumer keys the same way.
    //
    // PREFIXED, because the two key spaces must never meet. 'P' keys are PCI
    // addresses and 'O' keys are bare ordinals from a capture with no
    // gpu_device rows; without the prefix, pci_bus 3 and ordinal 3 would join to
    // each other and produce an owner that is confidently wrong. A string key
    // costs a hash join and buys a namespace that cannot collide.
    //
    // The 'O' fallback is not a legacy concession — it is what a capture of a
    // non-CUDA rank, or one whose buffers never flushed in-run (see GpuDevice's
    // own comment), legitimately looks like. Those degrade to exactly the
    // behaviour this file had before 528 existed, which was correct whenever
    // nothing was remapped.
    {"_device_key", R"sql(
CREATE OR REPLACE VIEW _device_key AS
SELECT r.rank,
       r.device,
       CASE WHEN g.pci_bus IS NOT NULL THEN 'P' || CAST(g.pci_bus AS VARCHAR)
            ELSE 'O' || CAST(r.device AS VARCHAR)
       END AS dev_key
FROM (
    SELECT DISTINCT rank, device     AS device FROM mem_object WHERE device <> 65535
    UNION SELECT DISTINCT rank, run_device     FROM gpu_access
    UNION SELECT DISTINCT rank, mem_device     FROM gpu_access
    UNION SELECT DISTINCT rank, device         FROM gpu_kernel WHERE device <> 65535
    UNION SELECT DISTINCT rank, cuda_dev::USMALLINT FROM nccl_comm WHERE cuda_dev >= 0
    UNION SELECT DISTINCT rank, ordinal        FROM gpu_device
) r
LEFT JOIN gpu_device g ON g.rank = r.rank AND g.ordinal = r.device
)sql"},

    // Strong evidence ONLY, and this is where the two views deliberately differ.
    // _rank_device answers "which GPU does this rank drive" and may fall back to
    // buffer residency to answer it at all. This one answers "who owns (node,
    // device)" and is the map a peer lookup resolves THROUGH — so a wrong entry
    // here does not leave a hole, it attributes another rank's NVLink traffic to
    // the wrong owner. Buffer residency is exactly the evidence that can name a
    // GPU a rank does not own (peer access), so it is not admitted here.
    //
    // The asymmetry is the point: a missing owner is recoverable, a confidently
    // wrong one is not.
    // KEYED ON dev_key, not on the ordinal, since 2026-09-01. The survey that
    // motivated GpuDevice measured the failure this fixes: four MN5 nodes of
    // identical hardware report bus ids 1B/2C/9D/AD, so `(node, ordinal)` is
    // only an identity while nothing remaps. Under the ordinary
    // CUDA_VISIBLE_DEVICES=$LOCAL_RANK idiom every rank on a node reports
    // ordinal 0 and this view collapsed them into one owner — min(rank) — while
    // reporting claimants > 1 to nobody who reads it.
    //
    // `device` is kept alongside as the human-readable ordinal of whichever
    // rank won, because rank_identity and the UI still speak ordinals. It is NOT
    // the key and must not be joined on.
    {"_device_owner", R"sql(
CREATE OR REPLACE VIEW _device_owner AS
SELECT n.node,
       k.dev_key,
       min(d.device)          AS device,
       min(d.rank)            AS rank,
       count(DISTINCT d.rank) AS claimants
FROM (
    SELECT DISTINCT rank, device     AS device FROM mem_object WHERE device <> 65535
    UNION
    SELECT DISTINCT rank, run_device AS device FROM gpu_access
    UNION
    SELECT DISTINCT rank, device     AS device FROM gpu_kernel WHERE device <> 65535
    UNION
    SELECT DISTINCT rank, cuda_dev::USMALLINT AS device FROM nccl_comm WHERE cuda_dev >= 0
) d
JOIN _node n ON n.rank = d.rank
JOIN _device_key k ON k.rank = d.rank AND k.device = d.device
GROUP BY n.node, k.dev_key
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
LEFT JOIN _device_key   gk ON gk.rank = g.rank AND gk.device = g.mem_device
LEFT JOIN _device_owner o  ON o.node = n.node AND o.dev_key = gk.dev_key
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

    // One row per successful two-sided RECEIVE completion, carrying the buffer
    // it landed in and how confidently that buffer was named.
    //
    // WHY THE RECEIVER IS THE ONLY WITNESS. A SEND names no remote address —
    // the receiver chooses the buffer by posting it — so the send-side branch
    // of object_access cannot see this traffic and drops it at
    // `remote_addr > 0`. Measured on scale_hubspoke_ib-44517390: 11,520 of the
    // capture's sends are two-sided, 180 per rank, every one of them properly
    // recorded and none of them attributable from the sender's rows. What the
    // receiver has is the post (addr, wr_id) and the completion (wr_id,
    // byte_len), which closes the loop locally; only the SENDER's identity has
    // to come from the QP.
    //
    // OPCODE 128 ONLY, NEVER 129. A RECV_RDMA_WITH_IMM completion also consumes
    // a receive WQE, but its payload went to the address the SENDER named — the
    // posted buffer is spent carrying the immediate value alone. Attributing
    // byte_len to the recv buffer there would put the bytes in the wrong object
    // AND double-count them, since the send-side branch has already placed them
    // correctly.
    //
    // THREE ARMS, TRIED IN ORDER OF HOW MUCH THEY ASSUME.
    //
    //   hardware  (rank, srq, slot) from ib_recv_cqe + ib_post_recv.slot. The
    //             NIC stamped the slot on the completion; wr_id is not
    //             consulted. Absent from captures older than those records and
    //             from every non-mlx5 NIC, which is why the two below stay.
    //   QP        (rank, qp_num, wr_id) — precise, the endpoint is fixed at
    //             post time.
    //   SRQ       (rank, wr_id) alone. An SRQ receive is not bound to a queue
    //             pair until a message lands, so the post carries qp_num = 0
    //             while its completion carries a real one.
    //
    // The last two are the application's key. wr_id is chosen by the caller and
    // routinely recycled as a buffer index:
    //
    //   QP receives  key (rank, qp_num, wr_id) — precise; the endpoint is fixed
    //                at post time.
    //   SRQ receives key (rank, wr_id) only. An SRQ receive is not bound to a
    //                queue pair until a message lands, so the post carries
    //                qp_num = 0 while its completion carries a real one.
    //
    // Same rule, and the same reason for it, as recv_completion_accounted.sql —
    // which learned it from 656 false violations on case 07. Measured on
    // scale_hubspoke_ib-44517390, ALL 11,520 completions resolve through the SRQ
    // arm and none through the QP arm, so the weaker key is not a corner case
    // here: it is the only one that fires.
    //
    // `addrs` is what makes that safe to publish. It is the number of distinct
    // buffers the key named, so 1 means the attribution is certain and anything
    // higher means it is a pick. Rows are kept either way — dropping them would
    // silently shrink totals, the failure the matrix's `?` column exists to
    // prevent — and link_health counts the ones that are not certain.
    //
    // LEFT joins throughout, deliberately: a completion that resolves to no
    // posted buffer, or to no known object, stays in this view with NULLs so it
    // can be COUNTED. An inner join would make the capture look clean by
    // deleting the evidence that it is not.
    {"_recv_attr", R"sql(
CREATE OR REPLACE VIEW _recv_attr AS
SELECT c.rank                AS recv_rank,
       c.qp_num              AS qp_num,
       c.wr_id               AS wr_id,
       c.byte_len            AS byte_len,
       e.peer_rank           AS sender_rank,
       r.addr                AS addr,
       r.addrs               AS addrs,
       -- Did the payload fit the entry we recorded? ib_post_recv keeps sge[0]
       -- only, and a scatter list is filled in order, so bytes beyond that
       -- entry's length landed somewhere this capture cannot name. num_sge = 1
       -- can never spill: its single entry holds the whole posted length.
       (r.num_sge > 1 AND c.byte_len > r.first_len) AS spilled,
       m.id                  AS object,
       m.range_base          AS range_base
FROM ib_poll_cq c
LEFT JOIN LATERAL (
    -- THE HARDWARE'S ANSWER FIRST, when the capture has one.
    --
    -- ib_recv_cqe carries the wqe_counter the NIC stamped on this completion
    -- and ib_srq_recv_slot carries the slot each posted buffer took, so
    -- (rank, srqn, slot) names the buffer exactly and wr_id is not consulted at
    -- all. This arm is absent from captures taken before those records existed
    -- and from any non-mlx5 NIC, which is why the wr_id arms below are kept
    -- rather than replaced: a derived table that only worked on new captures
    -- would make every capture in the corpus unreadable to answer a question
    -- they can already almost answer.
    --
    -- addrs = 1 by construction here. It is not an assumption — a slot holds
    -- one buffer at a time, which is the entire reason this arm exists.
    SELECT addr, addrs, num_sge, first_len FROM (
        SELECT -1 AS pref, addr, addrs, num_sge, first_len FROM (
            -- THE MOST RECENT POST INTO THAT SLOT, NOT ANY POST INTO IT.
            --
            -- wqe_counter is a 16-bit ring index and a slot is REUSED as the
            -- queue recycles, so (rank, srq, slot) names one buffer at a time
            -- and several over a run. The first version of this arm ignored
            -- that and would have picked min(addr) among every occupant the
            -- slot ever had — the same silent mis-attribution that recycled
            -- wr_id produces, reintroduced by the fix for it. It read as exact
            -- only because the measured run posted 16,414 WQEs into rings of
            -- 8,191 and 32,767 and never completed enough to wrap.
            --
            -- Ordering by post time and taking the last one at or before the
            -- completion is exact under FIFO: a slot cannot be reposted until
            -- its previous occupant has completed, so at most one occupant is
            -- outstanding when a completion for it arrives.
            SELECT p.addr AS addr, 1 AS addrs, p.num_sge AS num_sge,
                   p.first_len AS first_len
            FROM ib_recv_cqe q
            JOIN ib_post_recv p
              ON p.rank = q.rank AND p.subject = q.srq AND p.slot = q.wqe_counter
             AND p.origin = 2 AND p.slot <> 4294967295 AND p.addr > 0
             AND p.temporal_a_ticks <= q.temporal_a_ticks
            WHERE q.rank = c.rank AND q.cq = c.cq AND q.wr_id = c.wr_id
              AND q.qp_num = c.qp_num AND q.srq <> 0
            ORDER BY p.temporal_a_ticks DESC
            LIMIT 1
        ) x WHERE x.addr IS NOT NULL
        UNION ALL
        SELECT 0 AS pref, addr, addrs, num_sge, first_len FROM (
            SELECT min(p.addr) AS addr, count(DISTINCT p.addr) AS addrs,
                   max(p.num_sge) AS num_sge, min(p.first_len) AS first_len
            FROM ib_post_recv p
            WHERE p.origin = 1 AND p.addr > 0
              AND p.rank = c.rank AND p.qp_num = c.qp_num AND p.wr_id = c.wr_id
        ) q WHERE q.addr IS NOT NULL
        UNION ALL
        SELECT 1 AS pref, addr, addrs, num_sge, first_len FROM (
            SELECT min(p.addr) AS addr, count(DISTINCT p.addr) AS addrs,
                   max(p.num_sge) AS num_sge, min(p.first_len) AS first_len
            FROM ib_post_recv p
            WHERE p.origin = 2 AND p.addr > 0
              AND p.rank = c.rank AND p.wr_id = c.wr_id
        ) s WHERE s.addr IS NOT NULL
    ) a ORDER BY a.pref LIMIT 1
) r ON TRUE
LEFT JOIN _peer_of_qp e ON e.rank = c.rank AND e.qp_num = c.qp_num
LEFT JOIN LATERAL (
    SELECT o.id, o.range_base
    FROM (SELECT DISTINCT rank, id, range_base, range_size FROM mem_object) o
    WHERE o.rank = c.rank
      AND r.addr >= o.range_base
      AND r.addr <  o.range_base + o.range_size
    ORDER BY o.range_size
    LIMIT 1
) m ON TRUE
WHERE c.opcode = 128 AND c.status = 0
)sql"},
};

// Derived tables, materialised and written as Parquet in this order.
struct Table {
    const char* name;
    // The columns this table promises, in order. THIS IS THE CONTRACT, and it is
    // the only one the derived layer has.
    //
    // The on-disk format is defended four ways — magic, format version, stride,
    // schema hash, a byte-level conformance test and a Python reader proving
    // portability. These tables, which are what the UI panels, the agent
    // and every suite assertion actually read, had nothing: consumers bound to
    // column names that existed only inside a SQL string in this header. Rename
    // one and nothing failed to build; a panel went blank and a query returned
    // no rows, which reads exactly like a workload that did nothing.
    //
    // Checked against what the SQL really produces every time the layer is
    // defined — see define(). Cheap (one LIMIT 0 per table) and it runs
    // everywhere the reader runs, rather than only where someone remembered to
    // add a test.
    const char* columns;
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
    {"object_access", "owner_rank,object,obj_offset,size,source,actor_rank,actor_device,exact",
     R"sql(
WITH p2p AS (
    -- OWNER IS THE RANK THAT OWNS THE MEMORY, NEVER THE RANK THAT WROTE IT.
    --
    -- This said `g.rank` until 2026-08-17, which made the column mean the
    -- OPPOSITE of what the verbs branch below means by it: there, owner_rank is
    -- `e.peer_rank`, the far end. One column, two meanings, decided by `source`
    -- — so any panel grouping on it silently mixed them.
    --
    -- What it cost is exactly the property `hubspoke` exists to produce.
    -- Measured on hubspoke-44465477: the hub's 64 KiB recv buffer is one object
    -- written by three spokes, and it came out as THREE objects of one writer
    -- each (owner 1/obj 37, owner 2/obj 37, owner 3/obj 37) because the object
    -- id is symmetric-heap-shared and `mem_object` has a row for it on every
    -- rank, so the panel's `owner_rank = m.rank` join succeeded on the wrong
    -- one. `touched by` several GPUs — the whole point of the case — could not
    -- appear. Its ib twin (hubspoke_ib-44578395), which takes the verbs branch,
    -- reported the same buffer correctly all along: one object, three actors,
    -- three disjoint bands.
    --
    -- The join is the one `_nvlink_sends` already uses, and that is deliberate:
    -- "who owns this device's memory" must have ONE answer in this file, or
    -- comm_edges and object_access disagree about the same access.
    -- The local case is stated rather than assumed. `RemoteFilter`
    -- (providers/gpumem/engine.hpp:211) keeps only peer accesses today, so
    -- run_device = mem_device does not currently reach a capture — but a view
    -- that silently needs a filter in another component to be correct is a
    -- landmine for whoever relaxes it. When the access is local the owner is the
    -- rank itself, and no node identity is required to say so.
    SELECT po.owner_rank                            AS owner_rank,
    -- THE OBJECT ID IS PER-RANK, SO IT MUST BE TRANSLATED, NEVER CARRIED OVER.
    --
    -- `object` is an intern handle minted from the buffer's base address in the
    -- RECORDING rank's table, so it is a name only that rank understands. Fixing
    -- owner_rank above without fixing this produced a pair from two different
    -- namespaces: the owner was finally right while the id still belonged to the
    -- actor, and the Objects panel's `owner_rank = m.rank AND object = m.id`
    -- join then landed on whatever unrelated buffer that rank had given the same
    -- number.
    --
    -- Measured on nvshmem_coexist-44696991: id 701 names FOUR different buffers
    -- — 832 B on rank 0, 832 B on rank 1, the 256 KiB symmetric buffer on rank 2,
    -- and 4 B on rank 3. Rank 2's 196,605 accesses to the 256 KiB buffer were
    -- attributed to rank 3's 4-byte one. 139 ids in that capture have ranks
    -- disagreeing about their extent.
    --
    -- hubspoke hid it because its two providers mint few objects and the
    -- symmetric ones happened to align; coexist loads both providers, so the
    -- intern orders diverge and the numbers stop coinciding.
    --
    -- TRANSLATE BY RANGE, which is the rule records.hpp already states for the
    -- reader's own merge: "the range is the fact every emitter agrees on WITHOUT
    -- having to agree on a convention". Under a symmetric heap the same buffer
    -- has the same (base, size) on every PE — verified on this capture, where
    -- (1100757343744, 262144) exists on all four ranks as ids 725/730/701/719 —
    -- so the actor's extent identifies the owner's row exactly.
           om.id                                    AS object,
    -- ANCHOR PER (ACTOR, DEVICE), NOT PER ACTOR.
    --
    -- The partition was `(g.rank, g.object)`, and under a symmetric heap that is
    -- not one window but several glued together: one actor maps the SAME object
    -- id on every peer, at unrelated base addresses. Measured on the same
    -- capture: rank 0 writes object 39 — 2048 bytes — into devices 1, 2 and 3 at
    -- bases 1375635332608 / 1650513239552 / 1925391146496, and the single window
    -- over all three reported offsets up to 549755815936. That is 512 GiB into a
    -- 2 KiB object, and every real access collapsed into the first bin.
    --
    -- mem_device is what separates them, because the mapping is per peer.
    -- The window stays in the ACTOR's namespace (its rank, its device, its own
    -- object id) because the mapping whose anchor is being removed is the
    -- actor's. Partitioning on the translated id would merge two of the actor's
    -- mappings that happen to resolve to one owner buffer.
           po.address - min(po.address) OVER (PARTITION BY po.rank, po.mem_device, po.object)
                                                    AS obj_offset,
           po.size                                  AS size,
           'p2p'                                    AS source,
           po.rank::BIGINT                          AS actor_rank,
           po.run_device::BIGINT                    AS actor_device,
           FALSE                                    AS exact
    FROM (
        SELECT g.*,
               CASE WHEN g.run_device = g.mem_device THEN g.rank ELSE o.rank END AS owner_rank
        FROM gpu_access g
        LEFT JOIN _node n         ON n.rank = g.rank
        LEFT JOIN _device_key   gk ON gk.rank = g.rank AND gk.device = g.mem_device
LEFT JOIN _device_owner o  ON o.node = n.node AND o.dev_key = gk.dev_key
    ) po
    -- The actor's own record of the buffer it touched: this is what gives the
    -- extent, and it is the only thing that crosses ranks meaningfully.
    LEFT JOIN (SELECT DISTINCT rank, id, range_base, range_size FROM mem_object) am
           ON am.rank = po.rank AND am.id = po.object
    -- The same extent as the OWNER recorded it. For a local access this matches
    -- the actor's own row, so the translation is an identity and costs nothing.
    LEFT JOIN (SELECT DISTINCT rank, id, range_base, range_size FROM mem_object) om
           ON om.rank = po.owner_rank
          AND om.range_base = am.range_base
          AND om.range_size = am.range_size
    -- Two ways an access cannot be placed in an object, and neither may vanish
    -- quietly — link_health counts both as unattributed_p2p_accesses:
    --   the owning rank cannot be named (no node identity), or
    --   the owner has no buffer at the actor's extent, which means the memory is
    --   not symmetric and the actor's handle names nothing on the far side.
    -- Guessing either would put real accesses in the wrong object, which is the
    -- failure this whole comment block exists to record.
    WHERE po.owner_rank IS NOT NULL AND om.id IS NOT NULL
),
verbs AS (
    SELECT e.peer_rank                              AS owner_rank,
           m.id                                     AS object,
           s.remote_addr - m.range_base             AS obj_offset,
           s.length                                 AS size,
           'verbs'                                  AS source,
           s.rank::BIGINT                           AS actor_rank,
           rd.device::BIGINT                        AS actor_device,
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
    LEFT JOIN _rank_device rd ON rd.rank = s.rank
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
),
-- THE TWO-SIDED HALF, read from the RECEIVER's records.
--
-- A SEND carries no remote address — the receiver picks the buffer, not the
-- sender — so `verbs` above cannot see this traffic at all and drops it at
-- `remote_addr > 0`. Measured on scale_hubspoke_ib-44517390: 11,520 of the
-- capture's sends are two-sided, 180 per rank, and every one of them was
-- invisible in the object view while being perfectly well recorded.
--
-- The receiver knows what the sender does not. It posted the buffer with
-- ibv_post_recv (addr, wr_id) and reaped a completion carrying that same wr_id
-- and the byte count that actually landed. So the address is recovered locally,
-- on the receiving rank, and only the SENDER's identity needs the QP.
--
-- ONLY opcode 128 (IBV_WC_RECV), NEVER 129. A RECV_RDMA_WITH_IMM completion
-- also consumes a receive WQE, but its payload went to the address the SENDER
-- named — the posted buffer is spent carrying the immediate value alone.
-- Attributing byte_len to the recv buffer there would put the bytes in the
-- wrong object, and `verbs` above has already counted them correctly from the
-- send side. Including 129 here would therefore both misplace and double-count.
--
-- `exact` IS EARNED PER ROW, not asserted for the branch. wr_id is chosen by
-- the application and routinely recycled as a buffer index, so a completion
-- names a post only as precisely as its key allows:
--
--   QP receives  key (rank, qp_num, wr_id) — precise, the endpoint is fixed.
--   SRQ receives key (rank, wr_id) only. An SRQ receive is not bound to a queue
--                pair until a message lands, so qp_num is 0 on the post and
--                real on the completion. Same two-arm rule, and the same reason
--                for it, as recv_completion_accounted.sql — which learned it
--                from 656 false violations on case 07.
--
-- When a key maps to exactly one address the attribution is certain and exact
-- is TRUE. When it maps to several the row is still emitted, with the lowest
-- address and exact = FALSE, because dropping it would silently shrink totals —
-- the failure mode the `?` column exists to prevent in the matrix. link_health
-- counts those rows so an ambiguous capture cannot look like a clean one.
-- The resolution itself lives in the `_recv_attr` VIEW rather than here, because
-- link_health has to count what this branch could not attribute and a kTable
-- cannot reference another kTable — they are COPYed to Parquet, not registered.
-- One definition, two readers, exactly as _peer_of_qp was extracted.
verbs_recv AS (
    SELECT a.recv_rank                              AS owner_rank,
           a.object                                 AS object,
           a.addr - a.range_base                    AS obj_offset,
           a.byte_len                               AS size,
           'verbs_recv'                             AS source,
           a.sender_rank::BIGINT                    AS actor_rank,
           rd.device::BIGINT                        AS actor_device,
           (a.addrs = 1 AND NOT a.spilled)          AS exact
    FROM _recv_attr a
    LEFT JOIN _rank_device rd ON rd.rank = a.sender_rank
    WHERE a.object IS NOT NULL
)
SELECT * FROM p2p
UNION ALL
SELECT * FROM verbs
UNION ALL
SELECT * FROM verbs_recv
)sql"},

    // One row per local endpoint that exists, whether or not it moved bytes.
    // peer_rank is NULL rather than -1 here: an endpoint that was never
    // connected has no peer, which is a different statement from "we could not
    // resolve its peer", and NULL is the only value that does not assert one of
    // them. comm_edges below uses -1 because it must aggregate.
    // Who a rank IS, physically: which host it ran on and which GPU it owns.
    //
    // WHY THIS IS A TABLE AND NOT A PANEL'S `WITH` CLAUSE. Every consumer wants
    // to name a rank the way a person does — "n14 · GPU 0" — and every consumer
    // was deriving it privately: Devices.svelte built `node_ix` inline over
    // `_node` and `_rank_device`, Graph.svelte ordered its arcs by the same
    // dense_rank, and the chat agent could not do it at all, because the views
    // that recipe needs are internal ones it is told never to read. Three
    // derivations of one identity, and the one that is missing is the one a
    // user notices ("which host/GPU pair did the most remote accesses?").
    //
    // `node_ix` is the ordinal, NOT the hostname. `node_hash` is FNV-1a of
    // gethostname (records.hpp) and is one-way: the capture does not contain
    // the name of the machine and no query can recover it. What the ordinal is
    // good for is telling hosts apart and matching what the panels display —
    // both come from `dense_rank() OVER (ORDER BY node_hash)`, which is stable
    // for one capture and means nothing across two.
    //
    // IDENTITY IS (node, device), NEVER device ALONE — across 16 nodes there
    // are sixteen "GPU 0"s, and merging them reports a 64-rank job as a 4-GPU
    // one. Same collision class as keying a peer on qp_num without the lid.
    //
    // `device` is NULL when a rank drives more than one GPU (`devices` says how
    // many). That is a real state, not a gap to paper over.
    {"rank_identity", "rank,node_ix,node_hash,device,devices,label",
     R"sql(
SELECT n.rank,
       (dense_rank() OVER (ORDER BY n.node) - 1)::BIGINT AS node_ix,
       n.node                                            AS node_hash,
       d.device,
       coalesce(d.devices, 0)::BIGINT                    AS devices,
       'n' || (dense_rank() OVER (ORDER BY n.node) - 1)
            || ' · GPU ' || coalesce(d.device::VARCHAR, '?') AS label
FROM _node n
LEFT JOIN _rank_device d ON d.rank = n.rank
ORDER BY n.rank
)sql"},

    // Every numeric vocabulary a reader of this capture will meet, as rows.
    //
    // THE PROBLEM. `pcomm export --csv` decodes enums and `--parquet` does not,
    // and the derived layer is PARQUET-ONLY — `pcomm link` reads Parquet and
    // writes Parquet. So the export that carries comm_edges, transport_mix and
    // link_health is exactly the one that shows `3` where the other shows
    // `Prefetch`, and a reader who wants the derived layer cannot have the
    // names. Three separate application studies noticed independently; two of
    // them reverse-engineered a mapping out of the suite's test files.
    //
    // Parquet not decoding is DELIBERATE and stays that way — see the export's
    // own header, where resolving handles and enums "belongs to the
    // derived-table layer". This is that layer doing its job: the raw column
    // keeps the number the record holds, and the name is one join away.
    //
    //     SELECT k.name FROM gpu_access a
    //     JOIN vocabulary k ON k.vocab = 'MemOp' AND k.value = a.op
    //
    // WHY A TABLE RATHER THAN A DECODED COLUMN ON EACH ONE. `temporal_kind` is
    // on every raw table in the capture; MemOp and MemSpace are on gpu_access;
    // EdgePredicate on attribution. Adding a decoded twin to each would put the
    // vocabulary in as many places as it is used, which is how the five
    // hand-maintained record lists in this repo got out of step. One table, one
    // definition, joinable from anywhere.
    //
    // AND IT IS CHECKED. tools/schema_check.py parses these rows back out of
    // this string and compares them to the enums in libpcomm/schema/pcomm.fbs,
    // so a value renamed in the schema fails the build here rather than
    // publishing a wrong name to every reader. That is the same bargain
    // check_figures.py makes for the record set: the list may be written by
    // hand, but it may not drift.
    //
    // The values are ON DISK — pcomm.fbs says "do not renumber" — so this is a
    // decode of history, not a lookup that follows the source.
    {"vocabulary", "vocab,value,name",
     R"sql(
SELECT * FROM (VALUES
    ('TimeKind', 0, 'None'),
    ('TimeKind', 1, 'Instant'),
    ('TimeKind', 2, 'Interval'),
    ('TimeKind', 3, 'Window'),
    ('TimeKind', 4, 'Counter'),
    ('RecordKind', 0, 'Event'),
    ('RecordKind', 1, 'Entity'),
    ('RecordKind', 2, 'Artifact'),
    ('RecordKind', 3, 'Edge'),
    ('MemOp', 0, 'Load'),
    ('MemOp', 1, 'Store'),
    ('MemOp', 2, 'Atomic'),
    ('MemOp', 3, 'Prefetch'),
    ('MemSpace', 0, 'Global'),
    ('MemSpace', 1, 'Local'),
    ('MemSpace', 2, 'Shared'),
    ('EdgePredicate', 0, 'TouchedObject'),
    ('EdgePredicate', 1, 'MapsToSourceLine'),
    ('EdgePredicate', 2, 'ResidesOn'),
    ('EdgePredicate', 3, 'MapsToQp'),
    ('KernelSource', 0, 'Cupti'),
    ('KernelSource', 1, 'NvbitGlobaltimer'),
    ('KernelSource', 2, 'HostSubmit')
) AS v(vocab, value, name)
)sql"},

    // What each rank was ASKED to do, with the interned strings resolved.
    //
    // NAMED run_provenance AND NOT `invocation`, because `pcomm link` COPYs
    // every kTables entry to <name>.parquet in the same directory the raw
    // tables were written to — so a derived table sharing a raw table's name
    // silently overwrites it. That is a foot-gun in the link path rather than
    // in this view, and it is worth stating where the next person will hit it.
    //
    // The dictionary join is the whole reason this exists. Parquet does not
    // decode handles (see the export's own header: decoding "belongs to the
    // derived-table layer"), so the raw `invocation` table holds five uint32s
    // and answers nothing on its own.
    //
    // Through _intern_name, not intern_text, and the `texts = 1` guard is
    // load-bearing rather than copied. A REPLAYED capture nests one rank-*
    // directory per pass and both load as rank 0 with independent dictionaries,
    // so handle 3 can be an argv in one pass and a kernel name in the other. A
    // direct join to intern_text would emit one row per candidate string and
    // quietly multiply this table; the guard turns that into a NULL and keeps
    // the rank's row. `domain = 2` is InternDomain::String, spelled as a number
    // for the reason the UCT views below give — this layer mounts RAW streams,
    // where an enum is a number.
    //
    // The row surviving with NULLs is the point: it is evidence the engine was
    // alive on that rank, which is a different claim from the rank having no
    // invocation row at all.
    {"run_provenance", "rank,pid,argv,providers,build,images,env",
     R"sql(
SELECT i.rank,
       i.pid,
       CASE WHEN a.texts = 1 THEN a.text END AS argv,
       CASE WHEN p.texts = 1 THEN p.text END AS providers,
       CASE WHEN b.texts = 1 THEN b.text END AS build,
       CASE WHEN m.texts = 1 THEN m.text END AS images,
       CASE WHEN e.texts = 1 THEN e.text END AS env
FROM invocation i
LEFT JOIN _intern_name a ON a.rank = i.rank AND a.domain = 2 AND a.handle = i.argv
LEFT JOIN _intern_name p ON p.rank = i.rank AND p.domain = 2 AND p.handle = i.providers
LEFT JOIN _intern_name b ON b.rank = i.rank AND b.domain = 2 AND b.handle = i.build
LEFT JOIN _intern_name m ON m.rank = i.rank AND m.domain = 2 AND m.handle = i.images
LEFT JOIN _intern_name e ON e.rank = i.rank AND e.domain = 2 AND e.handle = i.env
ORDER BY i.rank
)sql"},

    {"endpoint", "rank,qp_num,lid,peer_lid,peer_qp_num,peer_rank,transport,sends,bytes",
     R"sql(
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

    // Every event, on ONE clock, as a span. The table a timeline reads.
    //
    // The unit is a SPAN and not a point: measured on the 64-rank corpus, every
    // host stream is temporal_kind = 2 (Interval), so a and b are both real and
    // ib_post_send has a median duration of 244 ticks (~122 ns). Collapsing
    // that to an instant would throw away the only column that distinguishes a
    // send that returned immediately from one that blocked 84 us — which is the
    // question a timeline is opened to answer.
    //
    // THE ARITHMETIC HERE IS EXACT ON PURPOSE, AND BOTH HALVES ARE LOAD-BEARING.
    //
    // 1. The subtraction is in HUGEINT, not UBIGINT. `ticks - t0` on a tick
    //    below the first anchor does not go negative in UBIGINT, it wraps to
    //    ~1.8e19 and lands the event 584 years away.
    //
    // 2. The DOUBLE touches ONLY the delta, never the absolute time. This one
    //    cost a rewrite: the first version computed `(w0 + delta*rate)::BIGINT`
    //    with w0 as a DOUBLE, and every duration in the capture came back 0 ns.
    //    A wall-clock nanosecond is ~1.79e18, which needs 19 digits; float64
    //    carries ~15.9, so the ulp up there is **256 ns** — verifiable as
    //    `(1786575955340045312::DOUBLE + 122)::BIGINT` returning the input
    //    unchanged. Median ib_post_send is 122 ns. Every span in this capture
    //    was being rounded to zero width by an addition that looked harmless.
    //    The delta is ~1e10 ticks at most, where float64 resolves far below a
    //    nanosecond, so scaling it is safe; adding it back must be integer.
    //    This is the same lesson query.js records for JSON numbers and UBIGINT
    //    addresses, in a different arithmetic.
    //
    // The clock join is INNER on (rank, domain). A record whose domain no
    // anchor covers cannot be placed on the wall clock at all, and plotting it
    // somewhere plausible would be a fabrication — so it is dropped here and
    // COUNTED in link_health.timeline_unanchored, which is the same trade
    // unresolved_nvlink makes: a hole you can see beats a chart that looks
    // complete.
    {"timeline_span", "rank,stream,ep,bytes,kind,t0_ns,t1_ns",
     R"sql(
SELECT e.rank,
       e.stream,
       e.ep,
       e.bytes,
       -- What [t0_ns, t1_ns] MEANS on this row, and it is not the same question
       -- as what it contains. 1 = Instant (t1 == t0), 2 = Interval — the span
       -- was measured — and 3 = Window, where the two ends BOUND an event whose
       -- real time is unknown. A renderer that treats 3 as 2 asserts a duration
       -- nothing measured; gpu_access is the lane that makes the distinction
       -- load-bearing. See core/time.hpp for the enum.
       e.kind,
       (c.w0::HUGEINT
          + ((e.a::HUGEINT - c.t0::HUGEINT)::DOUBLE * c.ns_per_tick)::HUGEINT)::BIGINT AS t0_ns,
       (c.w0::HUGEINT
          + ((greatest(e.b, e.a)::HUGEINT - c.t0::HUGEINT)::DOUBLE * c.ns_per_tick)::HUGEINT)::BIGINT AS t1_ns
FROM _tl_raw e
JOIN _clock c ON c.rank = e.rank AND c.domain = e.domain
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
    // `scope` ADDED 2026-09-01, and it is the answer to the most corroborated
    // finding of the round-2 application studies. Three unrelated programs put
    // intra-node UCX traffic into this table where it read as fabric: OSU 2
    // operations, GROMACS 83,892, Quantum ESPRESSO 1,435,037. On OSU it was
    // exact and small enough to see whole — four edges, of which two (0->0 and
    // 1->1, the MPI self endpoint) never touched a wire. A reader taking this
    // table as the fabric matrix, which is what it looks like, was wrong by two
    // rows there and by a million on QE.
    //
    // DERIVED FROM TOPOLOGY, NOT FROM TRANSPORT, and that is what makes it
    // possible at all. link_health carried a caveat about this for months whose
    // own text explained why it could not be fixed: "it cannot say WHICH edges:
    // the UCP endpoint and the UCT lane have no join between them". True, and
    // beside the point — "did this leave the node" is a question about the two
    // ranks, and both of them are already resolved right here. `_node` answers
    // it without the UCP/UCT join ever being needed.
    //
    // Uniform across all three lanes rather than bolted onto the ucx one. A
    // verbs edge between two ranks on one node is the same fact and was equally
    // unsayable; nvlink is intra-node by construction and now says so instead of
    // relying on the reader to know it.
    //
    // 'unknown' is its own value and is NEVER folded into 'network'. dst_rank
    // = -1 means the peer was not resolved, and a rank whose node identity is
    // missing (gethostname failed, no LID) cannot be placed either. Guessing
    // 'network' there would inflate exactly the number this column exists to
    // deflate — the same argument transport_mix.scope makes for its own third
    // value.
    {"comm_edges", "src_rank,dst_rank,transport,unit,scope,sends,bytes",
     R"sql(
SELECT e.src_rank, e.dst_rank, e.transport, e.unit,
       CASE WHEN e.dst_rank < 0                        THEN 'unknown'
            WHEN ns.node IS NULL OR nd.node IS NULL    THEN 'unknown'
            WHEN ns.node = nd.node                     THEN 'intra-node'
            ELSE 'network'
       END AS scope,
       e.sends, e.bytes
FROM (
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
  UNION ALL
    -- The UCX lane. `unit` is 'datatype-units', not 'messages', because
    -- UcxTagSend.count is a UCP datatype count and only equals bytes for a
    -- contiguous 8-bit datatype. Its own transport value, so the verbs/devx
    -- subset that CsvAgg's oracle checks is untouched by construction.
    --
    -- A get is recorded with its endpoints SWAPPED: the operation is issued by
    -- this rank, and the bytes travel from the peer to here.
    SELECT CASE WHEN s.inbound THEN coalesce(p.peer_rank, -1) ELSE s.rank END,
           CASE WHEN s.inbound THEN s.rank ELSE coalesce(p.peer_rank, -1) END,
           'ucx', 'datatype-units',
           sum(s.sends)::BIGINT, sum(s.units)::BIGINT
    FROM _ucx_sends s
    LEFT JOIN _ucx_peer_of_ep p ON p.rank = s.rank AND p.endpoint = s.endpoint
    GROUP BY 1, 2
) e
LEFT JOIN _node ns ON ns.rank = e.src_rank
LEFT JOIN _node nd ON nd.rank = e.dst_rank
ORDER BY e.transport, e.src_rank, e.dst_rank
)sql"},

    // WHAT A UCP SEND BECAME ON THE WIRE — the two layers in one relation.
    //
    // A FULL OUTER JOIN, because both sides have rows the other does not and
    // each absence is a different fact worth naming:
    //
    //   tag_matched         both sides. This flow went out through hardware tag
    //                       matching, and `doors` says which of the four.
    //   no_tagged_wire_op   a UCP send with no tagged UCT operation. It is NOT
    //                       "we lost the traffic": the wire operations exist,
    //                       they simply carry no tag, because the message took a
    //                       door that has none. Measured on job 45115011: the
    //                       4 MB size sits here, going out as an active-message
    //                       RTS the peer then pulls with get_zcopy, while the
    //                       64 KB size below the offload ceiling is tag_matched.
    //                       So a row here is usually a PROTOCOL fact about UCX,
    //                       not a hole in the capture. Confirmed on job 45123231:
    //                       exactly two such flows, one per rank, 20 sends and
    //                       83,886,080 units each — which is 20 x 4,194,304, the
    //                       whole 4 MB sweep and nothing else.
    //   wire_only           a tagged UCT operation with no UCP send above it.
    //                       **NO CAPTURE HAS PRODUCED ONE YET** — job 45123231
    //                       has 85 matched flows and zero of these. An earlier
    //                       note here claimed UCX's internal tags (0xfffbfd...)
    //                       were wire-only; the full join says otherwise, they
    //                       carry 19 UCP sends and 19 UCT operations apiece. The
    //                       first reading came from a top-8 listing that had cut
    //                       the UCP rows off, which is a good argument for
    //                       reading a join from the join.
    //                       The status stays because the case is real — wireup
    //                       and flow control produce transport traffic with no
    //                       UCP call above it, measured at 295 UCT against 260
    //                       UCP operations on job 45113099 — but that traffic
    //                       goes through untagged doors, so it lands in
    //                       transport_mix rather than here. tests/
    //                       tag_flow_rule.sh drives this branch, since no
    //                       hardware capture does.
    //
    // **NO link_health FINDING, deliberately.** That table reports a defect by
    // counting one above zero (it is a fixed rubric — see its own head, and
    // note that this comment previously mis-stated it as "empty is the expected
    // state"). Neither absence here is a defect. A
    // `no_tagged_wire_op` row is UCX choosing a protocol without a tag, and a
    // `wire_only` row is UCX doing its own housekeeping — both are facts about
    // the transport, and putting them in link_health would train a reader to
    // ignore the table that exists to be empty. They are reported HERE, as a
    // status column on the flow they describe.
    //
    // `ucp_units` and `uct_payload` are NOT the same quantity and must not be
    // differenced. The first is a UCP datatype count (bytes only for a
    // contiguous 8-bit datatype — the reason comm_edges says 'datatype-units');
    // the second is bytes on the wire, and on TagRndvZcopy it is bytes
    // ADVERTISED rather than sent. They are carried side by side so a reader can
    // see both, and named so nobody subtracts them by accident.
    {"tag_flow", "rank,tag,status,ucp_sends,ucp_units,uct_ops,uct_payload,doors",
     R"sql(
SELECT coalesce(u.rank, t.rank)   AS rank,
       coalesce(u.tag, t.tag)     AS tag,
       CASE WHEN u.rank IS NULL THEN 'wire_only'
            WHEN t.rank IS NULL THEN 'no_tagged_wire_op'
            ELSE 'tag_matched' END AS status,
       coalesce(u.ucp_sends, 0)::BIGINT   AS ucp_sends,
       coalesce(u.ucp_units, 0)::BIGINT   AS ucp_units,
       coalesce(t.uct_ops, 0)::BIGINT     AS uct_ops,
       coalesce(t.uct_payload, 0)::BIGINT AS uct_payload,
       coalesce(t.doors, 0)::BIGINT       AS doors
FROM _ucp_tag_flow u
FULL OUTER JOIN _uct_tag_flow t ON t.rank = u.rank AND t.tag = u.tag
ORDER BY rank, status, tag
)sql"},

    // THE TAG A msg_recv DID NOT CARRY — recovered from the probe that produced
    // its message handle. See `_ucx_probe_of_recv` for why the join is temporal.
    //
    // ONE ROW PER RECEIVE, not per flow, and that is the difference from
    // tag_flow above. tag_flow answers "what did this stream of sends become on
    // the wire", which is a question about an aggregate. This answers "what tag
    // did THIS receive actually match, and how many bytes really arrived" — a
    // field resolution, and aggregating it would destroy the thing being
    // recovered.
    //
    // `offered` AND `delivered` ARE DIFFERENT QUANTITIES AND MUST NOT BE
    // DIFFERENCED CASUALLY. `offered` is UcxTagRecv.count, the capacity the
    // application supplied, in raw UCP datatype units; the record's own comment
    // forbids adding it to a bytes total. `delivered` is UcxTagProbe.length,
    // which ucx_records.hpp calls "the one TRUE byte count anywhere in the UCX
    // record family". They are carried side by side, and named apart, for the
    // same reason ucp_units and uct_payload are.
    //
    // `no_probe` IS A REAL SIGNAL AND NOT AN ERROR. It means a receive consumed
    // a message handle this capture never saw probed: a probe through a door we
    // do not hook, or — the common case — a capture that started after the probe
    // had already run. It gets a row with a NULL tag rather than being dropped,
    // because a silently absent row would read as "this rank posted no
    // handle-keyed receives", which is the opposite of what happened.
    {"tag_recv_resolved", "rank,worker,message,status,sender_tag,offered,delivered",
     R"sql(
SELECT r.rank                              AS rank,
       r.worker                            AS worker,
       r.message                           AS message,
       CASE WHEN p.seq IS NULL THEN 'no_probe'
            ELSE 'probe_matched' END       AS status,
       p.sender_tag                        AS sender_tag,
       r.offered::BIGINT                   AS offered,
       p.delivered::BIGINT                 AS delivered
FROM _ucx_msg_recv r
LEFT JOIN _ucx_probe_of_recv p ON p.rank = r.rank AND p.seq = r.seq
ORDER BY r.rank, r.a
)sql"},

    // WHICH TRANSPORT CARRIED IT — the question comm_edges cannot answer.
    //
    // comm_edges is a rank matrix, and its `ucx` lane is built from UCP calls:
    // it knows rank 0 sent 408 MB to rank 1 and has no idea whether that went
    // over the NIC, over shared memory, or over a CUDA IPC handle. On MN5 all
    // three happen in one job. UctIface's declaration names this as the fact
    // the 800s exist for: "no other record in pcomm can answer that for a UCX
    // application, and comm_edges has been counting shared-memory traffic as
    // fabric traffic for want of it."
    //
    // **THIS IS NOT A LANE OF comm_edges, and the reason is worth stating.**
    // Two of them:
    //
    //   1. There is no peer rank here. A uct_ep is a LANE, not a queue pair;
    //      `uct_ep_get_address` would pack a QP number only through an internal
    //      struct this project refuses to parse, and `uct_ep_query` exposes
    //      none. A comm_edges lane would therefore have dst_rank = -1 on every
    //      row — an entire transport whose matrix is 100% unresolved, which is
    //      precisely the shape link_health exists to flag, not to publish.
    //   2. It would double-count. One UCP send becomes one, two or many UCT
    //      operations, and wireup and flow control produce UCT traffic with no
    //      UCP call above it at all — measured on MN5 job 45113099 as 295
    //      uct_send against 260 ucx_tag_send in one capture. Summing the two
    //      lanes would bill the same bytes twice at two layers.
    //
    // So this is a per-rank PROFILE beside the matrix, not a second matrix.
    //
    // `scope` is a claim about the TRANSPORT UCX CHOSE, deliberately narrower
    // than "did it leave the node". `self`/`sysv`/`posix`/`cma`/`knem`/`xpmem`
    // move bytes without a network stack; `cuda_ipc`/`cuda_copy`/`rocm_*` move
    // them over the local bus. `tcp` is counted as 'network' even though a
    // same-node pair can use it: it went through the kernel network stack,
    // which is a fact, where "left the node" would be a guess. Anything else —
    // a transport not in either list, or an iface whose name did not resolve —
    // is 'unknown' and is NEVER folded into one of the other two.
    //
    // The op name is decoded HERE rather than in the exporter, which is where
    // export_parquet's header says such mappings belong. Grouping by it is not
    // optional: the families measure different things (see _uct_sends), and a
    // row that merged them would carry three incomparable quantities in one
    // number.
    {"transport_mix",
     "rank,tl_name,dev_name,scope,op,ops,out_bytes,in_bytes,advertised_bytes,header_bytes",
     R"sql(
SELECT s.rank,
       s.tl_name,
       s.dev_name,
       s.scope,
       CASE s.op
         WHEN 0  THEN 'am_short'        WHEN 1  THEN 'am_short_iov'
         WHEN 2  THEN 'am_bcopy'        WHEN 3  THEN 'am_zcopy'
         WHEN 4  THEN 'put_short'       WHEN 5  THEN 'put_bcopy'
         WHEN 6  THEN 'put_zcopy'       WHEN 7  THEN 'get_short'
         WHEN 8  THEN 'get_bcopy'       WHEN 9  THEN 'get_zcopy'
         WHEN 10 THEN 'tag_eager_short' WHEN 11 THEN 'tag_eager_bcopy'
         WHEN 12 THEN 'tag_eager_zcopy' WHEN 13 THEN 'tag_rndv_zcopy'
         ELSE '?' || s.op
       END AS op,
       sum(s.ops)::BIGINT              AS ops,
       sum(s.out_bytes)::BIGINT        AS out_bytes,
       sum(s.in_bytes)::BIGINT         AS in_bytes,
       sum(s.advertised_bytes)::BIGINT AS advertised_bytes,
       sum(s.header_bytes)::BIGINT     AS header_bytes
FROM _uct_scoped s
GROUP BY 1, 2, 3, 4, 5
ORDER BY rank, scope, tl_name, dev_name, op
)sql"},

    // One row per work request that was posted and retired, with the time it
    // spent in flight. The Timeline draws this as a lane; this is the same
    // relation with the clock applied and the latency named, for anyone asking
    // the question in SQL rather than by eye — "which qp had the tail", "did
    // latency move when the kernel started".
    //
    // latency_ns comes from the TICK delta scaled once, not from subtracting
    // two converted wall-clock values. The second form rounds each endpoint to
    // its own HUGEINT before subtracting and loses sub-microsecond spans, which
    // is the same float64 trap _clock's comment describes, arriving a third
    // time in a different arithmetic.
    {"wr_flow", "rank,qp_num,wr_id,bytes,opcode,status,wc_opcode,t0_ns,t1_ns,latency_ns",
     R"sql(
SELECT f.rank,
       f.qp_num,
       f.wr_id,
       f.length::BIGINT AS bytes,
       f.opcode,
       f.status,
       f.wc_opcode,
       (c.w0::HUGEINT
          + ((f.a::HUGEINT - c.t0::HUGEINT)::DOUBLE * c.ns_per_tick)::HUGEINT)::BIGINT AS t0_ns,
       (c.w0::HUGEINT
          + ((f.b::HUGEINT - c.t0::HUGEINT)::DOUBLE * c.ns_per_tick)::HUGEINT)::BIGINT AS t1_ns,
       ((f.b::HUGEINT - f.a::HUGEINT)::DOUBLE * c.ns_per_tick)::BIGINT AS latency_ns
FROM _wr_flight f
JOIN _clock c ON c.rank = f.rank AND c.domain = f.domain
)sql"},

    // What the graph could not account for. This table exists so the UI can
    // *show* a hole rather than render a confident matrix over one — "a wrong
    // matrix that looks confident is worse than no matrix".
    //
    // IT IS A FIXED RUBRIC, NOT AN EMPTY TABLE, and this comment used to say
    // the opposite. Every branch below is `SELECT '<finding>', count(*)`, so a
    // perfectly healthy capture returns EVERY row with `n = 0`. The healthy
    // state is `WHERE n > 0` returning nothing, which is what
    // tests/hpc/cases/21_ucx_deep/job.sbatch has always queried.
    //
    // The old wording ("Empty is the expected state; every row is a defect")
    // was wrong about this table's own behaviour, and it did not stay put: the
    // tag_flow comment above quotes it as settled contract, and it was copied a
    // third time into tests/hpc/apps/BRIEF-BLACKBOX.md, where it told a cohort
    // of agents that a clean capture had a dozen defects. Found 2026-08-29 by
    // the 04_llmc study, which read the table instead of its documentation —
    // which is the entire argument for that round being black-box.
    //
    // AND IT WAS ATTACHED TO THE WRONG TABLE. This comment sat above `wr_flow`,
    // two entries up, for long enough that correcting its contents on
    // 2026-08-29 did not move it — I rewrote the paragraph in place and never
    // checked what it was documenting. A reader scrolling to `link_health`
    // found it undocumented; a reader scrolling to `wr_flow` found it described
    // as a health rubric. Both of yesterday's documentation defects were of
    // this kind: prose that stopped matching the code beneath it and was then
    // maintained anyway, without re-reading what it sat on.
    //
    // ---- severity, and why the table grew a column --------------------------
    //
    // `severity` is 'defect' or 'note', and the clean predicate is
    //
    //     SELECT * FROM link_health WHERE n > 0 AND severity = 'defect'
    //
    // returning nothing. It was added on 2026-09-02 because two rows here were
    // structurally incapable of being defects and were counted as such anyway:
    //
    //   * wr_flight_selective_signalling_qps PROVES the traffic beside it is
    //     unpairable by design — the queue pair declared sq_sig_all = 0 — and
    //     then the rubric counted the consequence as a finding. On job
    //     45330280 that is 65.6% of every post in a capture whose other fifteen
    //     rows are zero.
    //   * kernel_rows_submission_only, whose own comment already said "Not an
    //     error", was a finding purely because there was no other shelf.
    //
    // The rule for choosing: a 'defect' is something the LINK could not do —
    // an attribution that failed, a join that dropped rows, an identity that
    // collided. A 'note' is something the CAPTURE can account for — a fact
    // that bounds another table or explains a defect row, and whose non-zero
    // value is the normal case for some legitimate workload.
    //
    // This is the same lesson as the deleted ucx_edges_include_intranode row
    // (see below), arriving from the other direction. That one fired on every
    // healthy capture and was removed; these two fire on every honest NCCL and
    // CUPTI capture and are reclassified. Both failures are the same failure —
    // a rubric that cries defect on the normal case is a rubric nobody reads on
    // the day it is right.
    {"link_health", "finding,severity,n,note",
     R"sql(
SELECT 'colliding_endpoints' AS finding,
       'defect'              AS severity,
       count(*)              AS n,
       'two ranks claim one (lid, qp_num) — impossible on a sane fabric' AS note
FROM _owner WHERE claimants > 1
UNION ALL
SELECT 'unresolved_sends',
       'defect',
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
       'defect',
       coalesce(sum(sends), 0)::BIGINT,
       'NVLink accesses whose peer rank is unknown — this capture records no node identity'
FROM _nvlink_sends WHERE src_rank = -1 OR dst_rank = -1
UNION ALL
-- UCX counted separately again, and for a third distinct reason: its peer join
-- runs on an address PREFIX HASH, not on a queue pair or a node, so it fails in
-- ways neither row above can see. Two of them, deliberately merged into one
-- count because both mean the same thing to a consumer — this edge has no peer:
--   * the endpoint recorded no ucp_ep_create peer address at all (created from a
--     socket address or a connection request, so UcxEpConnection emitted
--     nothing);
--   * the prefix it did record is claimed by more than one worker, so
--     _ucx_peer_of_ep refuses to resolve it rather than guess.
-- Without this row an MPI-over-UCX capture with a fully unresolved matrix
-- reports a clean bill of health, which is exactly the failure the
-- unresolved_nvlink row was added for.
SELECT 'unresolved_ucx',
       'defect',
       coalesce(sum(s.sends), 0)::BIGINT,
       'UCX operations whose peer rank is unknown — no dialled address, or a prefix hash two workers share'
FROM _ucx_sends s
LEFT JOIN _ucx_peer_of_ep p ON p.rank = s.rank AND p.endpoint = s.endpoint
WHERE p.peer_rank IS NULL
UNION ALL
-- A UCT operation whose transport could not be named. Either no uct_iface row
-- carries that iface pointer, or its tl_name handle is claimed by two strings
-- in a replayed capture. Both leave the operation in transport_mix's 'unknown'
-- scope, and an 'unknown' bucket nobody counts is an 'unknown' bucket nobody
-- notices — the same argument unresolved_nvlink was added on.
SELECT 'unnamed_uct_transport',
       'defect',
       coalesce(sum(s.ops), 0)::BIGINT,
       'UCT operations whose transport name did not resolve — no uct_iface row, or an ambiguous intern handle'
FROM _uct_scoped s WHERE s.tl_name IS NULL
-- `ucx_edges_include_intranode` WAS HERE AND IS DELETED, because the thing it
-- warned about is fixed rather than merely tolerated. Its own text said it was
-- "a caveat about another table ... kept because a reader of comm_edges has no
-- other way to learn it", and that sentence stopped being true on 2026-09-01:
-- comm_edges now carries `scope`, so a reader learns it from the table itself,
-- per edge, instead of from a global count that could not say which rows it
-- meant.
--
-- Keeping it would have been worse than useless. It fired on every healthy
-- capture with more than one rank per node — 2 on OSU, 83,892 on GROMACS,
-- 1,435,037 on Quantum ESPRESSO — so a rubric whose contract is "n > 0 is a
-- finding" reported a defect for the normal case. That is how a rubric becomes
-- noise, and noise is what gets a whole table ignored on the day it is right.
--
-- The volume itself is not lost: transport_mix answers it by transport, and
-- comm_edges.scope now answers it by edge.
UNION ALL
-- Two ranks on one node whose gpu_device rows disagree about physical identity.
--
-- FIRES ON A COLLISION, never on degeneracy in general, and the distinction is
-- the lesson from ucx_edges_include_intranode: a rubric row that goes off on
-- every healthy capture reports the normal case as a defect and gets the whole
-- table ignored. UUIDs are healthy on both machines measured (sixteen distinct
-- random values on MN5, four on du04), so a row here means something genuinely
-- went wrong rather than "this deployment is one where UUIDs are dull".
--
-- What it catches is the shape a bad identity actually takes: two DIFFERENT PCI
-- addresses on one node reporting the SAME non-zero UUID. That is the vGPU and
-- container-runtime failure — "GPU-0" on every card — and under it the UUID
-- cross-check would silently confirm a merge instead of catching one. Zero
-- UUIDs are excluded because 0,0 is the documented "unavailable" value, not a
-- claim of identity.
SELECT 'colliding_gpu_uuids',
       'defect',
       coalesce(count(*), 0)::BIGINT,
       'two GPUs on one node report the same UUID — the identity cross-check cannot confirm ownership here'
FROM (
    SELECT n.node, g.uuid_hi, g.uuid_lo
    FROM gpu_device g
    JOIN _node n ON n.rank = g.rank
    WHERE NOT (g.uuid_hi = 0 AND g.uuid_lo = 0)
    GROUP BY n.node, g.uuid_hi, g.uuid_lo
    HAVING count(DISTINCT g.pci_bus) > 1
)
UNION ALL
-- The same failure seen from object_access rather than from comm_edges, and it
-- needs its own row because the two branches drop on different keys:
-- _nvlink_sends only looks at remote accesses (run_device <> mem_device), while
-- object_access attributes local ones too. A capture with no node identity
-- empties the whole p2p half of the object view, and an empty Objects panel
-- must read as a count here rather than as "this workload touched nothing".
SELECT 'unattributed_p2p_accesses',
       'defect',
       coalesce(count(*), 0)::BIGINT,
       'GPU accesses object_access could not place: owning rank unnameable, or the owner has no buffer at the actor''s extent'
FROM (
    SELECT g.rank, g.object, g.run_device, g.mem_device,
           CASE WHEN g.run_device = g.mem_device THEN g.rank ELSE o.rank END AS owner_rank
    FROM gpu_access g
    LEFT JOIN _node n         ON n.rank = g.rank
    LEFT JOIN _device_key   gk ON gk.rank = g.rank AND gk.device = g.mem_device
LEFT JOIN _device_owner o  ON o.node = n.node AND o.dev_key = gk.dev_key
) po
LEFT JOIN (SELECT DISTINCT rank, id, range_base, range_size FROM mem_object) am
       ON am.rank = po.rank AND am.id = po.object
LEFT JOIN (SELECT DISTINCT rank, id, range_base, range_size FROM mem_object) om
       ON om.rank = po.owner_rank
      AND om.range_base = am.range_base
      AND om.range_size = am.range_size
WHERE po.owner_rank IS NULL OR om.id IS NULL
UNION ALL
SELECT 'self_edges',
       'defect',
       coalesce(count(*), 0),
       'a rank sending to itself — the qp_num-collision signature'
FROM (
    SELECT s.rank
    FROM _sends s
    LEFT JOIN _rtr   r ON r.rank = s.rank AND r.qp_num = s.qp_num
    LEFT JOIN _owner o ON o.lid  = r.dest_lid AND o.qp_num = r.dest_qp_num
    WHERE o.rank = s.rank
)
UNION ALL
-- The receive branch of object_access recovers a buffer through a wr_id, which
-- the application owns and may recycle. Where one key named several buffers the
-- row is kept and marked inexact rather than dropped; this is the count that
-- stops that being invisible. Zero means every recovered receive buffer was
-- named by exactly one candidate — the state every capture measured so far is
-- in — but that is a property of the workload's wr_id discipline, not a
-- guarantee of the method, so it is measured per capture rather than assumed.
SELECT 'ambiguous_recv_buffers',
       'defect',
       coalesce(count(*), 0)::BIGINT,
       'recv completions whose wr_id named more than one posted buffer — attributed to the lowest, marked inexact'
FROM _recv_attr WHERE addrs > 1
UNION ALL
-- The other half of the same question: a two-sided receive whose bytes reached
-- no object we can name. Either the receive was posted before the door was
-- hooked, or its buffer lies outside every region the capture knows.
SELECT 'unattributed_recv_completions',
       'defect',
       coalesce(count(*), 0)::BIGINT,
       'successful recv completions resolving to no posted buffer or no known object'
FROM _recv_attr WHERE addr IS NULL OR object IS NULL
UNION ALL
-- The QP arm asserts that (rank, qp_num, wr_id) names one buffer, and until now
-- nothing checked it. It is the stronger key and it is still the application's:
-- a QP that recycles a wr_id across two different buffers breaks it exactly as
-- an SRQ does, and the arm would quietly take the lower address. Measured
-- rather than assumed, for the same reason its SRQ counterpart is.
SELECT 'ambiguous_qp_recv_keys',
       'defect',
       coalesce(count(*), 0)::BIGINT,
       'QP receive keys (qp_num, wr_id) that named more than one buffer'
FROM (
    SELECT rank, qp_num, wr_id
    FROM ib_post_recv WHERE origin = 1 AND addr > 0
    GROUP BY 1, 2, 3 HAVING count(DISTINCT addr) > 1
)
UNION ALL
-- A receive whose bytes did not fit its FIRST scatter entry. ib_post_recv
-- records sge[0] only, so those bytes ran into a buffer the capture cannot
-- name and the object attribution for that row is short by the overflow.
-- Scatter fills in order, so a receive is exactly attributed whenever
-- byte_len <= the first entry's length — which is why num_sge > 1 alone is not
-- the finding, and the length comparison is.
SELECT 'recv_spilled_past_first_sge',
       'defect',
       coalesce(count(*), 0)::BIGINT,
       'receives whose bytes ran past sge[0] into a buffer the capture does not record'
FROM _recv_attr WHERE spilled
UNION ALL
-- Events timeline_span had to drop: no ClockAnchor covers their (rank, domain),
-- so there is no line to carry their ticks onto the wall clock. The timeline
-- shows the rest, and this is the count that stops the omission being invisible
-- — a rank missing from the timeline entirely looks exactly like a rank that
-- did no work. Expect this the day a provider emits a second clock domain (a
-- GPU globaltimer, say) without anchoring it.
SELECT 'timeline_unanchored',
       'defect',
       coalesce(count(*), 0)::BIGINT,
       'events whose (rank, domain) has no clock anchor — omitted from the timeline'
FROM _tl_raw e
LEFT JOIN _clock c ON c.rank = e.rank AND c.domain = e.domain
WHERE c.rank IS NULL
UNION ALL
-- Kernel rows that are SUBMISSION rather than execution. Not an error — the
-- HostSubmit source is a deliberate last resort — but the timeline drops them,
-- so the count has to be visible or a capture with only submission timestamps
-- looks like a capture with no kernels. See core/vocab.hpp KernelSource.
SELECT 'kernel_rows_submission_only',
       'note',
       coalesce(count(*), 0)::BIGINT,
       'kernel rows timed at launch, not execution — excluded from the timeline'
FROM gpu_kernel WHERE source = 2
UNION ALL
-- Sends this capture never saw retire AND CANNOT EXPLAIN. A few mean a trace
-- that stopped with work in flight, which is ordinary; a lot mean the
-- (qp_num, wr_id) key is not holding, and every one of them is a work request
-- missing from the flight lane and from wr_flow. Without this row that lane
-- would simply be short and look complete.
--
-- Posts on a queue pair that DECLARED sq_sig_all = 0 are excluded and counted
-- as a note below — see _wr_unretired for why, and for the arithmetic that
-- guarantees the two rows still sum to every unpaired post.
SELECT 'wr_flight_unretired',
       'defect',
       coalesce(sum(unretired), 0)::BIGINT,
       'work requests posted but never matched to a completion, with no declared cause — absent from the flight lane'
FROM _wr_unretired WHERE sq_sig_all IS DISTINCT FROM 0
UNION ALL
-- The other side of that split, and a note rather than a defect: these posts
-- were never going to produce a completion, because their queue pair said at
-- creation that it does not signal every work request. They are absent from the
-- flight lane for a reason the capture states, not for one it cannot find.
SELECT 'wr_flight_unretired_selective',
       'note',
       coalesce(sum(unretired), 0)::BIGINT,
       'posts on queue pairs that declared sq_sig_all = 0 — no completion was ever going to be emitted for them'
FROM _wr_unretired WHERE sq_sig_all = 0
UNION ALL
-- Queue pairs that post more work requests than they complete and DO NOT SAY
-- WHY. These use UNSIGNALLED work requests: a WR posted without
-- IBV_SEND_SIGNALED produces no completion and is retired silently by the next
-- one, so no completion can be attributed to a specific send. They are excluded
-- from the flight lane entirely rather than paired positionally, which would
-- print a latency measured from the wrong send.
--
-- RENAMED from wr_flight_unsignalled_qps, because that name became a lie the
-- moment the declared cases moved out: what is left is precisely the set whose
-- cause the capture CANNOT name — selective signalling on a capture written
-- before ib_qp_signalling existed, or a run that stopped with work in flight.
SELECT 'wr_flight_unexplained_qps',
       'defect',
       coalesce(count(*), 0)::BIGINT,
       'queue pairs posting more WRs than they complete, with no sq_sig_all declaration to explain it'
FROM _wr_pairable
WHERE coalesce(sends, 0) <> coalesce(comps, 0) AND sq_sig_all IS DISTINCT FROM 0
UNION ALL
-- The ones the capture CAN name the cause for. A queue pair that declared
-- sq_sig_all = 0 and then completed fewer WRs than it posted IS doing selective
-- signalling — no inference, the queue pair said so at creation.
--
-- A NOTE, NOT A DEFECT, and this row is the reason the column exists. It was a
-- finding, so a capture that proved its own traffic unpairable by design scored
-- three findings for having done so. See the severity paragraph above the SQL.
SELECT 'wr_flight_selective_signalling_qps',
       'note',
       coalesce(count(*), 0)::BIGINT,
       'unpairable queue pairs that declared sq_sig_all = 0 — certainly selective signalling'
FROM _wr_pairable
WHERE coalesce(sends, 0) <> coalesce(comps, 0) AND sq_sig_all = 0
UNION ALL
-- Completions that appeared to precede their own post. See the WHERE in
-- _wr_flight: these are dropped rather than drawn at negative width.
SELECT 'wr_flight_backwards',
       'defect',
       coalesce(count(*), 0)::BIGINT,
       'completions timed before the send they retire — capture began mid-flight'
FROM (
    WITH s AS (
        SELECT rank, qp_num, wr_id, temporal_a_ticks AS a,
               row_number() OVER (PARTITION BY rank, qp_num, wr_id
                                  ORDER BY temporal_a_ticks) AS gen
        FROM ib_post_send
    ), c AS (
        SELECT rank, qp_num, wr_id, temporal_a_ticks AS a,
               row_number() OVER (PARTITION BY rank, qp_num, wr_id
                                  ORDER BY temporal_a_ticks) AS gen
        FROM ib_poll_cq WHERE (opcode & 128) = 0
    )
    SELECT s.rank
    FROM s JOIN c
      ON c.rank = s.rank AND c.qp_num = s.qp_num AND c.wr_id = s.wr_id AND c.gen = s.gen
    WHERE c.a < s.a
)
UNION ALL
-- ucx_deep declined to patch uct_iface_ops_t, so this capture has NO UCT data
-- plane at all.
--
-- THE MOST EXPENSIVE MISSING ROW IN THIS TABLE'S HISTORY. The guard is correct
-- and it announces itself precisely — on stderr, at load time, in a job log
-- nobody keeps. What survives into the capture is uct_binding.accepted = 0, a
-- column no document mentioned, in a table no derived view read. The visible
-- symptom is that `transport_mix` — the table a reader is pointed at to answer
-- "did this leave the node" — comes back EMPTY, which is indistinguishable from
-- "this workload sent nothing".
--
-- That cost two days on the LAMMPS round-2 study (jobs 45329318 / 45330017):
-- the study filed a correct refusal as a tool defect, and its central
-- prediction went unanswered because nothing said the answer was withheld
-- rather than absent. A refusal that only exists in stderr is a refusal the
-- capture does not carry.
--
-- Counted per RANK, not per capture: a heterogeneous allocation can bind on
-- some nodes and refuse on others, and "3 of 16" is the number that tells a
-- reader their transport_mix is partial rather than empty.
SELECT 'uct_ops_patch_refused',
       'defect',
       coalesce(count(*), 0)::BIGINT,
       'ranks where ucx_deep refused to patch an unverified uct_iface_ops_t — uct_send and transport_mix are empty for them'
FROM uct_binding WHERE accepted = 0
UNION ALL
-- The opposite lever, and a note because it is a deliberate act rather than a
-- failure: PCOMM_UCX_DEEP_FORCE overrode the guard. The run patched a layout
-- nobody verified, so every uct_* row in this capture is trustworthy only as
-- far as that decision was. A capture must carry the fact that it was taken.
SELECT 'uct_ops_patch_forced',
       'note',
       coalesce(count(*), 0)::BIGINT,
       'ranks where PCOMM_UCX_DEEP_FORCE overrode the version guard — uct_* rows rest on an unverified ops layout'
FROM uct_binding WHERE forced = 1
)sql"},
};

// Define the whole derived layer as views on an open connection.
//
// `--link` materialises kTables to Parquet instead, because its output is a
// fixture other tools read. A UI holds the capture in memory and only ever
// queries, so a view is the same answer without the copy — and, more to the
// point, without a temp directory the server would have to own and clean up.
// Does this table still produce the columns it promises?
//
// ASKS THE SQL, NOT THE CATALOG, and that is what makes it work in both callers.
// `pcomm ui` defines the layer as views, so a relation exists; `pcomm link` COPYs
// each entry straight to Parquet and never CREATEs one, so a catalog lookup here
// would silently check nothing on exactly the path that writes the fixture other
// tools read. That is the same trap link_health fell into — see the comment
// beside it in pcomm-read/main.cpp.
//
// LIMIT 0 so it costs a plan and no rows.
inline bool columns_match(duckdb_connection con, const Table& t) {
    duckdb_result     res;
    const std::string q = "SELECT * FROM (" + std::string(t.sql) + ") LIMIT 0";
    if (duckdb_query(con, q.c_str(), &res) != DuckDBSuccess) {
        duckdb_destroy_result(&res);
        return false;  // the CREATE already reported why
    }
    std::string got;
    for (idx_t i = 0; i < duckdb_column_count(&res); i++) {
        if (i)
            got += ',';
        got += duckdb_column_name(&res, i);
    }
    duckdb_destroy_result(&res);
    if (got == t.columns)
        return true;
    std::fprintf(stderr,
                 "pcomm: %s does not match its declared columns\n"
                 "  declared: %s\n"
                 "  produced: %s\n"
                 "  Update the `columns` line beside its SQL in "
                 "pcomm-read/link_sql.hpp — and check ui/, agent/ and\n"
                 "  tests/hpc/*/asserts/ for consumers of the old name.\n",
                 t.name, t.columns, got.c_str());
    return false;
}

inline std::size_t define(duckdb_connection con) {
    std::size_t failed = 0;
    for (const auto& v : kViews)
        if (!duck::exec(con, v.sql, v.name))
            failed++;
    for (const auto& t : kTables) {
        if (!duck::exec(con, "CREATE OR REPLACE VIEW \"" + std::string(t.name) + "\" AS " + t.sql,
                        t.name)) {
            failed++;
            continue;
        }
        if (!columns_match(con, t))
            failed++;
    }
    return failed;
}

}  // namespace pcomm::link
