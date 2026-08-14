# libibverbs_trace.so — proof-of-concept network provider, ported from
# SigmaTrace/Ibtrace. Validates the hook engine (pcomm::hook) against the new
# record model. Requires libibverbs headers (infiniband/verbs.h).

find_library(IBVERBS_LIB NAMES ibverbs REQUIRED)
# The HEADERS are located, not assumed to be on the default include path. They
# were: the provider linked against a found libibverbs.so while `#include
# <infiniband/verbs.h>` relied on /usr/include, so a host with the runtime and
# no -devel package configured cleanly and then failed at the first compile.
# That is the host tools/get_devheaders.sh exists for — it extracts rdma-core
# into .tools/sysroot, which reaches the compiler only through this.
find_path(IBVERBS_INCLUDE_DIR infiniband/verbs.h REQUIRED)

# mlx5dv.h is OPTIONAL, and the option is the point: the CQE probe in
# mlx5_cqe.hpp reads a Mellanox-specific ring layout, so a host without the
# header still builds a provider — one whose probe says out loud that it is
# unavailable rather than being quietly compiled out. libmlx5 is NOT linked;
# mlx5dv_init_obj is resolved with dlsym at runtime, so this adds a header
# dependency and no DT_NEEDED.
#
# Checked WITH the located include dir, or a sysroot build would find verbs.h
# and then decide mlx5dv.h is missing while it sits in the same directory.
include(CheckIncludeFileCXX)
set(CMAKE_REQUIRED_INCLUDES ${IBVERBS_INCLUDE_DIR})
check_include_file_cxx("infiniband/mlx5dv.h" PCOMM_HAVE_MLX5DV_HEADER)
unset(CMAKE_REQUIRED_INCLUDES)

add_library(ibverbs_trace SHARED
  ibverbs_provider.cpp
  # Its own translation unit because it is the only one that may include
  # mlx5dv.h — see the note at the bottom of mlx5_cqe.hpp.
  mlx5_cqe.cpp
)
set_target_properties(ibverbs_trace PROPERTIES PREFIX "lib" OUTPUT_NAME "ibverbs_trace")
target_include_directories(ibverbs_trace PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${IBVERBS_INCLUDE_DIR})
target_link_libraries(ibverbs_trace PRIVATE pcomm::pcomm pcomm::hook ${IBVERBS_LIB})
target_compile_definitions(ibverbs_trace PRIVATE
  PCOMM_HAVE_MLX5DV=$<BOOL:${PCOMM_HAVE_MLX5DV_HEADER}>)

# The provider .so now has a DT_NEEDED on libpcomm_engine.so, so it must be
# findable at runtime. $ORIGIN/../../libpcomm resolves correctly for both build
# layouts: build/providers/<p>/ (ibverbs, and gpumem after run_on_mn5.sh copies
# it there from build-gpumem/). New dtags so LD_LIBRARY_PATH can still override.
# In an *installed* tree the engine is a sibling in <prefix>/lib/pcomm, so
# INSTALL_RPATH is $ORIGIN rather than the build tree's ../../libpcomm.
set_target_properties(ibverbs_trace PROPERTIES
  BUILD_RPATH "$ORIGIN/../../libpcomm"
  INSTALL_RPATH "$ORIGIN;${PCOMM_INSTALL_RPATH_EXTRA}")
target_link_options(ibverbs_trace PRIVATE -Wl,--enable-new-dtags)
# A shared library links happily with undefined symbols and fails at RUNTIME, on
# every rank, with "undefined symbol". That is exactly what a definition placed
# outside the namespace its declaration lives in produces, and it cost a job:
# the build printed a warning nobody was grepping for and reported success.
# --no-undefined turns that into a link error on the dev box instead.
target_link_options(ibverbs_trace PRIVATE -Wl,--no-undefined)
install(TARGETS ibverbs_trace LIBRARY DESTINATION ${PCOMM_INSTALL_LIBDIR}
        COMPONENT ibverbs)

# Export only what must be exported (see ibverbs.map). Cuts the dynamic symbol
# table from 2536 to 9 and removes the weak-duplicate surface that made
# provider coexistence a linker accident (7a5b55a).
target_link_options(ibverbs_trace PRIVATE
  "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/ibverbs.map")
set_target_properties(ibverbs_trace PROPERTIES
  LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/ibverbs.map")
