# libpcomm — L0–L2 core. Header-only for now (hot path is templates anyway).
#
# INVARIANT: nothing under libpcomm/ may include a vendor header
# (cuda*, hip*, infiniband/*, sanitizer*). Vendor types live in providers/
# and gpu-backends/ only. See ../ARCHITECTURE.md §0 and §6.

add_library(pcomm INTERFACE)
add_library(pcomm::pcomm ALIAS pcomm)

target_include_directories(pcomm INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)
target_compile_features(pcomm INTERFACE cxx_std_20)

# pcomm::engine (SHARED) — the process-wide control plane, and the one place
# the engine's singletons are defined.
#
# It must be SHARED, not STATIC. A static lib gets copied into every provider
# .so, which is what put 969 weakly-linked duplicate symbols (including the
# Tracer / ThreadBufferRegistry / RecordIdTracker guards) into each provider and
# made coexistence depend on which copy the linker happened to bind — see
# 7a5b55a and plans/PLAN-20260727-0126-shared-engine-abi-todo.md. One .so means
# one Tracer, one buffer store, one set of intern tables, N providers.
#
# `dlsym` lives here for the same reason: only one object in a process may
# define it, and Dlsym.hpp has always said so ("can only be linked against ONCE
# in the entire program"). Now the build enforces it — a second hook provider
# can no longer bring its own copy.
add_library(pcomm_engine SHARED
  src/engine.cpp
  src/hook/Dlsym.cpp
  src/hook/SymbolResolver.cpp
)
add_library(pcomm::engine ALIAS pcomm_engine)
# Kept as an alias so existing provider CMake keeps working; pcomm::hook is now
# the same shared engine rather than a separate static lib.
add_library(pcomm::hook ALIAS pcomm_engine)

