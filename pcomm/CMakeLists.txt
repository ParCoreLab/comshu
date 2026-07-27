add_executable(pcomm_launcher main.cpp)
set_target_properties(pcomm_launcher PROPERTIES OUTPUT_NAME "pcomm")
# Links the engine so it can report its own path (pcomm_engine_so_path) for the
# LD_PRELOAD chain — the launcher never records anything itself.
target_link_libraries(pcomm_launcher PRIVATE pcomm::pcomm pcomm::engine)
