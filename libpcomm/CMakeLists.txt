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

# Schema conformance: assert the C++ records still match libpcomm/schema/pcomm.fbs.
# Needs the FlatBuffers headers; skipped when they are absent (MN5 is airgapped),
# so this guards developer builds rather than the HPC build.
if(PCOMM_BUILD_TESTS AND DEFINED PCOMM_FLATBUFFERS_INCLUDE)
  add_executable(pcomm_schema_conformance tests/schema_conformance.cpp)
  target_include_directories(pcomm_schema_conformance PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/schema/generated
    ${CMAKE_SOURCE_DIR}/providers/ibverbs
    ${PCOMM_FLATBUFFERS_INCLUDE})
  target_compile_features(pcomm_schema_conformance PRIVATE cxx_std_20)
  add_test(NAME schema_conformance COMMAND pcomm_schema_conformance)
endif()
