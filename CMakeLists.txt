cmake_minimum_required(VERSION 3.20)
project(pcomm VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(PCOMM_BUILD_TESTS      "Build core tests (no GPU required)" ON)
option(PCOMM_PROVIDER_GPUMEM  "Build the GPU memory-access provider (needs CUDA + NVBit)" OFF)
option(PCOMM_PROVIDER_IBVERBS "Build the ibverbs provider (needs libibverbs)" OFF)

# enable_testing() must come before any add_subdirectory that calls add_test:
# CTest silently drops tests registered in a directory that was added first.
# libpcomm/ registers the schema tests, and while enable_testing() sat at the
# bottom of this file they were never in the ctest list at all — `ctest -R
# schema` reported "No tests were found", which is indistinguishable from
# passing if you only look at the exit code of a full `ctest` run.
if(PCOMM_BUILD_TESTS)
  enable_testing()
endif()

add_subdirectory(libpcomm)
add_subdirectory(providers)
add_subdirectory(pcomm)
add_subdirectory(pcomm-read)

if(PCOMM_BUILD_TESTS)
  add_subdirectory(tests)
endif()
