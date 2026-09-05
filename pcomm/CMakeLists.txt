# pcomm — the one binary. `main.cpp` is the verb dispatcher, `run.cpp` the
# launcher; the reading half and the UI arrive as OBJECT libraries from their own
# directories (added before this one in the root CMakeLists, so both targets
# exist by the time they are named here).
add_executable(pcomm_launcher main.cpp run.cpp baked.cpp)
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

if(PCOMM_UI)
  target_link_libraries(pcomm_launcher PRIVATE pcomm_ui)
endif()
if(PCOMM_DUCKDB AND NOT PCOMM_DUCKDB_STATIC)
  # libduckdb.so lives outside any install prefix, so an uninstalled build has
  # to carry its location. This is also the reason PCOMM_DUCKDB_STATIC exists:
  # an RPATH into someone's source tree is not a thing you can copy to a cluster.
  set_property(TARGET pcomm_launcher APPEND PROPERTY
    BUILD_RPATH "${CMAKE_SOURCE_DIR}/.tools/duckdb")
endif()

# `pcomm --version` — the build id, from the same generated header the engine
# stamps into every capture (libpcomm/CMakeLists, pcomm_build_id). Two consumers
# of one generator rather than a second source of truth.
#
# It exists because every provenance check in tests/hpc had to go THROUGH a
# capture to learn which commit a binary was: the header is private to the
# engine target and nothing else could ask. That left the preventive layer
# (suite/check_build_freshness.sh) inferring staleness from mtimes when the
# binary knew the answer exactly. See PLAN-20260904-1900.
add_dependencies(pcomm_launcher pcomm_build_id)
target_include_directories(pcomm_launcher PRIVATE "${CMAKE_BINARY_DIR}/libpcomm")

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
