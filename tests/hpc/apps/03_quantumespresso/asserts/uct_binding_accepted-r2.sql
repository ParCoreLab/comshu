-- requires: uct_binding
-- READ THIS ONE FIRST when uct_send is empty. It is 21_ucx_deep's assertion,
-- unchanged, pointed at an OLDER UCX than the provider has ever met.
--
-- ucx_deep reaches UCX's data plane by patching `uct_iface_ops_t`, an installed
-- header with no ABI promise across versions, and guards that by comparing the
-- ops-table size it compiled against with the UCX actually loaded. comshu.fbs
-- type 806 states the consequence: "`accepted = 0` is why uct_send is empty, and
-- saying so is the whole reason this record exists."
--
-- WHY IT IS AT RISK HERE AND NOT IN CASE 21. Case 21 runs under the suite
-- runtime (nvidia-hpc-sdk/25.7, HPC-X 2.22.1) and measured runtime 1.18.0
-- against a 1.19.0 build. THIS case cannot: pw.x is linked against HPC-X 2.16
-- and its UCX 1.15 (round 1's NOTES.md, ldd), and the UCX provider is symbol
-- interposition, so it must sit in front of the libucp that is actually loaded.
-- 1.15 is older than anything ucx_deep has been run against, and there is no way
-- to find out offline whether its ops table is the size the guard wants --
-- `comshu list` documents what a provider captures, not what it requires.
--
-- SO A FAILURE HERE IS A RESULT, NOT AN ACCIDENT. accepted = 0 means the
-- transport layer is blind on exactly the software stack the science
-- applications on this machine load, and the finding is named to the guard and
-- to the two version numbers this row carries. It is claim type 4 of
-- apps/README.md and it is worth more than a green board.
--
-- forced = 1 means COMSHU_UCX_DEEP_FORCE overrode a refusal. The job script does
-- not set it, and a capture whose every row rests on a guard that said no must
-- not be reported as an ordinary pass.
--
-- Names no version and no size: it asserts the provider's own verdict.
SELECT rank,
       compiled_version,
       runtime_version,
       ops_size,
       accepted,
       forced
FROM uct_binding
WHERE CAST(accepted AS INTEGER) <> 1
   OR CAST(forced AS INTEGER) <> 0;
