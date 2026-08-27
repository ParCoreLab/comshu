-- requires: uct_binding
-- The provider must have DARED to patch, and must have done it without being
-- told to.
--
-- ucx_deep reaches UCX's data plane through `uct_iface_ops_t`, an installed
-- header with no ABI promise across versions, and it guards that by comparing
-- the table size it compiled against with the UCX actually loaded. accepted = 0
-- is the honest empty capture: uct_send has no rows and this row says why,
-- instead of leaving a reader to conclude the application sent nothing.
--
-- forced = 1 means PCOMM_UCX_DEEP_FORCE overrode a refusal. A forced capture may
-- be perfectly correct, but it is a capture whose every row rests on a guard
-- that said no, and the suite must not report that as an ordinary pass.
--
-- Names no version and no size: it asserts the provider's own verdict, not a
-- particular UCX. Job 45113099 measured 1.18.0 against a 1.19.0 build with
-- ops_size 400 on both, which is the agreement this guard exists to check for
-- itself on every future run rather than have someone remember.
SELECT rank,
       compiled_version,
       runtime_version,
       ops_size,
       accepted,
       forced
FROM uct_binding
WHERE CAST(accepted AS INTEGER) <> 1
   OR CAST(forced AS INTEGER) <> 0;
