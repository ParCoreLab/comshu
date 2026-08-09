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

# The page, embedded as a C++ raw string so the binary is self-contained: a UI
# that needs a sibling file is a UI that breaks when it is copied to a login
# node. Regenerated whenever index.html changes; never committed.
set(_assets "${CMAKE_CURRENT_BINARY_DIR}/ui_assets.hpp")
add_custom_command(
  OUTPUT  "${_assets}"
  COMMAND ${CMAKE_COMMAND}
          -DIN=${CMAKE_CURRENT_SOURCE_DIR}/index.html
          -DOUT=${_assets}
          -P ${CMAKE_CURRENT_SOURCE_DIR}/embed.cmake
  DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/index.html"
          "${CMAKE_CURRENT_SOURCE_DIR}/embed.cmake"
  COMMENT "embedding pcomm-ui/index.html"
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
