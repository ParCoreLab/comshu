# comshu-ui — only ever built with DuckDB, since serving a capture is querying it.
# Guarded by the caller (see the root CMakeLists), so this file may assume both
# COMSHU_DUCKDB and the fetched libraries.

set(_duckdb_dir  "${CMAKE_SOURCE_DIR}/.tools/duckdb")
set(_httplib_dir "${CMAKE_SOURCE_DIR}/.tools/httplib")

if(NOT EXISTS "${_httplib_dir}/httplib.h")
  message(FATAL_ERROR
    "comshu-ui needs cpp-httplib at ${_httplib_dir}.\n"
    "  run tools/get_httplib.sh (needs network — not available on MN5)")
endif()

# The built frontend, embedded byte for byte so the binary is self-contained: a
# UI that needs a sibling directory is a UI that serves a blank page the first
# time somebody scp's it to a login node.
#
# BUILT here, not committed. ui/dist is a build artifact, and carrying one in
# git meant new blobs on every frontend change, because Vite's hashed names
# never dedupe. tools/build_ui.sh produces it; this file only embeds it.
#
# ui/dist is the REAL app (ui/, Svelte) and is now the only page this server
# knows. It used to embed a hand-written comshu-ui/index.html — the step-3
# matrix prototype — while the nine-panel app was reachable only by building it
# and passing --dir. That made the default invocation show the weakest thing the
# project had, so the prototype is gone and --dir is now purely a dev override.
set(_ui_dist "${CMAKE_SOURCE_DIR}/ui/dist")

find_program(COMSHU_NPM_EXECUTABLE npm)
if(NOT COMSHU_NPM_EXECUTABLE)
  message(FATAL_ERROR
    "comshu-ui builds the frontend and there is no npm on PATH.\n"
    "  install node and npm, or configure with -DCOMSHU_UI=OFF\n"
    "  (the first build fetches the packages, so it needs network — which is "
    "why MN5 builds without the UI)")
endif()

# The SOURCES, not the output. Globbing ui/dist at configure time is what the
# previous version did, and it cannot survive dist being built rather than
# committed: at configure time the directory is empty or absent, so the glob
# returns nothing and the embedder depends on nothing.
file(GLOB_RECURSE _ui_sources CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/ui/src/*")
add_custom_command(
  OUTPUT  "${_ui_dist}/index.html"
  COMMAND "${CMAKE_SOURCE_DIR}/tools/build_ui.sh"
  DEPENDS "${CMAKE_SOURCE_DIR}/tools/build_ui.sh"
          "${CMAKE_SOURCE_DIR}/ui/package.json"
          "${CMAKE_SOURCE_DIR}/ui/package-lock.json"
          "${CMAKE_SOURCE_DIR}/ui/vite.config.js"
          "${CMAKE_SOURCE_DIR}/ui/index.html"
          ${_ui_sources}
  COMMENT "building the comshu frontend (npm)"
  VERBATIM)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(_assets "${CMAKE_CURRENT_BINARY_DIR}/ui_assets.hpp")
add_custom_command(
  OUTPUT  "${_assets}"
  COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/tools/embed_assets.py"
          "${_ui_dist}" "${_assets}"
  # The bundle's own index.html stands in for the whole directory: Vite renames
  # every hashed asset when content changes, so there is no stable file list to
  # depend on, and index.html is rewritten whenever those names change.
  DEPENDS "${CMAKE_SOURCE_DIR}/tools/embed_assets.py" "${_ui_dist}/index.html"
  COMMENT "embedding ui/dist into comshu-ui"
  VERBATIM)

# `comshu ui`, as an OBJECT library linked into the one executable — same reason
# as the reading half: one command, one help text. PUBLIC usage requirements so
# they reach the executable, which is built in another directory.
add_library(comshu_ui OBJECT main.cpp "${_assets}")
target_include_directories(comshu_ui PUBLIC
  "${_duckdb_dir}" "${_httplib_dir}" "${CMAKE_CURRENT_BINARY_DIR}")
# The generated raw-column tables and the shared mount policy, from the reader.
target_link_libraries(comshu_ui PUBLIC comshu_duck)
add_dependencies(comshu_ui comshu_duck_gen)
target_link_libraries(comshu_ui PUBLIC "${_duckdb_dir}/libduckdb.so")
find_package(Threads REQUIRED)
target_link_libraries(comshu_ui PUBLIC Threads::Threads)
# What tells the dispatcher this build has a `ui` verb at all.
target_compile_definitions(comshu_ui PUBLIC COMSHU_HAVE_UI=1)
