-- requires: ucx_context ib_mem_region
-- Two hook-based providers, one process, one pass.
--
-- `comshu run -p ucx,ib` is required by PLAN-20260815-1540's definition of done
-- and had NEVER BEEN EXECUTED: the smoke job that ran used `-p ucx` with
-- COMSHU_PROVIDER_IBVERBS=OFF. Co-running two non-exclusive providers is the
-- arrangement the shared engine exists for — one SmartDlsym, one recorder, two
-- record ranges — and the failure it guards against is not subtle. Naive dual
-- preloading of two providers that each claim dlsym at constructor time
-- SEGFAULTS before main(); that is the finding in things-to-circle-back-to.md
-- that produced the exclusive/non-exclusive distinction in the first place.
--
-- BOTH HALVES, because either alone proves nothing. A capture with only UCX
-- rows is what a silently-not-engaged verbs provider looks like, and a capture
-- with only verbs rows is what it looks like when UCX loses its interposition to
-- a library-ordering change. The pair is the claim.
--
-- ib_mem_region and not ib_post_send is the right verbs witness on this stack,
-- and the reason IS the UCX story: UCX registers memory through ibverbs — which
-- is why the control plane fills — and posts through nothing exported, so
-- ib_post_send is EXPECTED to be empty here. Asserting on it would fail a
-- correct capture.
SELECT 'the ucx provider recorded nothing in a -p ucx,ib pass' AS problem
WHERE NOT EXISTS (SELECT 1 FROM ucx_context)
UNION ALL
SELECT 'the ibverbs provider recorded nothing in a -p ucx,ib pass' AS problem
WHERE NOT EXISTS (SELECT 1 FROM ib_mem_region);
