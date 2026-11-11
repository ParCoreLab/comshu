-- requires: gpu_device rank_identity params.ranks params.nodes params.ranks_per_node
-- The claim `gpu_device` exists to make: a device ORDINAL is node-local, a UUID
-- is global, and a PCI bus address is neither.
--
-- WHY 16 GPUs ACROSS 4 IDENTICAL NODES IS THE SMALLEST CONFIGURATION THAT CAN
-- SHOW THIS. On one node, ordinal and UUID are in bijection and the record has
-- nothing to say. On two, "repeats across nodes" and "appears twice" are the
-- same observation. Four nodes of four make the three quantities pull apart:
-- there are 16 GPUs, 4 ordinals, and — because the four nodes are the same
-- machine — 4 bus addresses. `libcomshu/schema/comshu.fbs:210` says the record
-- exists precisely because "the bus id repeats across nodes", and nothing in
-- this project had measured that on an application.
--
-- WHAT EACH CLAUSE CATCHES:
--   1. UUIDs that are not globally unique. A GPU UUID is a hardware serial; two
--      distinct physical GPUs reporting one UUID would break every downstream
--      identity join, and link_health has a `colliding_gpu_uuids` finding for
--      exactly that case within a node — this is the cross-node half, which
--      that finding does not cover.
--   2. A node that does not see its own four GPUs. Each of the four ranks on a
--      node enumerates the devices it can see, so the union over a node must be
--      exactly gpus-per-node distinct UUIDs. Fewer means the enumeration is
--      partial; more means it is crossing a node boundary, which is the failure
--      the record is meant to prevent.
--   3. THE POINT OF THE RECORD: an ordinal that names more than one UUID. It
--      MUST — ordinal 0 is a different physical GPU on each of the four nodes —
--      so this clause fires when the capture claims otherwise, i.e. when a
--      device ordinal has been treated as globally meaningful somewhere.
--   4. The bus address must repeat across nodes for the same reason, so a
--      capture reporting 16 distinct bus addresses on 16 identical GPUs would be
--      reporting something other than a bus address.
--
-- CLAUSES 3 AND 4 ARE "MUST NOT BE UNIQUE" CHECKS, which is unusual and is why
-- they are written as counts rather than as offending rows: an assertion that
-- passes when a column happens to be degenerate would be the wrong way round
-- here.
SELECT 'two physically distinct GPUs report the same UUID' AS problem,
       distinct_uuids, (SELECT ranks FROM params) AS want, '-'
FROM (SELECT count(DISTINCT (uuid_hi, uuid_lo)) AS distinct_uuids FROM gpu_device) x
WHERE distinct_uuids <> (SELECT ranks FROM params)

UNION ALL

SELECT 'a node does not enumerate exactly its own GPUs',
       uuids_seen, (SELECT ranks_per_node FROM params), CAST(node_hash AS VARCHAR)
FROM (SELECT r.node_hash, count(DISTINCT (g.uuid_hi, g.uuid_lo)) AS uuids_seen
      FROM gpu_device g JOIN rank_identity r USING (rank)
      GROUP BY r.node_hash) y
WHERE uuids_seen <> (SELECT ranks_per_node FROM params)

UNION ALL

SELECT 'a device ordinal names only one GPU, so the ordinal is being treated as global',
       count(DISTINCT (uuid_hi, uuid_lo)), (SELECT nodes FROM params), CAST(ordinal AS VARCHAR)
FROM gpu_device GROUP BY ordinal
HAVING count(DISTINCT (uuid_hi, uuid_lo)) <> (SELECT nodes FROM params)

UNION ALL

SELECT 'the PCI bus address does not repeat across the identical nodes',
       distinct_pci, (SELECT ranks_per_node FROM params), '-'
FROM (SELECT count(DISTINCT pci_bus) AS distinct_pci FROM gpu_device) z
WHERE distinct_pci <> (SELECT ranks_per_node FROM params)

UNION ALL

SELECT 'gpu_device is empty, so this assertion checked nothing', 0, 0, '-'
WHERE (SELECT count(*) FROM gpu_device) = 0;
