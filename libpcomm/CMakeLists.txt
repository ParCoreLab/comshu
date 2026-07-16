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

# Interception engine (L2) — lifted from SigmaTrace's Hook/SmartDlsym, in the
# pcomm::hook / pcomm::smart_dlsym namespaces. Header engine is part of the
# INTERFACE lib above; the symbol-resolution runtime is compiled here.
# Still vendor-free: per-symbol traits live in the network providers.
add_library(pcomm_hook STATIC
  src/hook/Dlsym.cpp
  src/hook/SymbolResolver.cpp
)
add_library(pcomm::hook ALIAS pcomm_hook)
target_link_libraries(pcomm_hook PUBLIC pcomm ${CMAKE_DL_LIBS})
target_compile_features(pcomm_hook PUBLIC cxx_std_20)
