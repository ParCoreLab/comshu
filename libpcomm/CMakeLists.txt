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
