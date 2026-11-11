# libcupti_trace.so — kernel execution windows from CUPTI's activity API.
#
# Its own target rather than part of the engine because libcomshu_engine.so may
# not pull in a vendor header (the invariant at the top of libcomshu/src/
# engine.cpp), and its own target rather than part of a provider because it is
# not tied to a transport — it is the engine's GpuKernel record with the vendor
# binding kept at arm's length, the same shape as gpu-backends/nvbit.
#
# Plain C++, no CUDA language: this compiles no device code. It needs cupti.h
# for the activity record layouts and links libcupti; nvcc is not involved,
# which is what lets it build in the same tree as the ibverbs provider instead
# of needing gpumem's separate gcc-11.4 build.

find_path(CUPTI_INCLUDE_DIR cupti_activity.h
  HINTS
    ${CUDAToolkit_INCLUDE_DIRS}
    ${CUDA_TOOLKIT_ROOT_DIR}/extras/CUPTI/include
    $ENV{CUDA_HOME}/extras/CUPTI/include
    $ENV{CUDA_HOME}/targets/x86_64-linux/include
    /usr/local/cuda/extras/CUPTI/include
    /usr/local/cuda/targets/x86_64-linux/include)

find_library(CUPTI_LIBRARY cupti
  HINTS
    ${CUDA_TOOLKIT_ROOT_DIR}/extras/CUPTI/lib64
    $ENV{CUDA_HOME}/extras/CUPTI/lib64
    $ENV{CUDA_HOME}/targets/x86_64-linux/lib
    /usr/local/cuda/extras/CUPTI/lib64
    /usr/local/cuda/targets/x86_64-linux/lib)

# cuda.h, for CUfunction/CUstream/CUlaunchConfig in the launch interposers.
find_path(CUDA_DRIVER_INCLUDE_DIR cuda.h
  HINTS
    ${CUDAToolkit_INCLUDE_DIRS}
    $ENV{CUDA_HOME}/include
    $ENV{CUDA_HOME}/targets/x86_64-linux/include
    /usr/local/cuda/include
    /usr/local/cuda/targets/x86_64-linux/include)

if(NOT CUPTI_INCLUDE_DIR OR NOT CUPTI_LIBRARY OR NOT CUDA_DRIVER_INCLUDE_DIR)
  message(STATUS
    "comshu: cupti backend SKIPPED — cupti_activity.h/libcupti/cuda.h not found. "
    "Kernel lifetimes will be absent from captures made with this build. "
    "Set CUDA_HOME or -DCUDA_TOOLKIT_ROOT_DIR.")
  return()
endif()

add_library(cupti_trace SHARED cupti_backend.cpp)
set_target_properties(cupti_trace PROPERTIES PREFIX "lib" OUTPUT_NAME "cupti_trace")

target_include_directories(cupti_trace PRIVATE
  ${CUPTI_INCLUDE_DIR}
  ${CUDA_DRIVER_INCLUDE_DIR}
  ${CMAKE_SOURCE_DIR}/libcomshu/include)

# comshu::engine, not a private copy: this must share the ONE Tracer, recorder
# and intern-table set with every other loaded provider, by construction rather
# than by linker accident.
target_link_libraries(cupti_trace PRIVATE comshu::engine ${CUPTI_LIBRARY} dl)
target_compile_features(cupti_trace PRIVATE cxx_std_20)

# Same reasoning as the other providers: the DT_NEEDED on libcomshu_engine.so has
# to resolve at runtime from the build tree and from an installed tree, and new
# dtags so LD_LIBRARY_PATH still wins.
set_target_properties(cupti_trace PROPERTIES
  BUILD_RPATH "$ORIGIN/../../libcomshu"
  INSTALL_RPATH "$ORIGIN;${COMSHU_INSTALL_RPATH_EXTRA}")
target_link_options(cupti_trace PRIVATE "LINKER:--enable-new-dtags")

install(TARGETS cupti_trace LIBRARY DESTINATION ${COMSHU_INSTALL_LIBDIR}
        COMPONENT cupti)

message(STATUS "comshu: cupti backend ON — ${CUPTI_LIBRARY}")
