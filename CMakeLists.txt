cmake_minimum_required(VERSION 3.20)
project(pcomm VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(PCOMM_BUILD_TESTS      "Build core tests (no GPU required)" ON)
option(PCOMM_PROVIDER_GPUMEM  "Build the GPU memory-access provider (needs CUDA + NVBit)" OFF)
option(PCOMM_PROVIDER_IBVERBS "Build the ibverbs provider (needs libibverbs)" OFF)

add_subdirectory(libpcomm)
add_subdirectory(providers)
add_subdirectory(pcomm)
add_subdirectory(pcomm-read)

if(PCOMM_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
