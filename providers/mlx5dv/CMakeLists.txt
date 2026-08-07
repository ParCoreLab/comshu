# libmlx5dv_trace.so — the DEVX control plane, which the ibverbs provider
# structurally cannot see (IBGDA never calls ibv_create_qp/modify_qp/post_send).
#
# Needs mlx5dv.h from rdma-core for the interposed symbols AND for the
# DEVX_GET/DEVX_ADDR_OF accessors, which rdma-core does define. The
# mlx5_ifc_*_bits structs those accessors compute offsets from are NOT shipped
# by rdma-core, which is why third_party/mlx5 is vendored.

find_library(MLX5_LIB NAMES mlx5 REQUIRED)
find_library(IBVERBS_LIB_FOR_MLX5 NAMES ibverbs REQUIRED)

add_library(mlx5dv_trace SHARED
  mlx5dv_provider.cpp
)
set_target_properties(mlx5dv_trace PROPERTIES PREFIX "lib" OUTPUT_NAME "mlx5dv_trace")
target_include_directories(mlx5dv_trace PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/third_party/mlx5/include)
target_link_libraries(mlx5dv_trace PRIVATE
  pcomm::pcomm pcomm::hook ${MLX5_LIB} ${IBVERBS_LIB_FOR_MLX5} ${CMAKE_DL_LIBS})

# Same RPATH reasoning as the ibverbs provider: the .so has a DT_NEEDED on
# libpcomm_engine.so and must find it in both the build and installed layouts.
set_target_properties(mlx5dv_trace PROPERTIES
  BUILD_RPATH "$ORIGIN/../../libpcomm"
  INSTALL_RPATH "$ORIGIN;${PCOMM_INSTALL_RPATH_EXTRA}")
target_link_options(mlx5dv_trace PRIVATE -Wl,--enable-new-dtags)
# A shared library links happily with undefined symbols and fails at RUNTIME, on
# every rank, with "undefined symbol". That is exactly what a definition placed
# outside the namespace its declaration lives in produces, and it cost a job:
# the build printed a warning nobody was grepping for and reported success.
# --no-undefined turns that into a link error on the dev box instead.
target_link_options(mlx5dv_trace PRIVATE -Wl,--no-undefined)
install(TARGETS mlx5dv_trace LIBRARY DESTINATION ${PCOMM_INSTALL_LIBDIR}
        COMPONENT mlx5dv)

target_link_options(mlx5dv_trace PRIVATE
  "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/mlx5dv.map")
set_target_properties(mlx5dv_trace PROPERTIES
  LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/mlx5dv.map")
