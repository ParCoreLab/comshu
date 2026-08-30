-- requires: tag_recv_resolved ucx_tag_probe
-- A resolved delivered length must have a probe behind it.
--
-- tag_recv_resolved is described as "the tag and true delivered length of a
-- receive that arrived by message handle". The only record in the UCX family
-- that carries a true delivered length is UcxTagProbe -- "`length` is the one
-- true byte count in the UCX family" -- and it is written only by a probe that
-- FOUND a message. So a `delivered` value is a number that has to come from
-- somewhere, and there is exactly one somewhere.
--
-- WHAT THIS APPLICATION CONTRIBUTES, and why it is a good instrument for the
-- question. GROMACS calls neither MPI_Mprobe nor MPI_Improbe -- its halo and
-- PP<->PME exchanges are plain MPI_Isend/MPI_Irecv against a known source and a
-- known size -- so Open MPI never reaches ucp_tag_probe_nb and this capture is
-- predicted to hold no probes. If ucx_tag_probe turns out to be present and
-- non-empty anyway, that is itself worth knowing: something below MPI is
-- probing, and it is not the application.
--
-- The clause fires on a row whose delivered length names a message id that no
-- probe ever reported. That is the shape of defect case 22's
-- every_rma_key_resolves.sql was written for on the one-sided side: an event
-- referencing an entity nothing created. Here it would mean a length resolved
-- against a recycled handle -- plausible, wrong, and invisible without this.
SELECT 'a delivered length names a message no probe reported' AS problem,
       t.rank, t.worker, t.message, t.status, t.offered, t.delivered
FROM tag_recv_resolved t
WHERE t.delivered IS NOT NULL
  AND NOT EXISTS (SELECT 1 FROM ucx_tag_probe p
                  WHERE p.rank = t.rank AND p.message = t.message);
