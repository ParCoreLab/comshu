-- requires: node_identity params.ranks params.nodes
-- Every rank names its machine, and the machines add up to the job.
--
-- WHY THIS EXISTS. Node identity is the foundation the whole peer graph stands
-- on: a device ordinal is NODE-LOCAL, so rank 0 and rank 4 both run on
-- `run_device 0` and are different GPUs. `_node` (link_sql.hpp) resolves a rank
-- to a machine, every device lookup is scoped by it, and `comm_edges` reports
-- `dst_rank = -1` where it cannot. Nothing checked that it works.
--
-- It used to be inferred from LIDs — "ranks sharing an HCA port share a node" —
-- which is a fallback that fails exactly where it is most needed: a pure-NVLink
-- run has no LID at all, and case 05's 1.18M peer accesses once resolved to
-- "unknown peer" for that reason. The capture now RECORDS the node
-- (`node_identity.node_hash`, from gethostname) and `_node` prefers the record
-- over the inference. This asserts the record, because the inference is the
-- thing being retired and an untested replacement is not an improvement.
--
-- THREE CLAUSES, one per way it can be wrong, and the third is the one that
-- matters most.
--
--   1. The wrong NUMBER of machines. Derived from meta.json's `nodes`, not
--      observed: a 2-node job whose ranks all report one hash has either lost a
--      node or merged two, and both make every device lookup on the missing
--      side resolve to the wrong rank.
--
--   2. A rank whose hash is ZERO. gethostname failed there. link_sql treats 0
--      as absent rather than as a node — deliberately, since otherwise every
--      failed rank across a job would merge onto one fictional machine — so
--      such a rank silently falls out of every node-scoped view. Silent is the
--      problem; this makes it loud.
--
--   3. A rank with NO ROW AT ALL. This is the vacuity clause, and without it
--      the first two are close to unfalsifiable: a capture where node_identity
--      is empty has the right number of distinct hashes (zero != nodes catches
--      that) but a capture missing *some* ranks can still show the right node
--      count while the absent ranks resolve to nothing. Compared against
--      params.ranks, which is the job as launched.
--
-- Measured before it was written, across seven cases spanning 1-8 ranks and 1-4
-- nodes (job 45591634): every one matches its declared `nodes` exactly, with no
-- zero hashes anywhere. So this asserts a property the suite already has rather
-- than a wish — and it is now the thing that notices when it stops being true.
--
-- DO NOT CAST node_hash, and this is the interesting part of the file.
--
-- The house rule is "CAST is not decoration" — sql_check.sh loads these CSVs
-- into sqlite where every column is TEXT, so lexicographic min() and `'0' <> 0`
-- bite, and decode_starts_at_the_queue_head.sql cost a false alarm to exactly
-- that. Applied here, the rule produces a DIFFERENT false alarm, in the other
-- direction, and the first version of this file shipped it: every real 2-node
-- capture reported one machine.
--
-- node_hash is a **uint64**. sqlite's INTEGER is SIGNED 64-bit, so
-- `CAST('11099171855685771097' AS INTEGER)` does not overflow or error — it
-- SATURATES to 9223372036854775807. Both of case 07's node hashes are above
-- INT64_MAX, so both cast to the same number and count(DISTINCT) collapsed 2
-- machines into 1.
--
-- Compared as TEXT instead, which is exact: two hashes are the same machine iff
-- their decimal strings match, and the engine writes them with %llu so there is
-- no leading-zero or spacing ambiguity to worry about. `rank` keeps its CAST —
-- it is a small integer and the original reasoning applies to it.
--
-- The rule, stated properly for whoever hits this next: CAST when the column is
-- a NUMBER that TEXT comparison would order or compare wrongly. Do not CAST an
-- identifier that merely looks numeric — a hash, a key, a GUID — because the
-- cast can lose information the string never had a problem with.
-- Written as a subquery compared in WHERE, not as an aggregate with HAVING:
-- sqlite rejects `HAVING` without a `GROUP BY` ("a GROUP BY clause is required
-- before HAVING"), and a bare aggregate over the whole table has no grouping to
-- give it. Same shape as the third clause below, which is why both read alike.
SELECT 'job resolved to the wrong number of machines' AS problem,
       (SELECT count(DISTINCT node_hash) FROM node_identity
         WHERE node_hash <> '0')                             AS nodes_seen,
       (SELECT nodes FROM params)                            AS nodes_expected
WHERE (SELECT count(DISTINCT node_hash) FROM node_identity
        WHERE node_hash <> '0') <> (SELECT nodes FROM params)

UNION ALL

SELECT 'rank could not name its machine (gethostname failed; it falls out of every node-scoped view)',
       CAST(rank AS INTEGER),
       NULL
FROM node_identity
WHERE node_hash = '0'

UNION ALL

SELECT 'not every rank recorded a node',
       (SELECT count(DISTINCT CAST(rank AS INTEGER)) FROM node_identity),
       (SELECT ranks FROM params)
WHERE (SELECT count(DISTINCT CAST(rank AS INTEGER)) FROM node_identity)
      <> (SELECT ranks FROM params)
