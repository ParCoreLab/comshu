-- requires: transport_mix
-- pcomm's own `scope` column must separate the transports that leave the node
-- from the ones that do not.
--
-- WRITTEN WITHOUT KNOWING WHAT `scope` SAYS, and that is deliberate rather than
-- a limitation. The vocabulary of `scope` is not published: pcomm.fbs declares
-- no enum for it, the derived-table contract in BRIEF-BLACKBOX.md gives
-- transport_mix no column list at all, and `pcomm types` names records rather
-- than derived columns. So an assertion of the form `scope = 'inter'` would be
-- guessing at a string, and would pass vacuously the moment the guess was wrong
-- -- which is the exact hazard that makes an unpublished vocabulary expensive.
--
-- The claim can be made without the vocabulary. Whatever the labels are, a
-- column that DISCRIMINATES cannot give the same label to rc_mlx5 and to sysv.
-- So: group by scope and fail any group that contains both an InfiniBand
-- transport and a shared-memory one. That is falsifiable, needs no string
-- constant of pcomm's, and catches the real defect -- a scope that is constant,
-- or that maps a CUDA IPC interface to the fabric because its device name is
-- not `mlx5_*`.
--
-- cuda_ipc IS THE INTERESTING CASE and the reason this is not a formality.
-- Intra-node GPU-to-GPU traffic under UCX rides cuda_ipc, whose device name is
-- neither an HCA nor a memory device. A scope derived from the device name
-- rather than the transport name gets sysv right and cuda_ipc wrong, and with
-- direct GPU communication on -- the default in GROMACS 2025 on a CUDA
-- library-MPI build -- cuda_ipc is where most of this run's intra-node traffic
-- goes. So this is the arm in which the defect would actually bite, and the
-- direct-off arm is its control: same decomposition, same edges, host
-- transports only.
--
-- The second clause is the degenerate case the first cannot see: if `scope`
-- takes only one value across the whole capture it discriminates nothing, and
-- with both transport families present that is a defect rather than a
-- coincidence.
SELECT 'one scope value covers both an InfiniBand transport and a shared-memory one' AS problem,
       scope,
       count(*) AS rows_in_scope,
       sum(CASE WHEN tl_name IN ('rc_mlx5','rc_verbs','dc_mlx5','ud_mlx5','ud_verbs')
                THEN 1 ELSE 0 END) AS fabric_rows,
       sum(CASE WHEN tl_name IN ('self','sysv','posix','cma','knem','xpmem',
                                 'cuda_ipc','cuda_copy','gdr_copy')
                THEN 1 ELSE 0 END) AS intra_rows
FROM transport_mix
GROUP BY scope
HAVING sum(CASE WHEN tl_name IN ('rc_mlx5','rc_verbs','dc_mlx5','ud_mlx5','ud_verbs')
                THEN 1 ELSE 0 END) > 0
   AND sum(CASE WHEN tl_name IN ('self','sysv','posix','cma','knem','xpmem',
                                 'cuda_ipc','cuda_copy','gdr_copy')
                THEN 1 ELSE 0 END) > 0

UNION ALL

SELECT 'scope takes a single value though both transport families are present',
       (SELECT min(CAST(scope AS VARCHAR)) FROM transport_mix),
       (SELECT count(*) FROM transport_mix),
       (SELECT count(*) FROM transport_mix
        WHERE tl_name IN ('rc_mlx5','rc_verbs','dc_mlx5','ud_mlx5','ud_verbs')),
       (SELECT count(*) FROM transport_mix
        WHERE tl_name IN ('self','sysv','posix','cma','knem','xpmem',
                          'cuda_ipc','cuda_copy','gdr_copy'))
WHERE (SELECT count(DISTINCT CAST(scope AS VARCHAR)) FROM transport_mix) = 1
  AND EXISTS (SELECT 1 FROM transport_mix
              WHERE tl_name IN ('rc_mlx5','rc_verbs','dc_mlx5','ud_mlx5','ud_verbs'))
  AND EXISTS (SELECT 1 FROM transport_mix
              WHERE tl_name IN ('self','sysv','posix','cma','knem','xpmem',
                                'cuda_ipc','cuda_copy','gdr_copy'));
