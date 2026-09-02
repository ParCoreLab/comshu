-- requires: uct_binding params.ranks
-- The UCT interposition must have been ACCEPTED, on every rank.
--
-- WHY THIS IS A FILE OF ITS OWN, SEPARATE FROM both_layers_reached_every_rank.
-- If the binding is refused, `uct_send` is not merely empty -- the Parquet
-- exporter omits a raw table with no rows entirely -- so every assertion that
-- names it reports `[skip]`, and a run in which the whole UCT layer failed to
-- engage looks in the summary exactly like a run in which the fabric was quiet.
-- This assertion requires only `uct_binding`, which is written whether the patch
-- took or not, so it turns that silence into a red row.
--
-- IT IS NOT HYPOTHETICAL. Job 45329318 measured
--
--     ticks, compiled_version, runtime_version, ops_size, accepted, forced
--     ...,   1.15.0,           1.20.0,          400,      0,        0
--
-- on all sixteen ranks: the provider had been compiled against UCX 1.15.0
-- headers while LAMMPS ran on the system UCX 1.20.0, the uct_iface_ops_t layout
-- guard refused, and transport_mix came out empty on a run whose UCP layer had
-- captured 13,589 sends. The build was at fault, not the tool -- glogin's login
-- profile auto-loads UCX/1.15.0, which exports CPLUS_INCLUDE_PATH, and gcc drops
-- a `-I` that names a standard system directory, so `-DUCX_INCLUDE_DIR=/usr/include`
-- silently lost to the module. The record said so plainly and is the only reason
-- that was diagnosable at all; this file is the assertion that makes it
-- impossible to miss next time.
--
-- CLAUSE 3 IS THE ANTI-VACUITY GUARD: no uct_binding rows at all means the
-- ucx_deep provider never loaded, which is a different failure from a refused
-- binding and must not pass.
SELECT 'the UCT binding was refused on some rank' AS problem,
       rank, compiled_version, runtime_version, ops_size, accepted
FROM uct_binding
WHERE CAST(accepted AS INTEGER) <> 1

UNION ALL

-- A FORCED binding is not an ordinary pass. PCOMM_UCX_DEEP_FORCE overrides the
-- layout guard, and a capture taken that way is asserting against a struct
-- layout nobody checked. job-r2.sbatch unsets the variable; this says so.
SELECT 'the UCT binding was forced rather than accepted',
       rank, compiled_version, runtime_version, ops_size, accepted
FROM uct_binding
WHERE CAST(forced AS INTEGER) <> 0

UNION ALL

SELECT 'not every rank recorded a uct_binding',
       (SELECT count(DISTINCT rank) FROM uct_binding), '-', '-',
       (SELECT ranks FROM params), -1
WHERE (SELECT count(DISTINCT rank) FROM uct_binding) <> (SELECT ranks FROM params);
