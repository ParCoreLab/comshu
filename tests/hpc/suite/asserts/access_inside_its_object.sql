-- requires: object_access mem_object
-- An access attributed to an object must FIT IN that object.
--
-- An invariant, naming no rank, device or object: obj_offset is an offset into
-- the object it is attributed to, so offset + size cannot exceed the object's
-- size. Violating it means the offset was computed against the wrong anchor, and
-- the object view's heatmap is then binning real accesses into a range that does
-- not exist.
--
-- It has already caught one: object_access's p2p window partitioned on
-- (rank, object), which under a symmetric heap glues one actor's mappings of the
-- SAME object id on several peers into one window. On hubspoke-44465477 a
-- 2048-byte object reported offsets up to 549755815936 — 512 GiB into 2 KiB —
-- and every real access collapsed into the first bin.
--
-- DISTINCT because mem_object carries a row per rank for the same symmetric
-- object, and joining without it multiplies each access by the rank count.
SELECT 'access runs past the end of its object' AS problem,
       a.owner_rank, a.object, max(a.obj_offset + a.size) AS max_end, m.range_size
FROM object_access a
JOIN (SELECT DISTINCT rank, id, range_size FROM mem_object) m
  ON m.rank = a.owner_rank AND m.id = a.object
GROUP BY a.owner_rank, a.object, m.range_size
HAVING max(a.obj_offset + a.size) > m.range_size;
