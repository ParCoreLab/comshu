# pcomm — the one binary. `main.cpp` is the verb dispatcher, `run.cpp` the
# launcher; the reading half and the UI arrive as OBJECT libraries from their own
# directories (added before this one in the root CMakeLists, so both targets
# exist by the time they are named here).
add_executable(pcomm_launcher main.cpp run.cpp)
set_target_properties(pcomm_launcher PROPERTIES OUTPUT_NAME "pcomm")
target_link_libraries(pcomm_launcher PRIVATE pcomm::pcomm pcomm_read)

# The engine is NOT linked. It exports `dlsym` — that is the point of it — so a
# DT_NEEDED here would put the interposer in the global symbol scope of every
# invocation, `pcomm read` and `pcomm ui` included, and `ui` links a DuckDB that
# dlopens. run.cpp dlopens it RTLD_LOCAL when a run actually needs its path, so
# the soname still has to be findable: that is what these rpaths are for, and
# the build tree needs the entry that linking used to supply for free.
set_property(TARGET pcomm_launcher APPEND PROPERTY
  BUILD_RPATH "${CMAKE_BINARY_DIR}/libpcomm")

if(PCOMM_DUCKDB)
  target_link_libraries(pcomm_launcher PRIVATE pcomm_ui)
  # libduckdb.so lives outside any install prefix, so an uninstalled build has
  # to carry its location.
  set_property(TARGET pcomm_launcher APPEND PROPERTY
    BUILD_RPATH "${CMAKE_SOURCE_DIR}/.tools/duckdb")
endif()

# Installed at <prefix>/bin, with the engine one hop away at ../lib/pcomm.
# BUILD_RPATH is untouched, so the build tree keeps working exactly as before
# (CMAKE_BUILD_WITH_INSTALL_RPATH is OFF: INSTALL_RPATH applies only to the
# copy `cmake --install` makes).
set_target_properties(pcomm_launcher PROPERTIES
  INSTALL_RPATH "$ORIGIN/../${PCOMM_INSTALL_LIBDIR};${PCOMM_INSTALL_RPATH_EXTRA}")
target_link_options(pcomm_launcher PRIVATE -Wl,--enable-new-dtags)
if(PCOMM_INSTALL_CORE)
  install(TARGETS pcomm_launcher RUNTIME DESTINATION bin COMPONENT core)
endif()
