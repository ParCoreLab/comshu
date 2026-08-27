# UCT-layer provider. Needs the same UCX tree the ucx provider does — the
# headers it reads (uct/api/tl.h, uct/api/version.h) are installed by UCX and
# sit beside ucp/api/ucp.h, so one include dir covers both.
#
# It links libucp, not libuct: the only UCX symbol it CALLS is
# ucp_get_version(), and uct_iface_open reaches the real implementation through
# RTLD_NEXT rather than through a link-time reference — which is what makes the
# interposition work at all.
find_library(UCX_UCP_LIB NAMES ucp REQUIRED)
find_path(UCX_INCLUDE_DIR ucp/api/ucp.h REQUIRED)

add_library(ucx_deep_trace SHARED ucx_deep_provider.cpp)
set_target_properties(ucx_deep_trace PROPERTIES PREFIX "lib" OUTPUT_NAME "ucx_deep_trace")
target_include_directories(ucx_deep_trace PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${UCX_INCLUDE_DIR})
target_link_libraries(ucx_deep_trace PRIVATE pcomm::pcomm pcomm::hook ${UCX_UCP_LIB} ${CMAKE_DL_LIBS})
set_target_properties(ucx_deep_trace PROPERTIES
  BUILD_RPATH "$ORIGIN/../../libpcomm"
  INSTALL_RPATH "$ORIGIN;${PCOMM_INSTALL_RPATH_EXTRA}")
target_link_options(ucx_deep_trace PRIVATE -Wl,--enable-new-dtags -Wl,--no-undefined
  "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/ucx_deep.map")
set_target_properties(ucx_deep_trace PROPERTIES LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/ucx_deep.map")
install(TARGETS ucx_deep_trace LIBRARY DESTINATION ${PCOMM_INSTALL_LIBDIR} COMPONENT ucx)
