# These link pcomm::engine, not just pcomm::pcomm. Since 4dd6a69 the recording
# path goes through the C ABI in engine_abi.h and the singletons (Tracer,
# ThreadBufferRegistry, the arenas) live in the shared engine — a test that
# links only the header-only interface target fails at link time, which is how
# all of these rotted unnoticed while every build that ran passed
# -DPCOMM_BUILD_TESTS=OFF. tools/build_all.sh exists so that cannot recur.

# ── Tests must keep their assertions ────────────────────────────────────────
# Every test here checks with assert(), and every build that runs them uses a
# CMAKE_BUILD_TYPE that defines NDEBUG (tools/build_all.sh uses RelWithDebInfo;
# the README's Release does the same). That compiles assert() to nothing, so 44
# runtime assertions across five files were no-ops: the suite verified that
# nothing segfaults and *nothing else*, while reporting "tests ok".
#
# Demonstrated rather than assumed: reverting attr/pipeline.hpp to the bug that
# plans/PLAN-20260803-1021-gpumem-window-timestamps-*.md exists to fix left
# core_smoke passing. With this, it fails.
#
# -UNDEBUG on the test targets only — the library and providers keep NDEBUG, so
# the asserts on the capture hot path stay compiled out, which is what we want.
function(pcomm_add_test target)
  target_compile_options(${target} PRIVATE -UNDEBUG)
endfunction()

add_executable(core_smoke core_smoke.cpp)
target_link_libraries(core_smoke PRIVATE pcomm::pcomm pcomm::engine)
pcomm_add_test(core_smoke)
add_test(NAME core_smoke COMMAND core_smoke)

add_executable(hook_smoke hook_smoke.cpp)
target_link_libraries(hook_smoke PRIVATE pcomm::pcomm pcomm::hook)
pcomm_add_test(hook_smoke)
add_test(NAME hook_smoke COMMAND hook_smoke)

find_package(Threads REQUIRED)
add_executable(thread_flush_smoke thread_flush_smoke.cpp)
target_link_libraries(thread_flush_smoke PRIVATE pcomm::pcomm pcomm::engine Threads::Threads)
pcomm_add_test(thread_flush_smoke)
add_test(NAME thread_flush_smoke COMMAND thread_flush_smoke)

add_executable(fork_smoke fork_smoke.cpp)
target_link_libraries(fork_smoke PRIVATE pcomm::pcomm pcomm::engine Threads::Threads)
pcomm_add_test(fork_smoke)
add_test(NAME fork_smoke COMMAND fork_smoke)

# A killed process must still leave a capture that can be placed on a node and a
# clock. Nothing flushes on an abnormal exit, and the failure is inverted -- the
# high-volume streams survive and the one-row identity streams do not -- so
# start() flushes those two immediately. This is what stops that line being read
# as a redundant flush and deleted.
add_executable(crash_identity_smoke crash_identity_smoke.cpp)
target_link_libraries(crash_identity_smoke PRIVATE pcomm::pcomm pcomm::engine)
pcomm_add_test(crash_identity_smoke)
add_test(NAME crash_identity COMMAND crash_identity_smoke)

# The intern dictionary: written through Tracer (so, the engine), read back
# through rec::InternDictionary — the same path the reader uses.
add_executable(intern_format intern_format.cpp)
target_link_libraries(intern_format PRIVATE pcomm::pcomm pcomm::engine)
pcomm_add_test(intern_format)
add_test(NAME intern_format COMMAND intern_format)

# The intern hot path. Providers intern once per RECORD, not once per entity, so
# this is one of the few places in the tree where a constant factor is the whole
# story: ucx_deep measured 86 ns of a 138 ns operation inside rec().intern().
# Correctness first (a wrong handle points a record at the wrong string and
# leaves the capture well-formed), and the cost printed rather than asserted —
# a threshold on whatever machine builds the tree would be a flaky test.
add_executable(intern_hot_path intern_hot_path.cpp)
target_link_libraries(intern_hot_path PRIVATE pcomm::pcomm pcomm::engine Threads::Threads)
pcomm_add_test(intern_hot_path)
add_test(NAME intern_hot_path COMMAND intern_hot_path)

