-- requires: mem_object params.pitch_width params.pitch_height
-- The two routes whose extent the workload cannot fix in advance.
--
-- cudaMallocPitch: the driver pads each row to a pitch it chooses, and the
-- object is pitch * height — the padding is part of the allocation an address
-- can land in. So: exactly one CuMemAlloc object of at least width * height and
-- less than (width + 4096) * height (no pitch rounds a row up by a page).
--
-- cuMemMap: two chunks mapped into one reservation are two objects of one size.
-- Asserting 2 is also a measurement: the driver's own stream-ordered pool maps
-- memory too, and if that ever reached a door this count would say so. The
-- workload ALSO maps a third chunk through an imported handle (exported as an
-- fd and imported back, as NCCL maps a peer's buffer); that mapping is not an
-- object, so a 3 here means imported memory is being recorded as the mapper's.
SELECT 'cudaMallocPitch did not produce one object of pitch x height' AS problem, objects
FROM (SELECT count(*) AS objects FROM mem_object m, params p
      WHERE m.origin = 'CuMemAlloc'
        AND CAST(m.size AS BIGINT) >= p.pitch_width * p.pitch_height
        AND CAST(m.size AS BIGINT) <  (p.pitch_width + 4096) * p.pitch_height) t
WHERE objects != 1
UNION ALL
SELECT 'cuMemMap did not produce two equal-sized objects',
       (SELECT count(*) FROM mem_object WHERE origin = 'CuMemMap')
WHERE (SELECT count(*) FROM mem_object WHERE origin = 'CuMemMap') != 2
   OR (SELECT count(DISTINCT size) FROM mem_object WHERE origin = 'CuMemMap') != 1;
