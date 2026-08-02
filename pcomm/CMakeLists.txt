add_executable(pcomm_launcher main.cpp)
set_target_properties(pcomm_launcher PROPERTIES OUTPUT_NAME "pcomm")
# Links the engine so it can report its own path (pcomm_engine_so_path) for the
# LD_PRELOAD chain — the launcher never records anything itself.
target_link_libraries(pcomm_launcher PRIVATE pcomm::pcomm pcomm::engine)

# Installed at <prefix>/bin, with the engine one hop away at ../lib/pcomm.
# BUILD_RPATH is untouched, so the build tree keeps working exactly as before
# (CMAKE_BUILD_WITH_INSTALL_RPATH is OFF: INSTALL_RPATH applies only to the
# copy `cmake --install` makes).
set_target_properties(pcomm_launcher PROPERTIES
  INSTALL_RPATH "$ORIGIN/../${PCOMM_INSTALL_LIBDIR};${PCOMM_INSTALL_RPATH_EXTRA}")
target_link_options(pcomm_launcher PRIVATE -Wl,--enable-new-dtags)
install(TARGETS pcomm_launcher RUNTIME DESTINATION bin)