# NB both are PRIVATE on purpose. `pcomm` carries INTERFACE cxx_std_20, and
# propagating that to a CUDA target makes CMake demand a "CUDA20" dialect it
# cannot map for nvcc (the clean-build failure fixed in 877ab90). Consumers get
# the headers from the INTERFACE include dir below without inheriting the
# language-standard requirement.
target_link_libraries(pcomm_engine PRIVATE pcomm ${CMAKE_DL_LIBS})
target_compile_features(pcomm_engine PRIVATE cxx_std_20)
target_include_directories(pcomm_engine INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

# Installed beside the providers in <prefix>/lib/pcomm, so they resolve their
# DT_NEEDED on it through $ORIGIN. pcomm_engine_so_path() then reports that
# installed path via dladdr, which is what the launcher puts first in
# LD_PRELOAD — the whole chain works with no environment set.
set_target_properties(pcomm_engine PROPERTIES
  INSTALL_RPATH "$ORIGIN;${PCOMM_INSTALL_RPATH_EXTRA}")
target_link_options(pcomm_engine PRIVATE -Wl,--enable-new-dtags)
if(PCOMM_INSTALL_CORE)
  install(TARGETS pcomm_engine LIBRARY DESTINATION ${PCOMM_INSTALL_LIBDIR}
          COMPONENT core)
  install(DIRECTORY include/pcomm DESTINATION include COMPONENT core)
endif()

# Schema conformance: assert the C++ records still match libpcomm/schema/pcomm.fbs.
#
# The FlatBuffers headers are vendored under third_party/ (520 KB, header-only,
# pinned to the flatc that generated pcomm_generated.h) precisely so this is not
# skipped anywhere. It used to be conditional on a path the developer supplied,
# which meant the one check standing between a record change and unreadable
# captures ran only when someone remembered to point at a FlatBuffers checkout —
# and on this machine that path was a scratch directory that no longer exists.
# Override PCOMM_FLATBUFFERS_INCLUDE to build against a different copy.
if(NOT DEFINED PCOMM_FLATBUFFERS_INCLUDE)
  set(PCOMM_FLATBUFFERS_INCLUDE ${CMAKE_SOURCE_DIR}/third_party/flatbuffers/include)
endif()

if(PCOMM_BUILD_TESTS AND EXISTS ${PCOMM_FLATBUFFERS_INCLUDE})
  add_executable(pcomm_schema_conformance tests/schema_conformance.cpp)
  target_include_directories(pcomm_schema_conformance PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/schema/generated
    ${CMAKE_SOURCE_DIR}/providers/ibverbs
    ${CMAKE_SOURCE_DIR}/providers/mlx5dv
    ${PCOMM_FLATBUFFERS_INCLUDE})
  target_compile_features(pcomm_schema_conformance PRIVATE cxx_std_20)
  # Keep assert() alive: every build type that runs tests defines
  # NDEBUG. See the note in tests/CMakeLists.txt.
  target_compile_options(pcomm_schema_conformance PRIVATE -UNDEBUG)
  add_test(NAME schema_conformance COMMAND pcomm_schema_conformance)
endif()

# Stream format round-trip. Header-only (FileSink and StreamHeader both are), so
# unlike tests/ this needs neither pcomm::engine nor a provider — it is the only
# local exercise of the writer.
if(PCOMM_BUILD_TESTS)
  add_executable(pcomm_stream_format tests/stream_format.cpp)
  target_link_libraries(pcomm_stream_format PRIVATE pcomm)
  target_compile_features(pcomm_stream_format PRIVATE cxx_std_20)
  # Keep assert() alive: every build type that runs tests defines
  # NDEBUG. See the note in tests/CMakeLists.txt.
  target_compile_options(pcomm_stream_format PRIVATE -UNDEBUG)
  add_test(NAME stream_format COMMAND pcomm_stream_format)
endif()

# Record versioning: a capture from before a schema bump and one from after must
# read into the same table. Header-only for the same reason stream_format is.
if(PCOMM_BUILD_TESTS)
  add_executable(pcomm_record_versioning tests/record_versioning.cpp)
  target_link_libraries(pcomm_record_versioning PRIVATE pcomm)
  target_compile_features(pcomm_record_versioning PRIVATE cxx_std_20)
  # Keep assert() alive: every build type that runs tests defines
  # NDEBUG. See the note in tests/CMakeLists.txt.
  target_compile_options(pcomm_record_versioning PRIVATE -UNDEBUG)
  add_test(NAME record_versioning COMMAND pcomm_record_versioning)
endif()

# Schema coverage: every PCOMM_RECORD in the tree is in pcomm.fbs, with the same
# fields in the same order, the same type_id, and a case in the conformance test.
#
# This one is attached to the *build*, not only to ctest. schema_conformance can
# only check records it names, so the failure it cannot see is an eleventh record
# that nobody added to it — and a check you have to remember to run is no check
# against a mistake made by forgetting. Text-level by necessity: no flatc on MN5.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
  set(schema_check_stamp ${CMAKE_CURRENT_BINARY_DIR}/schema_check.stamp)
  add_custom_command(
    OUTPUT ${schema_check_stamp}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/schema_check.py ${CMAKE_SOURCE_DIR}
    COMMAND ${CMAKE_COMMAND} -E touch ${schema_check_stamp}
    DEPENDS
      ${CMAKE_SOURCE_DIR}/tools/schema_check.py
      ${CMAKE_CURRENT_SOURCE_DIR}/schema/pcomm.fbs
      ${CMAKE_CURRENT_SOURCE_DIR}/include/pcomm/core/records.hpp
      ${CMAKE_CURRENT_SOURCE_DIR}/include/pcomm/core/vocab.hpp
      ${CMAKE_CURRENT_SOURCE_DIR}/include/pcomm/core/time.hpp
      ${CMAKE_CURRENT_SOURCE_DIR}/include/pcomm/core/ids.hpp
      ${CMAKE_CURRENT_SOURCE_DIR}/include/pcomm/rec/stream_header.hpp
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/schema_conformance.cpp
      ${CMAKE_SOURCE_DIR}/providers/ibverbs/ib_records.hpp
      # check 6 (ibverbs name tables) reads both readers' sources
      ${CMAKE_SOURCE_DIR}/pcomm-read/main.cpp
      ${CMAKE_SOURCE_DIR}/tools/read_pcs.py
    COMMENT "Checking C++ records against libpcomm/schema/pcomm.fbs"
    VERBATIM)
  add_custom_target(pcomm_schema_check ALL DEPENDS ${schema_check_stamp})
  add_dependencies(pcomm_engine pcomm_schema_check)

  if(PCOMM_BUILD_TESTS)
    add_test(NAME schema_check
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/schema_check.py
                     ${CMAKE_SOURCE_DIR})
  endif()
else()
  message(WARNING "no Python3: skipping the record/schema coverage check "
                  "(tools/schema_check.py) — a new record can reach a capture "
                  "with no schema entry")
endif()

# Generated-header drift: pcomm_generated.h must be what flatc makes of
# pcomm.fbs today. A test, not a build step — it needs flatc, which MN5 does not
# have, and the whole reason the header is committed is that the build must work
# without it. The script skips loudly rather than failing when flatc is absent.
if(PCOMM_BUILD_TESTS)
  add_test(NAME schema_generated_current
           COMMAND ${CMAKE_SOURCE_DIR}/tools/check_generated.sh)
endif()

# Portability, demonstrated rather than asserted: read a real suite capture from
# Python through flatc-generated bindings and recompute case 01's goldens. The
# claim that motivated the whole schema — "other languages can read captures" —
# was never once exercised until this ran. Needs no flatc and no network: the
# bindings and the FlatBuffers Python runtime are both committed.
if(PCOMM_BUILD_TESTS AND Python3_Interpreter_FOUND
   AND EXISTS ${CMAKE_SOURCE_DIR}/tests/data/ib_pingpong-44578115)
  add_test(NAME schema_python_reader
           COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/read_pcs.py)
endif()
