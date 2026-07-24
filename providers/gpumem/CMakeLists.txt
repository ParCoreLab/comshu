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

# NB: we take pcomm's *headers* directly rather than linking pcomm::pcomm. That
# INTERFACE target carries `cxx_std_20`, which would propagate onto this CUDA
# target and make CMake demand a "CUDA20" dialect it can't map for nvcc 12.9
# (clean-configure error). The C++20 dialect is supplied by -std=c++20 below.
target_include_directories(gpumem_trace PRIVATE
  ${NVBIT_ROOT}
  ${CMAKE_SOURCE_DIR}/gpu-backends/nvbit
  ${CMAKE_SOURCE_DIR}/libpcomm/include
  ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(gpumem_trace PRIVATE
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
