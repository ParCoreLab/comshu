# libgpumem_trace.so — GPU memory-access provider (the Snoopie engine).
# Hosts the attribution chain; instrumentation is delegated to a gpu-backends/
# impl behind pcomm::gpu::IGpuBackend (NVBit first, Sanitizer next).
#
# The port of snoopie/src/mem_multigpu fills this in; add
# enable_language(CUDA) + NVBit paths when the device-side collector lands.

add_library(gpumem_trace SHARED
  gpumem_provider.cpp
)
set_target_properties(gpumem_trace PROPERTIES PREFIX "lib" OUTPUT_NAME "gpumem_trace")
target_link_libraries(gpumem_trace PRIVATE pcomm::pcomm)
