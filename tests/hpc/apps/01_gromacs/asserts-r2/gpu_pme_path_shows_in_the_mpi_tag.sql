-- requires: ucx_tag_send params.gpudirect params.gmx_tag_coord params.gmx_tag_coord_gpu params.ompi_tag_shift
-- The direct-GPU-communication knob changes the MPI tag, and the tag is in the
-- capture.
--
-- ROUND 1'S UNFINISHED BUSINESS. That study ran both arms of
-- GMX_DISABLE_DIRECT_GPU_COMM and found the traffic byte-identical: 103,292
-- sends against 103,215, and the PME rank's outbound total equal TO THE BYTE
-- across a knob that changes which memory the buffer lives in. Its conclusion
-- (F4) was that ucx_tag_send carries no memory type and therefore cannot tell a
-- host-staged exchange from a GPU-direct one, and that the evidence would have
-- to come from the verbs layer -- which on this stack sees nothing.
--
-- That conclusion was right about the memory type and wrong about the
-- consequence, because GROMACS itself uses a DIFFERENT MPI TAG for the two
-- paths:
--
--   ewald/pme_pp_communication.h:54
--       enum { ..., eCommType_COORD = 7, eCommType_COORD_GPU = 8, ... }
--   ewald/pme_pp.cpp:296                    (host path)
--       MPI_Isend(x.data(), n*sizeof(rvec), MPI_BYTE, pme_nodeid,
--                 eCommType_COORD, ...)
--   ewald/pme_pp_comm_gpu_impl_gpu.cpp:256  (direct path)
--       MPI_Isend(sendptr_x, sendSize*DIM, MPI_FLOAT, pmeRank_,
--                 eCommType_COORD_GPU, ...)
--
-- and Open MPI puts that tag in the top 24 bits of the UCP tag
-- (pml_ucx_request.h:42, shift = RANK_BITS + CONTEXT_BITS = 40). So the
-- discriminator round 1 went looking for at the ibverbs registration door was
-- sitting in a column it had already exported.
--
-- TWO CLAUSES, AND THE SECOND IS THE ONE THAT CANNOT BE EXPLAINED AWAY. If the
-- ON arm shows no tag 8 that may mean the GPU PME<->PP path was never selected
-- -- a decision GROMACS makes from the task assignment, nothing to do with
-- comshu, and one round 1's evidence positively suggests, since a PME rank whose
-- outbound bytes are identical across the knob cannot have switched from
-- MPI_BYTE to MPI_FLOAT. But if the OFF arm shows tag 8, GROMACS sent a
-- coordinate buffer down a path the environment variable had disabled, or the
-- capture attributed a tag to the wrong message. There is no benign reading of
-- that one.
--
-- ONE SPECIFICATION, TWO RUNS. The arm is the parameter `gpudirect`, so this
-- single file is the assertion for both arms rather than two queries nobody can
-- diff. That is what the variant mechanism in round 1's asserts.sh is for.
SELECT 'the direct-GPU-comm arm sent no PP->PME coordinates on the GPU tag' AS problem,
       (SELECT gpudirect FROM params)         AS gpudirect,
       (SELECT gmx_tag_coord_gpu FROM params) AS expected_mpi_tag,
       (SELECT count(*) FROM ucx_tag_send)    AS sends_in_capture
WHERE (SELECT gpudirect FROM params) = 1
  AND NOT EXISTS (
    SELECT 1 FROM ucx_tag_send
    WHERE (CAST(tag AS BIGINT) >> (SELECT ompi_tag_shift FROM params))
          = (SELECT gmx_tag_coord_gpu FROM params))

UNION ALL

SELECT 'the host-staged arm sent PP->PME coordinates on the GPU tag',
       (SELECT gpudirect FROM params),
       (SELECT gmx_tag_coord_gpu FROM params),
       (SELECT count(*) FROM ucx_tag_send
        WHERE (CAST(tag AS BIGINT) >> (SELECT ompi_tag_shift FROM params))
              = (SELECT gmx_tag_coord_gpu FROM params))
WHERE (SELECT gpudirect FROM params) = 0
  AND EXISTS (
    SELECT 1 FROM ucx_tag_send
    WHERE (CAST(tag AS BIGINT) >> (SELECT ompi_tag_shift FROM params))
          = (SELECT gmx_tag_coord_gpu FROM params))

UNION ALL

-- Both arms must show the host coordinate tag OR the GPU one. Neither means the
-- PP->PME exchange is not in the capture at all, which would invalidate every
-- volume claim made about it -- including round 1's headline, which this run
-- does not repeat but does rely on.
SELECT 'neither PP->PME coordinate tag appears in the capture',
       (SELECT gpudirect FROM params),
       (SELECT gmx_tag_coord FROM params),
       (SELECT count(*) FROM ucx_tag_send)
WHERE (SELECT count(*) FROM ucx_tag_send) > 0
  AND NOT EXISTS (
    SELECT 1 FROM ucx_tag_send
    WHERE (CAST(tag AS BIGINT) >> (SELECT ompi_tag_shift FROM params))
          IN ((SELECT gmx_tag_coord FROM params),
              (SELECT gmx_tag_coord_gpu FROM params)));