# The new-WR-API state machine. providers/ibverbs needs libibverbs to build, but
# wr_builder.hpp deliberately holds no vendor type, so the sequencing logic is
# testable on any machine — which is where its bugs would be.
add_executable(wr_builder_test wr_builder_test.cpp)
target_include_directories(wr_builder_test PRIVATE ${CMAKE_SOURCE_DIR}/providers/ibverbs)
target_link_libraries(wr_builder_test PRIVATE pcomm::pcomm)
pcomm_add_test(wr_builder_test)
add_test(NAME wr_builder COMMAND wr_builder_test)

# InternTable itself: identity, handle numbering, storage stability, and the
# shared string/key counter. No engine, no NIC — the paths are all reachable
# from the header.
add_executable(intern_table_test intern_table_test.cpp)
target_link_libraries(intern_table_test PRIVATE pcomm::pcomm)
pcomm_add_test(intern_table_test)
add_test(NAME intern_table COMMAND intern_table_test)

# The sink's failure paths. Every interesting case here is a case where the
# filesystem refuses, which is exactly the class this project had no test for
# until a filesystem quota on MN5 produced captures missing whole streams and
# three suite cases went red pointing at the wrong layer entirely.
add_executable(sink_write_errors sink_write_errors.cpp)
target_link_libraries(sink_write_errors PRIVATE pcomm::pcomm)
pcomm_add_test(sink_write_errors)
add_test(NAME sink_write_errors COMMAND sink_write_errors)

# NCCL's own commHash, which is the key the profiler-plugin rows join on. NOT
# gated on PCOMM_PROVIDER_NCCL and deliberately so: the header pulls in no NCCL
# and no CUDA, so this runs in every configuration, including the ones where the
# provider is switched off. A join key that silently stops matching is the kind
# of defect that only shows up as an empty table three machines later.
add_executable(nccl_comm_hash_test nccl_comm_hash_test.cpp)
target_include_directories(nccl_comm_hash_test PRIVATE ${CMAKE_SOURCE_DIR}/providers/nccl)
pcomm_add_test(nccl_comm_hash_test)
add_test(NAME nccl_comm_hash COMMAND nccl_comm_hash_test)

# The MN5 suite's cross-cutting assertions, run against synthetic captures that
# contain a known defect. Every other test here checks the tool; this one checks
# the *instruments* — an assertion that has been relaxed into silence looks
# exactly like one that passes, and hpc/suite/asserts_fire.sh is where a
# relaxation has to prove it still fires. No NIC and no capture needed: it builds
# its own tables. Skips loudly (exit 0) when sqlite3 is absent, the same bargain
# schema_generated_current makes with flatc.
add_test(NAME asserts_fire
         COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/hpc/suite/asserts_fire.sh)

# The one rule this repository states twice: send->completion pairing, once in
# C++ for the pftrace exporter (which must run without DuckDB) and once in SQL
# for the derived layer (which must run over a Parquet export). Neither can
# adopt the other, so this asserts they agree on a fixture instead. The script
# skips loudly when the binary has no DuckDB, so it is registered unconditionally
# rather than behind PCOMM_DUCKDB — a test that silently vanishes from half the
# builds is how the assertion rot in the comment at the top of this file started.
add_test(NAME flow_pairing_agree
         COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/flow_pairing_agree.sh
                 $<TARGET_FILE:pcomm_launcher>
                 ${CMAKE_CURRENT_SOURCE_DIR}/data/ib_pingpong-44578115)

# And the rule itself, on a capture built so that every clause has something to
# get wrong. flow_pairing_agree.sh above compares the two implementations, which
# catches divergence and — demonstrated, not assumed — cannot catch a rule that
# is wrong in BOTH, nor one whose fixture never expresses the defect.
add_test(NAME wr_flow_rule
         COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/wr_flow_rule.sh $<TARGET_FILE:pcomm_launcher>)

