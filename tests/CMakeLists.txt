add_executable(core_smoke core_smoke.cpp)
target_link_libraries(core_smoke PRIVATE pcomm::pcomm)
add_test(NAME core_smoke COMMAND core_smoke)
