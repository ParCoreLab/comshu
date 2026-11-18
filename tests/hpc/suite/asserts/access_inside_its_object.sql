-- requires: object_access object_life
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

-- A UCX row's size is a DATATYPE COUNT (object_access.unit), not an extent, so
-- it contributes only the byte its offset names: the offset itself must still be
-- inside the object, which is the anchor this check exists to test, while
-- "offset + count" would compare elements to bytes.
-- AGAINST THE INCARNATION IT WAS CREDITED TO (object_life), not against any
-- extent the handle ever had: a handle is a base address, and since the engine
-- records cudaFree an address can carry a 140 KB buffer and then a 319 KB one
-- (LAMMPS r2). Checking against the larger would pass an offset that ran off
-- the end of the smaller. A row whose incarnation is unnamed (life NULL, a p2p
-- access to an extent allocated more than once) is checked against that
-- extent's smallest size, which is the strict reading.
SELECT 'access runs past the end of its object' AS problem,
       a.owner_rank, a.object, a.life,
       max(a.obj_offset + CASE WHEN a.unit = 'bytes' THEN a.size ELSE 1 END) AS max_end,
       min(l.size) AS range_size
FROM object_access a
JOIN object_life l
  ON l.rank = a.owner_rank AND l.object = a.object
 AND (a.life = l.life OR a.life IS NULL)
GROUP BY a.owner_rank, a.object, a.life
HAVING max(a.obj_offset + CASE WHEN a.unit = 'bytes' THEN a.size ELSE 1 END) > min(l.size);
