-- Every copy's context device is also its SOURCE device on this workload, and
-- never its destination.
--
-- This is the claim `ctx_device` rests on, checked where the workload makes it
-- unambiguous: rank r pushes FROM its own buffer INTO the peer's, so the
-- context it issued in is the source end. If CUPTI's `deviceId` were the
-- destination — or an unrelated ordinal — _device_owner would hand every rank
-- the GPU of its neighbour and the ring would come out reversed while still
-- looking like a ring. Names no ordinal: it compares three columns of the same
-- row against each other.
SELECT rank, src_device, dst_device, ctx_device, count(*) AS n
FROM cuda_memcpy
WHERE CAST(ctx_device AS INTEGER) != CAST(src_device AS INTEGER)
   OR CAST(ctx_device AS INTEGER) = CAST(dst_device AS INTEGER)
GROUP BY rank, src_device, dst_device, ctx_device;
