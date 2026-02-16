cmake_minimum_required(VERSION 3.20)

project(phasegap VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

find_package(MPI REQUIRED COMPONENTS CXX)
find_package(OpenMP REQUIRED COMPONENTS CXX)

set(PHASEGAP_GIT_SHA "unknown")
execute_process(
  COMMAND git rev-parse --short=12 HEAD
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  OUTPUT_VARIABLE PHASEGAP_GIT_SHA_OUT
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_QUIET
)
if(PHASEGAP_GIT_SHA_OUT)
  set(PHASEGAP_GIT_SHA "${PHASEGAP_GIT_SHA_OUT}")
endif()

add_executable(phasegap
  src/cli/cli.cpp
  src/kernels/kernels.cpp
  src/main.cpp
  src/mpi/ring_halo.cpp
  src/stats/checksum.cpp
  src/stats/csv.cpp
  src/stats/manifest.cpp
  src/stats/metrics.cpp
  src/stats/timer.cpp
  src/trace/writer.cpp
)

target_include_directories(phasegap
  PRIVATE
    include
)

target_link_libraries(phasegap
  PRIVATE
    MPI::MPI_CXX
    OpenMP::OpenMP_CXX
)

target_compile_options(phasegap
  PRIVATE
    $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic>
)

target_compile_definitions(phasegap
  PRIVATE
    PHASEGAP_GIT_SHA="${PHASEGAP_GIT_SHA}"
)
