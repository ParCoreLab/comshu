# These link pcomm::engine, not just pcomm::pcomm. Since 4dd6a69 the recording
# path goes through the C ABI in engine_abi.h and the singletons (Tracer,
# ThreadBufferRegistry, the arenas) live in the shared engine — a test that
# links only the header-only interface target fails at link time, which is how
# all of these rotted unnoticed while every build that ran passed
# -DPCOMM_BUILD_TESTS=OFF. tools/build_all.sh exists so that cannot recur.

add_executable(core_smoke core_smoke.cpp)
target_link_libraries(core_smoke PRIVATE pcomm::pcomm pcomm::engine)
add_test(NAME core_smoke COMMAND core_smoke)

add_executable(hook_smoke hook_smoke.cpp)
target_link_libraries(hook_smoke PRIVATE pcomm::pcomm pcomm::hook)
add_test(NAME hook_smoke COMMAND hook_smoke)

find_package(Threads REQUIRED)
add_executable(thread_flush_smoke thread_flush_smoke.cpp)
target_link_libraries(thread_flush_smoke PRIVATE pcomm::pcomm pcomm::engine Threads::Threads)
add_test(NAME thread_flush_smoke COMMAND thread_flush_smoke)

add_executable(fork_smoke fork_smoke.cpp)
target_link_libraries(fork_smoke PRIVATE pcomm::pcomm pcomm::engine Threads::Threads)
add_test(NAME fork_smoke COMMAND fork_smoke)

# The intern dictionary: written through Tracer (so, the engine), read back
# through rec::InternDictionary — the same path pcomm-read uses.
add_executable(intern_format intern_format.cpp)
target_link_libraries(intern_format PRIVATE pcomm::pcomm pcomm::engine)
add_test(NAME intern_format COMMAND intern_format)
