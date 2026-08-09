add_executable(pcomm_read main.cpp)
set_target_properties(pcomm_read PROPERTIES OUTPUT_NAME "pcomm-read")
# The reader decodes every provider's record schema; ib_records.hpp is
# vendor-neutral (no verbs.h), so this include adds no ibverbs dependency.
target_include_directories(pcomm_read PRIVATE ${CMAKE_SOURCE_DIR}/providers/ibverbs
                                              ${CMAKE_SOURCE_DIR}/providers/mlx5dv)
target_link_libraries(pcomm_read PRIVATE pcomm::pcomm)

# DuckDB, when asked for. Located rather than assumed: a missing library is a
# configure-time error naming the script that fetches it, not a link failure
# fifty lines into someone else's output.
if(PCOMM_DUCKDB)
  set(_duckdb_dir "${CMAKE_SOURCE_DIR}/.tools/duckdb")
  if(NOT EXISTS "${_duckdb_dir}/duckdb.h" OR NOT EXISTS "${_duckdb_dir}/libduckdb.so")
    message(FATAL_ERROR
      "PCOMM_DUCKDB=ON but no libduckdb at ${_duckdb_dir}.\n"
      "  run tools/get_duckdb.sh (needs network — not available on MN5)")
  endif()
  # Raw column tables, generated from the record declarations at build time.
  # Generated rather than committed so it cannot go stale, and generated only
  # here so the cluster build — which has no python guarantee and no DuckDB —
  # never needs it. Depends on every header that can declare a record, so
  # adding a record regenerates rather than silently omits it.
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  set(_raw_hdr "${CMAKE_CURRENT_BINARY_DIR}/raw_columns.hpp")
  file(GLOB_RECURSE _record_hdrs CONFIGURE_DEPENDS
       "${CMAKE_SOURCE_DIR}/libpcomm/include/pcomm/core/*.hpp"
       "${CMAKE_SOURCE_DIR}/providers/*/*_records.hpp")
  add_custom_command(
    OUTPUT  "${_raw_hdr}"
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/tools/gen_raw_columns.py"
            "${CMAKE_SOURCE_DIR}" "${_raw_hdr}"
    DEPENDS "${CMAKE_SOURCE_DIR}/tools/gen_raw_columns.py"
            "${CMAKE_SOURCE_DIR}/tools/schema_check.py"
            ${_record_hdrs}
    COMMENT "generating raw DuckDB column tables from the record declarations"
    VERBATIM)
  # An INTERFACE target so pcomm-ui consumes the SAME generated header and the
  # SAME mount policy, rather than growing a second copy of either.
  add_library(pcomm_duck INTERFACE)
  target_sources(pcomm_duck INTERFACE "${_raw_hdr}")
  target_include_directories(pcomm_duck INTERFACE
    "${CMAKE_CURRENT_BINARY_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}" "${_duckdb_dir}")
  # The generated header names Stamped<R> and every record type, so it needs
  # libpcomm's headers and the providers' record schemas — carried by the
  # interface rather than re-listed by every consumer.
  target_link_libraries(pcomm_duck INTERFACE pcomm::pcomm)
  target_include_directories(pcomm_duck INTERFACE
    "${CMAKE_SOURCE_DIR}/providers/ibverbs" "${CMAKE_SOURCE_DIR}/providers/mlx5dv")
  add_custom_target(pcomm_duck_gen DEPENDS "${_raw_hdr}")
  add_dependencies(pcomm_duck pcomm_duck_gen)

  target_sources(pcomm_read PRIVATE "${_raw_hdr}")

  target_compile_definitions(pcomm_read PRIVATE PCOMM_HAVE_DUCKDB=1)
  target_include_directories(pcomm_read PRIVATE "${_duckdb_dir}" "${CMAKE_CURRENT_BINARY_DIR}")
  target_link_libraries(pcomm_read PRIVATE "${_duckdb_dir}/libduckdb.so")
  # The .so lives outside any install prefix, so an uninstalled build has to
  # carry its location; BUILD_RPATH keeps `./build/pcomm-read/pcomm-read`
  # runnable without LD_LIBRARY_PATH ceremony.
  set_property(TARGET pcomm_read APPEND PROPERTY BUILD_RPATH "${_duckdb_dir}")
endif()

# Header-only against libpcomm, so it carries no pcomm .so dependency — but it
# is still C++ and needs the toolchain libstdc++ it was built with, which is
# what PCOMM_INSTALL_RPATH_EXTRA supplies to an installed copy.
set_target_properties(pcomm_read PROPERTIES
  INSTALL_RPATH "${PCOMM_INSTALL_RPATH_EXTRA}")
target_link_options(pcomm_read PRIVATE -Wl,--enable-new-dtags)
if(PCOMM_INSTALL_CORE)
  install(TARGETS pcomm_read RUNTIME DESTINATION bin COMPONENT core)
endif()
