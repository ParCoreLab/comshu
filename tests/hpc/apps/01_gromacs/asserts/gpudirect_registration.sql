-- requires: mem_object params.gpudirect
-- THE HEADLINE. What GMX_DISABLE_DIRECT_GPU_COMM changes is which memory the
-- NIC is asked to register, and that is a fact the capture holds.
--
-- ORACLE. src/gromacs/taskassignment/decidegpuusage.cpp:794 — direct GPU
-- communication is ON by default in a CUDA + library-MPI build and is turned
-- off only by GMX_DISABLE_DIRECT_GPU_COMM. On the ON path the pointer handed to
-- MPI is device memory:
--   domdec/gpuhaloexchange_impl_gpu.cpp:489  MPI_Irecv(recvPtr,  ...)   Float3* on device
--   domdec/gpuhaloexchange_impl_gpu.cpp:492  MPI_Send (sendPtr,  ...)   Float3* on device
--   ewald/pme_pp_comm_gpu_impl_gpu.cpp:256   MPI_Isend(sendptr_x, ...)  device buffer
-- On the OFF path GROMACS copies to host staging first and MPI only ever sees
-- host memory:
--   domdec/gpuhaloexchange_impl_gpu.cpp:517-525  copyFromDeviceBuffer -> h_outOfPlace* -> MPI
--   domdec/haloexchange.cpp:89,126               MPI_Isend/MPI_Irecv on host buffers
--   ewald/pme_pp.cpp:296                         MPI_Isend(x.data(), ...) host x
--
-- So the discriminator is NOT the message count and NOT the byte count — those
-- are the same either way, which pme_coord_volume.sql asserts directly. It is
-- whether any range the ibverbs provider saw registered is device memory.
-- mem_object.device is filled by comshu::core::device_of() at ibv_reg_mr /
-- ibv_reg_mr_iova2 / ibv_reg_dmabuf_mr (providers/ibverbs/ibverbs_provider.cpp
-- :580, :622, :914), and is 65535 (kUnknownDevice) for host memory.
--
-- `gpudirect` is the ONE parameter that differs between the two variants in
-- meta.json, which is what lets this file be the specification for both arms
-- rather than two queries nobody can diff.
--
-- READ A FAILURE CAREFULLY, because the two directions mean opposite things:
--   expected 1, observed 0 -> either GROMACS did not take the direct path (read
--       the mdrun log: "GPU-aware MPI detected, enabling direct GPU
--       communication"), or the traffic left through a door comshu does not hook
--       and the capture is BLIND, not the run cheaper. That is claim (4), not a
--       performance result, and confusing the two is the worst outcome
--       available here.
--   expected 0, observed 1 -> UCX registered device memory for something other
--       than GROMACS's buffers, and the discriminator is not as sharp as this
--       assertion claims.
WITH dev AS (
    SELECT count(*) AS n,
           coalesce(sum(CAST(size AS INTEGER)), 0) AS bytes,
           count(DISTINCT CAST(device AS INTEGER)) AS devices
    FROM mem_object
    WHERE CAST(device AS INTEGER) <> 65535
)
SELECT 'NIC-registered device memory does not match the direct-GPU-comm setting' AS problem,
       (SELECT gpudirect FROM params) AS gpudirect_expected,
       dev.n AS device_memory_objects, dev.bytes AS device_memory_bytes,
       dev.devices AS distinct_devices,
       (SELECT count(*) FROM mem_object) AS all_objects
FROM dev
WHERE (CASE WHEN dev.n > 0 THEN 1 ELSE 0 END) <> (SELECT gpudirect FROM params);
