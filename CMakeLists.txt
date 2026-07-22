cmake_minimum_required(VERSION 3.20)
project(ge_server LANGUAGES CXX VERSION 1.0.0)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

option(GE_NATIVE_ARCH  "Tune for the host microarchitecture"        ON)
option(GE_ALLOC_PROBE  "Intercept global new/delete to count allocations" ON)
option(GE_LTO          "Link-time optimization"                     OFF)
option(GE_SANITIZE     "Build with ASan+UBSan (tests/debug only)"   OFF)

include(cmake/CompilerFlags.cmake)

# ---------------------------------------------------------------- core library
add_library(ge_core STATIC
  src/core/util.cpp
  src/core/topology.cpp
  src/alloc/probe.cpp
  src/metrics/process.cpp
  src/metrics/perf_counters.cpp
  src/net/socket.cpp
  src/net/reactor.cpp
  src/net/uring.cpp
  src/net/connection.cpp
  src/world/shard.cpp
  src/lb/balancer.cpp
  src/server/world_server.cpp
  src/bench/loadgen.cpp
  src/bench/report.cpp
)
target_include_directories(ge_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(ge_core PUBLIC ge_flags)
find_package(Threads REQUIRED)
target_link_libraries(ge_core PUBLIC Threads::Threads)
if(GE_ALLOC_PROBE)
  target_compile_definitions(ge_core PUBLIC GE_ALLOC_PROBE=1)
endif()

# ---------------------------------------------------------------- executables
add_executable(ge_world_server apps/world_server/main.cpp)
target_link_libraries(ge_world_server PRIVATE ge_core)

add_executable(ge_gateway apps/gateway/main.cpp)
target_link_libraries(ge_gateway PRIVATE ge_core)

add_executable(ge_loadgen apps/loadgen/main.cpp)
target_link_libraries(ge_loadgen PRIVATE ge_core)

add_executable(ge_viewer apps/viewer/main.cpp)
target_link_libraries(ge_viewer PRIVATE ge_core)

add_executable(ge_bench bench/bench_main.cpp)
target_link_libraries(ge_bench PRIVATE ge_core)

# ---------------------------------------------------------------- tests
enable_testing()
add_executable(ge_tests
  tests/test_main.cpp
  tests/test_containers.cpp
  tests/test_proto.cpp
  tests/test_alloc.cpp
  tests/test_histogram.cpp
  tests/test_balancer.cpp
  tests/test_world.cpp
  tests/test_server.cpp
)
target_link_libraries(ge_tests PRIVATE ge_core)
add_test(NAME unit COMMAND ge_tests)

message(STATUS "ge_server  build=${CMAKE_BUILD_TYPE}  arch=${CMAKE_SYSTEM_PROCESSOR}  alloc_probe=${GE_ALLOC_PROBE}")
