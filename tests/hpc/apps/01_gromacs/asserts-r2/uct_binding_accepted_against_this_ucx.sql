-- requires: uct_binding
-- The provider must have dared to patch this UCX, and must have done it without
-- being told to.
--
-- READ THIS ONE FIRST IF uct_send IS EMPTY. UctBinding exists to say why, and
-- its own schema comment says so: "accepted = 0 is why uct_send is empty, and
-- saying so is the whole reason this record exists".
--
-- WHY IT IS NOT MERELY A COPY OF CASE 21'S. That case measured UCX 1.18.0
-- against a 1.19.0 build, ops_size 400 on both. This run is against the UCX that
-- ships in HPC-X 2.16 inside NVIDIA HPC SDK 23.11 -- **UCX 1.15**, three minor
-- releases OLDER than anything ucx_deep has ever been run against, and the one
-- the application's own MPI was built for. uct_iface_ops_t is an installed
-- header with no ABI promise across versions and the guard compares table sizes;
-- an older UCX is exactly where that guard is most likely to fire, and if it
-- does, every other assertion in this directory is skipping on an empty table
-- rather than passing.
--
-- Round 1's NOTES.md documents the trap that makes this a live risk rather than
-- a theoretical one: libucx_trace.so carries a RUNPATH to HPC SDK 25.7's
-- hpcx-2.22.1/ucx/lib, and because the provider is LD_PRELOADed it wins the
-- SONAME. Unless LD_LIBRARY_PATH puts hpcx-2.16/ucx/lib first, the provider
-- binds to UCX 1.18 while the application's pml_ucx runs against 1.15 -- two
-- UCXes in one process. The job script prepends the right one and prints
-- `ldd | grep libucp` to prove it; this assertion is what notices if that did
-- not take.
--
-- forced = 1 means COMSHU_UCX_DEEP_FORCE overrode a refusal. A forced capture may
-- be perfectly correct, but it is a capture whose every row rests on a guard
-- that said no, and a study must not report that as an ordinary pass. Nothing in
-- this job sets that variable, so a 1 here would mean it was inherited from the
-- environment.
--
-- NAMES NO VERSION AND NO SIZE: it asserts the provider's own verdict, not a
-- particular UCX, so it stays correct when MN5's modules move.
SELECT rank,
       compiled_version,
       runtime_version,
       ops_size,
       accepted,
       forced
FROM uct_binding
WHERE CAST(accepted AS INTEGER) <> 1
   OR CAST(forced AS INTEGER) <> 0;