# The two derived tables the UI actually opens with, on the same pattern and for
# the reason the pattern exists. Both were verifiable only through tests/hpc
# until now — that is, only by booking a GPU cluster — which put the weakest
# regression net under the most-read output. Neither needs a NIC, a GPU or a
# capture: they build their own tables, like wr_flow_rule.sh above.
#
# Both fixtures are two-sided on every key that is a PAIR: qp_num 5 exists on
# two HCAs, device 1 exists on two nodes, and one symmetric buffer has a
# different intern id on every rank. A fixture where the second half of each key
# is unique cannot tell a correct join from one that dropped it.
#
# Every clause each asserts was checked by DELETING it and watching the test go
# red. That found a hole the first time round: removing the remote_addr > 0
# guard left object_access_rule.sh green, because no object in the fixture
# contained address 0, so the range join dropped the row for the right answer by
# the wrong reason. The fixture carries an object at base 0 now.
add_test(NAME comm_edges_rule
         COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/comm_edges_rule.sh $<TARGET_FILE:pcomm_launcher>)

add_test(NAME object_access_rule
         COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/object_access_rule.sh $<TARGET_FILE:pcomm_launcher>)

# Transport attribution — the two joins the derived layer gained on 2026-08-27,
# an intern handle to its string and a uct_send to its interface. It also covers
# the op families NO HARDWARE CAPTURE HAS PRODUCED: MN5 job 45113099 recorded
# 295 UCT operations and every one was an active message, so the get / rendezvous
# split is asserted here or nowhere.
add_test(NAME transport_mix_rule
         COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/transport_mix_rule.sh $<TARGET_FILE:pcomm_launcher>)

# The only join between what the application asked UCX for and what UCX put on
# the wire. It rests on one measured fact — uct_send.tag carries the
# application's own tag, MN5 job 45115011 — so the rule is worth pinning: a FULL
# OUTER join whose two absences are different facts, and a UCT side selected by
# DOOR rather than by `tag <> 0`, since tag zero is a real tag.
add_test(NAME tag_flow_rule
         COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tag_flow_rule.sh $<TARGET_FILE:pcomm_launcher>)

# The probe -> msg_recv join, whose rule is TEMPORAL because UCX recycles the
# message handle out of its request mpool. A fixture rather than a capture for
# exactly that reason: handle reuse is what breaks the join, no hardware run
# reproduces it on demand, and an equality join returns confident wrong pairs
# rather than an error.
add_test(NAME tag_recv_rule
         COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tag_recv_rule.sh $<TARGET_FILE:pcomm_launcher>)

# The invocation record's handle join. A fixture rather than a capture because
# the two clauses worth testing — handles being scoped to (rank, domain), and a
# handle claimed by two strings resolving to NULL instead of to a guess — only
# appear on a multi-rank or a REPLAYED capture, and neither is something a
# single local run produces.
add_test(NAME run_provenance_rule
         COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/run_provenance_rule.sh $<TARGET_FILE:pcomm_launcher>)

# The NCCL profiler layer's two joins. A fixture and not a capture because the
# cases that break them do not occur on the hardware we can run: a communicator
# whose commHash is underivable needs ncclCommSplit, and the event-id rank-scope
# clause needs two ranks whose per-process counters collide — which they always
# do, but only a fixture can pin them to the exact same numbers and check that
# the traffic did not cross over. It also drives link_health's three new rows to
# non-zero, since a rubric row nobody has seen fire is a row that reads as clean
# on the day it is right.
add_test(NAME nccl_execution_rule
         COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/nccl_execution_rule.sh $<TARGET_FILE:pcomm_launcher>)

# link_health's capture-completeness rows, over a fixture rather than a real
# capture for the same reason as above: the interesting states are a rank that
# was killed mid-run and a stream file that disappeared after the run, and
# neither can be arranged on demand by running a workload. It also holds the
# no-false-positive case, which is the one that decides whether anyone keeps
# reading the rubric at all.
add_test(NAME capture_integrity_rule
         COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/capture_integrity_rule.sh $<TARGET_FILE:pcomm_launcher>)

