# libgpumem_trace.so — GPU memory-access provider (the Snoopie engine).
# Built as an NVBit tool: the .so carries the nvbit_at_* callbacks, the
# device-side instrumentation function, and the vendor-free engine that feeds
# the pcomm recorder. Build recipe follows snoopie/src/mem_multigpu.

if(NOT DEFINED CMAKE_CUDA_ARCHITECTURES OR CMAKE_CUDA_ARCHITECTURES STREQUAL "")
  set(CMAKE_CUDA_ARCHITECTURES 80)  # A100 (du04); override with -DCMAKE_CUDA_ARCHITECTURES
endif()
enable_language(CUDA)

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

target_include_directories(gpumem_trace PRIVATE
  ${NVBIT_ROOT}
  ${CMAKE_SOURCE_DIR}/gpu-backends/nvbit
  ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(gpumem_trace PRIVATE
  pcomm::pcomm
  ${NVBIT_ROOT}/libnvbit.a
  cuda
)

set_source_files_properties(${CMAKE_SOURCE_DIR}/gpu-backends/nvbit/inject_funcs.cu
  PROPERTIES COMPILE_FLAGS "-maxrregcount=24 --keep-device-functions")

target_compile_options(gpumem_trace PRIVATE
  $<$<COMPILE_LANGUAGE:CUDA>:-allow-unsupported-compiler --keep-device-functions>)

target_link_options(gpumem_trace PRIVATE
  $<DEVICE_LINK:-allow-unsupported-compiler -Xptxas=-astoolspatch --keep-device-functions>)
