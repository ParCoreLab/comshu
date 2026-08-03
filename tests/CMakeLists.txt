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

# The intern dictionary: written through Tracer (so, the engine), read back
# through rec::InternDictionary — the same path pcomm-read uses.
add_executable(intern_format intern_format.cpp)
target_link_libraries(intern_format PRIVATE pcomm::pcomm pcomm::engine)
pcomm_add_test(intern_format)
add_test(NAME intern_format COMMAND intern_format)

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