# What `pcomm sql` accepts as a query. Not about SQL at all — about the
# `COPY (<q>) TO` wrapper, which turns a trailing semicolon and a trailing
# comment into parse errors that name neither.
add_test(NAME sql_cli_rule
         COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/sql_cli_rule.sh $<TARGET_FILE:pcomm_launcher>)

# ucx_deep's hot-path lookup tables, driven directly.
#
# UNCONDITIONAL, unlike everything else UCX in this file, and deliberately so:
# ucx_deep_tables.hpp includes no UCX header and no pcomm header, so this runs
# in the core configuration that tools/build_all.sh actually builds. The UCX
# configuration is not in build_all.sh (PLAN-20260815-1540, Future work 5), so a
# test that needed it would run for nobody.
add_executable(ucx_deep_tables ucx_deep_tables.cpp)
target_include_directories(ucx_deep_tables PRIVATE ${CMAKE_SOURCE_DIR}/providers/ucx_deep)
target_compile_features(ucx_deep_tables PRIVATE cxx_std_20)
target_compile_options(ucx_deep_tables PRIVATE -UNDEBUG)
add_test(NAME ucx_deep_tables COMMAND ucx_deep_tables)

# The UCX provider's record gate. Registered only when that provider is built,
# because it needs both the .so and UCX itself — but it does NOT need a cluster:
# the self transport runs the whole workload in one process, which is why this
# exists at all. Everything the provider recorded was previously verified by
# reading MN5 job output by hand.
#
# The workload links against UCX directly rather than going through the provider,
# so its doors are the ones an application really calls.
if(PCOMM_PROVIDER_UCX)
  # libucs as well as libucp: the workload reports failures with
  # ucs_status_string, and unlike the provider it links UCX directly rather than
  # interposing it, so the transitive dependency has to be named.
  find_library(UCX_UCS_LIB NAMES ucs HINTS ${UCX_UCP_LIB} PATH_SUFFIXES lib lib64)
  add_executable(ucx_selftest ucx_selftest.cpp)
  target_include_directories(ucx_selftest PRIVATE ${UCX_INCLUDE_DIR})
  target_link_libraries(ucx_selftest PRIVATE ${UCX_UCP_LIB} ${UCX_UCS_LIB})
  add_test(NAME ucx_records
           COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/ucx_records.sh
                   $<TARGET_FILE:pcomm_launcher>
                   $<TARGET_FILE:ucx_selftest>
                   $<TARGET_FILE:pcomm_engine>
                   $<TARGET_FILE:ucx_trace>)
  # libucp pulls in libuct and libucm, and RUNPATH — which is what the linker
  # emits by default — is NOT inherited by a dependency's dependencies. So the
  # binary finds libucp through its own runpath and then libucp fails to find
  # libuct. Naming the directory once here is clearer than over-linking the test
  # against transitive libraries it never calls.
  # The UCT-layer gate, on the SAME workload, with both UCX providers loaded.
  # Same tree, same switch, same one command: ucx_deep is not a configuration of
  # its own, precisely so it cannot become a configuration no gate builds.
  add_test(NAME ucx_deep_records
           COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/ucx_deep_records.sh
                   $<TARGET_FILE:pcomm_launcher>
                   $<TARGET_FILE:ucx_selftest>
                   $<TARGET_FILE:pcomm_engine>
                   $<TARGET_FILE:ucx_trace>
                   $<TARGET_FILE:ucx_deep_trace>)
  get_filename_component(_ucx_libdir "${UCX_UCP_LIB}" DIRECTORY)
  set_tests_properties(ucx_records ucx_deep_records PROPERTIES
    ENVIRONMENT "LD_LIBRARY_PATH=${_ucx_libdir}:$ENV{LD_LIBRARY_PATH}")
endif()
