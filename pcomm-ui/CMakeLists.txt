# pcomm-ui — only ever built with DuckDB, since serving a capture is querying it.
# Guarded by the caller (see the root CMakeLists), so this file may assume both
# PCOMM_DUCKDB and the fetched libraries.

set(_duckdb_dir  "${CMAKE_SOURCE_DIR}/.tools/duckdb")
set(_httplib_dir "${CMAKE_SOURCE_DIR}/.tools/httplib")

if(NOT EXISTS "${_httplib_dir}/httplib.h")
  message(FATAL_ERROR
    "pcomm-ui needs cpp-httplib at ${_httplib_dir}.\n"
    "  run tools/get_httplib.sh (needs network — not available on MN5)")
endif()

# The built frontend, embedded byte for byte so the binary is self-contained: a
# UI that needs a sibling directory is a UI that serves a blank page the first
# time somebody scp's it to a login node. Regenerated whenever ui/dist changes;
# never committed.
#
# ui/dist is the REAL app (ui/, Svelte) and is now the only page this server
# knows. It used to embed a hand-written pcomm-ui/index.html — the step-3
# matrix prototype — while the nine-panel app was reachable only by building it
# and passing --dir. That made the default invocation show the weakest thing the
# project had, so the prototype is gone and --dir is now purely a dev override.
set(_ui_dist "${CMAKE_SOURCE_DIR}/ui/dist")
if(NOT EXISTS "${_ui_dist}/index.html")
  message(FATAL_ERROR
    "pcomm-ui embeds the built frontend, and there is none at ${_ui_dist}.\n"
    "  build it:  pnpm -C ui install && pnpm -C ui build\n"
    "  (needs network for the first install — like get_duckdb.sh, not "
    "available on MN5)")
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
# CONFIGURE_DEPENDS so a rebuilt bundle (Vite renames its hashed assets on every
# content change) re-runs cmake rather than embedding the previous file list.
file(GLOB_RECURSE _ui_dist_files CONFIGURE_DEPENDS "${_ui_dist}/*")
set(_assets "${CMAKE_CURRENT_BINARY_DIR}/ui_assets.hpp")
add_custom_command(
  OUTPUT  "${_assets}"
  COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/tools/embed_assets.py"
          "${_ui_dist}" "${_assets}"
  DEPENDS "${CMAKE_SOURCE_DIR}/tools/embed_assets.py" ${_ui_dist_files}
  COMMENT "embedding ui/dist into pcomm-ui"
  VERBATIM)

add_executable(pcomm_ui main.cpp "${_assets}")
set_target_properties(pcomm_ui PROPERTIES OUTPUT_NAME "pcomm-ui")
target_include_directories(pcomm_ui PRIVATE
  "${_duckdb_dir}" "${_httplib_dir}" "${CMAKE_CURRENT_BINARY_DIR}")
# The generated raw-column tables and the shared mount policy, from pcomm-read.
target_link_libraries(pcomm_ui PRIVATE pcomm_duck)
add_dependencies(pcomm_ui pcomm_duck_gen)
target_link_libraries(pcomm_ui PRIVATE "${_duckdb_dir}/libduckdb.so")
find_package(Threads REQUIRED)
target_link_libraries(pcomm_ui PRIVATE Threads::Threads)
set_property(TARGET pcomm_ui APPEND PROPERTY BUILD_RPATH "${_duckdb_dir}")

if(PCOMM_INSTALL_CORE)
  install(TARGETS pcomm_ui RUNTIME DESTINATION bin COMPONENT core)
endif()
