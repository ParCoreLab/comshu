-- requires: ucx_tag_send params.halo_min_bytes
-- Every halo message is a whole number of MPI_DOUBLEs.
--
-- CommBrick and CommKokkos send nothing but MPI_DOUBLE buffers: every MPI_Send
-- in src/comm_brick.cpp (:505, :519, :564, :684, :848, :932, ...) and in
-- src/KOKKOS/comm_kokkos.cpp (:166, :188, :198) passes MPI_DOUBLE, and
-- OpenMPI's pml_ucx hands ucp_tag_send_nbx a contiguous BYTE count
-- (ompi/mca/pml/ucx/pml_ucx.c), so ucx_tag_send.count is bytes and must be a
-- multiple of 8.  Restricted to messages above halo_min_bytes so the claim is
-- about LAMMPS's own traffic and not about MPI's internal control messages.
SELECT s.rank, s.count AS msg_bytes, count(*) AS occurrences
FROM ucx_tag_send s
WHERE s.count >= (SELECT halo_min_bytes FROM params)
  AND s.count % (SELECT double_bytes FROM params) != 0
GROUP BY 1, 2
ORDER BY 1, 2;
