# nccl_trace — the host-API provider.
#
# NOT linked against libnccl. Every symbol we call into NCCL (pncclCommUserRank
# and friends, plus the pnccl twins the hooks resolve) is looked up at runtime
# in the process that already loaded it. Linking would pin this .so to one NCCL
# and break the thing that makes the provider portable: MN5's cases load the
# HPC SDK's 2.29.3 while other hosts have 2.18-2.24, and all of them expose the
# same PROFAPI surface.
find_path(NCCL_INCLUDE_DIR nccl.h REQUIRED)
find_path(CUDA_RT_INCLUDE_DIR cuda_runtime_api.h REQUIRED)

add_library(nccl_trace SHARED nccl_provider.cpp)
set_target_properties(nccl_trace PROPERTIES PREFIX "lib" OUTPUT_NAME "nccl_trace")
target_include_directories(nccl_trace PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${NCCL_INCLUDE_DIR}
  ${CUDA_RT_INCLUDE_DIR})
target_link_libraries(nccl_trace PRIVATE pcomm::pcomm pcomm::hook)
set_target_properties(nccl_trace PROPERTIES
  BUILD_RPATH "$ORIGIN/../../libpcomm"
  INSTALL_RPATH "$ORIGIN;${PCOMM_INSTALL_RPATH_EXTRA}")
# --no-undefined is deliberately NOT passed, unlike every other provider here:
# the pnccl symbols this calls are resolved from the application's libnccl at
# load time and are undefined at link time by design.
target_link_options(nccl_trace PRIVATE -Wl,--enable-new-dtags
  "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/nccl.map")
set_target_properties(nccl_trace PROPERTIES LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/nccl.map")
install(TARGETS nccl_trace LIBRARY DESTINATION ${PCOMM_INSTALL_LIBDIR} COMPONENT nccl)
