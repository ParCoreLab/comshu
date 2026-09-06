# libmlx5dv_trace.so — the DEVX control plane, which the ibverbs provider
# structurally cannot see (IBGDA never calls ibv_create_qp/modify_qp/post_send).
#
# Needs mlx5dv.h from rdma-core for the interposed symbols AND for the
# DEVX_GET/DEVX_ADDR_OF accessors, which rdma-core does define. The
# mlx5_ifc_*_bits structs those accessors compute offsets from are NOT shipped
# by rdma-core, which is why third_party/mlx5 is vendored.
#
# TWO headers, because NVSHMEM splits them that way and both are its copies
# verbatim: mlx5_ifc.h carries qpc/create_qp, and mlx5_prm.h carries cqc and
# create_cq. Taking the CQ structs from NVSHMEM's own tree rather than writing
# them out is deliberate — a bit-field layout transcribed by hand is decoded
# silently wrong, which is the failure this provider spends most of its comments
# on. Source: nvshmem/src/modules/transport/common/{mlx5_ifc,mlx5_prm}.h, the
# same files ibgda.cpp itself builds its DEVX commands with.

find_library(MLX5_LIB NAMES mlx5 REQUIRED)
find_library(IBVERBS_LIB_FOR_MLX5 NAMES ibverbs REQUIRED)
# Located rather than assumed, for the reason spelled out in the ibverbs
# provider: a host can carry the runtime .so without the -devel headers, and
# tools/get_devheaders.sh puts them somewhere only a find_path will reach.
find_path(MLX5DV_INCLUDE_DIR infiniband/mlx5dv.h REQUIRED)
find_package(Threads REQUIRED)

add_library(mlx5dv_trace SHARED
  mlx5dv_provider.cpp
  # The doorbell hijack's per-UAR repeater. Its wait_mem lives in a .cpp because
  # UMWAIT/MWAITX need per-function target attributes a header cannot carry.
  memory_repeater.cpp
)
set_target_properties(mlx5dv_trace PROPERTIES PREFIX "lib" OUTPUT_NAME "mlx5dv_trace")
target_include_directories(mlx5dv_trace PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${MLX5DV_INCLUDE_DIR}
  ${CMAKE_SOURCE_DIR}/third_party/mlx5/include)
target_link_libraries(mlx5dv_trace PRIVATE
  pcomm::pcomm pcomm::hook ${MLX5_LIB} ${IBVERBS_LIB_FOR_MLX5} ${CMAKE_DL_LIBS}
  Threads::Threads)

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
