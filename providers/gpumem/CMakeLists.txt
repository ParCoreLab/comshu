# libgpumem_trace.so — GPU memory-access provider (the Snoopie engine).
# Built as an NVBit tool: the .so carries the nvbit_at_* callbacks, the
# device-side instrumentation function, and the vendor-free engine that feeds
# the pcomm recorder. Build recipe follows snoopie/src/mem_multigpu.

if(NOT DEFINED CMAKE_CUDA_ARCHITECTURES OR CMAKE_CUDA_ARCHITECTURES STREQUAL "")
  set(CMAKE_CUDA_ARCHITECTURES 80)  # A100 (du04); override with -DCMAKE_CUDA_ARCHITECTURES
endif()
enable_language(CUDA)

# This target mixes a C++20 host source with CUDA sources, so CMake ties the CUDA
# dialect to C++20 (CUDA20). CMake 3.25 knows nvcc supports it but is missing the
# flag mapping ("does not know the flags to enable it") → clean-configure error.
# Teach it the flag; then the normal standard machinery emits -std=c++20 for nvcc.
set(CMAKE_CUDA20_STANDARD_COMPILE_OPTION  "-std=c++20")
set(CMAKE_CUDA20_EXTENSION_COMPILE_OPTION "-std=c++20")

set(NVBIT_ROOT ${CMAKE_SOURCE_DIR}/gpu-backends/nvbit/nvbit-1.7.7.1)

add_library(gpumem_trace SHARED
  gpumem_provider.cpp
  ${CMAKE_SOURCE_DIR}/gpu-backends/nvbit/nvbit_backend.cu
  ${CMAKE_SOURCE_DIR}/gpu-backends/nvbit/inject_funcs.cu
)
set_target_properties(gpumem_trace PROPERTIES
  PREFIX "lib" OUTPUT_NAME "gpumem_trace"
  CUDA_SEPARABLE_COMPILATION ON
)
# NB: C++20 for the CUDA compile is forced via -std=c++20 in target_compile_options
# below (engine.hpp/recorder use concepts). We do NOT use the CUDA_STANDARD 20
# property: with this nvcc CMake fails to map the "CUDA20" dialect and errors at
# configure, and it doesn't emit the -std flag anyway.

# NB: still no link to pcomm::pcomm. That INTERFACE target carries `cxx_std_20`,
# which would propagate onto this CUDA target and make CMake demand a "CUDA20"
# dialect it can't map for nvcc 12.9 (clean-configure error, 877ab90). The C++20
# dialect is supplied by -std=c++20 below. pcomm::engine is safe to link: its
# cxx_std_20 is PRIVATE, so we get its INTERFACE include dir and nothing else.
target_include_directories(gpumem_trace PRIVATE
  ${NVBIT_ROOT}
  ${CMAKE_SOURCE_DIR}/gpu-backends/nvbit
  ${CMAKE_SOURCE_DIR}/libpcomm/include
  ${CMAKE_CURRENT_SOURCE_DIR}
)

# Linking the shared engine (rather than embedding the header-only one) is what
# makes this provider share ONE Tracer/Recorder/intern-table set with every
# other provider in the process, by construction instead of by linker accident.
target_link_libraries(gpumem_trace PRIVATE
  pcomm::engine
  ${NVBIT_ROOT}/libnvbit.a
  cuda
)

set_source_files_properties(${CMAKE_SOURCE_DIR}/gpu-backends/nvbit/inject_funcs.cu
  PROPERTIES COMPILE_FLAGS "-maxrregcount=24 --keep-device-functions")

target_compile_options(gpumem_trace PRIVATE
  # -std=c++20 is set via CMAKE_CUDA_FLAGS above (kept out of here so CMake does
  # not parse it into a CUDA_STANDARD requirement it can't satisfy for nvcc).
  $<$<COMPILE_LANGUAGE:CUDA>:-allow-unsupported-compiler --keep-device-functions>)

target_link_options(gpumem_trace PRIVATE
  $<DEVICE_LINK:-allow-unsupported-compiler -Xptxas=-astoolspatch --keep-device-functions>)

# The NVHPC driver bakes a DT_RPATH to its own compilers/lib + the gcc-11.4 host
# lib64 into this .so. DT_RPATH outranks LD_LIBRARY_PATH, so when gpumem is
# preloaded ahead of a hook provider it supplies gcc-11.4's libstdc++ to the
# whole process and the gcc-13.2-built provider dies on a missing GLIBCXX_3.4.31
# — coexistence would then depend on LD_PRELOAD order. New dtags emit DT_RUNPATH
# instead, which is searched *after* LD_LIBRARY_PATH, so the runtime module
# (gcc/13.2.0, per common.sh) wins and either order works.
target_link_options(gpumem_trace PRIVATE $<HOST_LINK:-Xlinker --enable-new-dtags>)

# The provider .so now has a DT_NEEDED on libpcomm_engine.so, so it must be
# findable at runtime. $ORIGIN/../../libpcomm resolves correctly for both build
# layouts: build/providers/<p>/ (ibverbs, and gpumem after run_on_mn5.sh copies
# it there from build-gpumem/). New dtags so LD_LIBRARY_PATH can still override.
set_target_properties(gpumem_trace PROPERTIES
  BUILD_RPATH "$ORIGIN/../../libpcomm"
  INSTALL_RPATH "$ORIGIN/../../libpcomm")
