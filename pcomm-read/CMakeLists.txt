# The reading half of the one `pcomm` binary — read/dump/export/link/sql/types.
# An OBJECT library rather than an executable: there is a single command now, and
# a second entry point would be a second help text to keep in step. pcomm/ links
# this in; nothing else should.
#
# PUBLIC, not PRIVATE, throughout: usage requirements have to reach the target
# that actually links these objects, which lives in another directory.
add_library(pcomm_read OBJECT main.cpp)
# The reader decodes every provider's record schema; ib_records.hpp is
# vendor-neutral (no verbs.h), so this include adds no ibverbs dependency.
target_include_directories(pcomm_read PUBLIC ${CMAKE_SOURCE_DIR}/providers/ibverbs
                                             ${CMAKE_SOURCE_DIR}/providers/mlx5dv)
target_link_libraries(pcomm_read PUBLIC pcomm::pcomm)

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
  # Linking this target IS having DuckDB, so the flag the headers gate on rides
  # with it. It used to be PRIVATE on pcomm_read alone, which meant pcomm-ui got
  # the headers but silently compiled out every block guarded by the flag —
  # capture_load.hpp's loader vanished and the error named a missing member
  # rather than a missing define.
  target_compile_definitions(pcomm_duck INTERFACE PCOMM_HAVE_DUCKDB=1)
  target_include_directories(pcomm_duck INTERFACE
    "${CMAKE_SOURCE_DIR}/providers/ibverbs" "${CMAKE_SOURCE_DIR}/providers/mlx5dv")
  add_custom_target(pcomm_duck_gen DEPENDS "${_raw_hdr}")
  add_dependencies(pcomm_duck pcomm_duck_gen)

  target_sources(pcomm_read PRIVATE "${_raw_hdr}")

  target_compile_definitions(pcomm_read PUBLIC PCOMM_HAVE_DUCKDB=1)
  target_include_directories(pcomm_read PUBLIC "${_duckdb_dir}" "${CMAKE_CURRENT_BINARY_DIR}")
  target_link_libraries(pcomm_read PUBLIC "${_duckdb_dir}/libduckdb.so")
endif()

# Installation, rpath and the BUILD_RPATH that keeps ./build/pcomm/pcomm runnable
# without LD_LIBRARY_PATH ceremony all belong to the executable, in pcomm/.
