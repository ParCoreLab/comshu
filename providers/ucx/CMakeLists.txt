find_library(UCX_UCP_LIB NAMES ucp REQUIRED)
find_path(UCX_INCLUDE_DIR ucp/api/ucp.h REQUIRED)

add_library(ucx_trace SHARED ucx_provider.cpp)
set_target_properties(ucx_trace PROPERTIES PREFIX "lib" OUTPUT_NAME "ucx_trace")
target_include_directories(ucx_trace PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${UCX_INCLUDE_DIR})
target_link_libraries(ucx_trace PRIVATE comshu::comshu comshu::hook ${UCX_UCP_LIB})
set_target_properties(ucx_trace PROPERTIES
  BUILD_RPATH "$ORIGIN/../../libcomshu"
  INSTALL_RPATH "$ORIGIN;${COMSHU_INSTALL_RPATH_EXTRA}")
target_link_options(ucx_trace PRIVATE -Wl,--enable-new-dtags -Wl,--no-undefined
  "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/ucx.map")
set_target_properties(ucx_trace PROPERTIES LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/ucx.map")
install(TARGETS ucx_trace LIBRARY DESTINATION ${COMSHU_INSTALL_LIBDIR} COMPONENT ucx)
