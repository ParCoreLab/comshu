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

# The injected function is compiled ON ITS OWN, whole-program, with
# -Xptxas -astoolspatch — NOT as part of the shared library below.
#
# That flag is what forbids ptxas the uniform datapath inside a tools patch, and
# it is not optional: NVBit preserves only UR4:UR5 across an injected call
# (SAVE_UREG_SPACE in libnvbit.a), so a patch that uses UR6+ destroys whatever
# the instrumented kernel had live there. Without the flag ptxas allocated
# pcomm_instrument_mem UR4-UR27, which is the "64-bit index" illegal access in
# ../nvbit-bug-report — a bug that had nothing to do with index width.
#
# It has to be a separate target because the flag cannot coexist with either of
# the things gpumem_trace needs. ptxas rejects it in a TU containing an entry
# function, and rejects it alongside -dc ("Conflicting options '--compile-only'
# and --compile-as-tools-patch"), while gpumem_trace requires
# CUDA_SEPARABLE_COMPILATION for nvbit_backend.cu. Putting the flag on the
# target's $<DEVICE_LINK:...> instead — which is what this build did until
# 2026-09-09 — compiles and links cleanly and applies nothing, because with -dc
# ptxas has already generated the SASS by then.
#
# Check it, do not assume it. The flag is silent when it fails to apply:
#   cuobjdump -sass libgpumem_trace.so | awk '/Function : pcomm_instrument_mem/{f=1} \
#     f&&/Function : /&&!/pcomm_instrument_mem/{f=0} f' | grep -oE 'UR[0-9]+' | sort -uV
# Any output at all means the fix is not in effect.
add_library(gpumem_inject OBJECT
  ${CMAKE_SOURCE_DIR}/gpu-backends/nvbit/inject_funcs.cu)
set_target_properties(gpumem_inject PROPERTIES
  CUDA_SEPARABLE_COMPILATION OFF
  POSITION_INDEPENDENT_CODE ON)
target_include_directories(gpumem_inject PRIVATE
  ${NVBIT_ROOT}
  ${CMAKE_SOURCE_DIR}/gpu-backends/nvbit
  ${CMAKE_SOURCE_DIR}/libpcomm/include
  ${CMAKE_CURRENT_SOURCE_DIR})
# -std=c++20 and -allow-unsupported-compiler are repeated here rather than
# inherited: this target deliberately does not link pcomm::engine (it needs only
# headers), so it gets none of gpumem_trace's compile options.
target_compile_options(gpumem_inject PRIVATE
  -std=c++20 -allow-unsupported-compiler
  -maxrregcount=24 --keep-device-functions -Xptxas -astoolspatch)

add_library(gpumem_trace SHARED
  gpumem_provider.cpp
  ${CMAKE_SOURCE_DIR}/gpu-backends/nvbit/nvbit_backend.cu
  $<TARGET_OBJECTS:gpumem_inject>
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

# (-maxrregcount=24 and --keep-device-functions moved onto gpumem_inject above,
# with the -astoolspatch they have to travel with.)

# DIAGNOSTIC ONLY, default OFF. inject_funcs.cu has carried a
# PCOMM_INJECT_NO_PUSH branch since it was written — instrument everything,
# push nothing — and nothing could switch it on, so the question it exists to
# answer has never been asked. It separates "the injected code perturbs the
# kernel" from "channel_dev->push() does", which is the last split standing in
# the p2p illegal-access investigation (../nvbit-bug-report).
#
# A capture built this way records NO accesses. Never ship it; the board would
# be green and empty.
option(PCOMM_INJECT_NO_PUSH "diagnostic: instrument but never push to the channel" OFF)
if(PCOMM_INJECT_NO_PUSH)
  target_compile_definitions(gpumem_inject PRIVATE PCOMM_INJECT_NO_PUSH)
  message(WARNING "PCOMM_INJECT_NO_PUSH=ON — gpumem will instrument but record nothing")
endif()

target_compile_options(gpumem_trace PRIVATE
  # -std=c++20 is set via CMAKE_CUDA_FLAGS above (kept out of here so CMake does
  # not parse it into a CUDA_STANDARD requirement it can't satisfy for nvcc).
  $<$<COMPILE_LANGUAGE:CUDA>:-allow-unsupported-compiler --keep-device-functions>)

# -Xptxas=-astoolspatch is deliberately NOT here. It was, and it did nothing:
# with CUDA_SEPARABLE_COMPILATION the sources are compiled -dc, so ptxas has
# already generated their SASS before the device link sees the flag. It now sits
# on gpumem_inject's compile line, which is the only place it has any effect.
target_link_options(gpumem_trace PRIVATE
  $<DEVICE_LINK:-allow-unsupported-compiler --keep-device-functions>)

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
# In an *installed* tree the engine is a sibling in <prefix>/lib/pcomm, so
# INSTALL_RPATH is $ORIGIN rather than the build tree's ../../libpcomm.
set_target_properties(gpumem_trace PROPERTIES
  BUILD_RPATH "$ORIGIN/../../libpcomm"
  INSTALL_RPATH "$ORIGIN;${PCOMM_INSTALL_RPATH_EXTRA}")
# COMPONENT so this provider can be installed on its own. gpumem is configured
# in a second build tree (build-gpumem: nvcc + the gcc-11.4 host compiler, see
# tests/hpc/run_on_mn5.sh) where the launcher is not built at all, so a plain
# `cmake --install build-gpumem` fails looking for bin/pcomm. The documented
# command is `cmake --install build-gpumem --component gpumem`.
install(TARGETS gpumem_trace LIBRARY DESTINATION ${PCOMM_INSTALL_LIBDIR}
        COMPONENT gpumem)

# Export only what must be exported (see gpumem.map): 3438 -> 9. HOST_LINK so
# the flag goes to the host linker, not to nvcc's device link step.
# NB the host linker here is nvc++ (NVHPC driver) and CMake's link-option list
# processing does not survive the trip: "-Wl,--version-script=..." gets split at
# the comma ("Switch -Wl expects an argument") and "-Xlinker --version-script=..."
# arrives with -Xlinker detached, so nvc++ rejects the path as its own switch.
# LINK_FLAGS is a raw string appended to the link line with no list mangling.
set_target_properties(gpumem_trace PROPERTIES
  LINK_FLAGS "-Xlinker --version-script=${CMAKE_CURRENT_SOURCE_DIR}/gpumem.map")
set_target_properties(gpumem_trace PROPERTIES
  LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/gpumem.map")
