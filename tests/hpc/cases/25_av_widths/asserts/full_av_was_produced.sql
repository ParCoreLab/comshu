-- requires: mlx5_device_send params.expected_av_ds
-- The promoted capture must carry the address-vector width this case exists for.
--
-- $OUT is the roce_dc leg — DC over an Ethernet RoCE v2 port, where lid == 0
-- clears support_half_av_seg and NVSHMEM emits the 48-byte full AV. If this
-- capture's RDMA writes are not at expected_av_ds, one of two things happened
-- and both matter: the transport quietly fell back to a device that allows the
-- compact form, or the decoder stopped deriving the width correctly. Either way
-- the branch this case was built to keep exercised is no longer being exercised,
-- and it would go unnoticed exactly as it did for the weeks before job 45580050.
--
-- THE VACUITY HALF IS THE IMPORTANT HALF. A capture with no RDMA writes at all
-- satisfies "no row has the wrong width" perfectly, and that is the state this
-- assertion is most likely to face: the transport declining the RoCE port, the
-- doorbell arm not engaging, an export that produced an empty table. Every one
-- of those looks like a pass to the first clause. So the second clause fails on
-- a capture that decoded nothing, which is the difference between an assertion
-- and a query that cannot fail.
--
-- expected_av_ds comes from meta.json rather than being written as 3 here. The
-- rule sql_check.sh enforces is that an assertion references params or names no
-- specific value; a literal 3 in this file would be today's output enshrined in
-- SQL, which is a golden wearing a costume.
SELECT 'rdma_write at the wrong address-vector width' AS problem,
       rank,
       av_ds AS width_seen,
       count(*) AS n
FROM mlx5_device_send
WHERE opcode_name = 'rdma_write'
  AND CAST(av_ds AS INTEGER) != (SELECT expected_av_ds FROM params)
GROUP BY rank, av_ds

UNION ALL

SELECT 'capture decoded no rdma_write rows at all — nothing was exercised',
       NULL, NULL, 0
WHERE NOT EXISTS (
    SELECT 1 FROM mlx5_device_send WHERE opcode_name = 'rdma_write')
