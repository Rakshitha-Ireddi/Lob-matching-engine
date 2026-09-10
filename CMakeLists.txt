# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.20)
project(lob_matching_engine LANGUAGES CXX VERSION 1.0.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(LOB_LTO "Link-time optimisation for Release builds" OFF)
if(LOB_LTO AND CMAKE_BUILD_TYPE STREQUAL "Release")
  include(CheckIPOSupported)
  check_ipo_supported(RESULT LOB_IPO_OK OUTPUT _ipo_msg)
  if(LOB_IPO_OK)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
  endif()
endif()

option(LOB_BUILD_TESTS "Build the GoogleTest suite" ON)
option(LOB_BUILD_APPS  "Build the engine server, simulator and benchmark" ON)
option(LOB_NATIVE      "Tune for the build host (-march=native)" ON)
option(LOB_ASAN        "Build with AddressSanitizer + UBSan" OFF)

add_library(lob_flags INTERFACE)
target_compile_options(lob_flags INTERFACE
  $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wshadow -Wnon-virtual-dtor -Wno-interference-size>
  $<$<AND:$<CONFIG:Release>,$<CXX_COMPILER_ID:GNU,Clang>>:-O3 -fno-omit-frame-pointer>
  $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive->
)
if(LOB_NATIVE)
  include(CheckCXXCompilerFlag)
  check_cxx_compiler_flag("-march=native" LOB_HAS_MARCH_NATIVE)
  if(LOB_HAS_MARCH_NATIVE)
    target_compile_options(lob_flags INTERFACE -march=native)
  endif()
endif()
if(LOB_ASAN)
  target_compile_options(lob_flags INTERFACE -fsanitize=address,undefined -g)
  target_link_options(lob_flags INTERFACE -fsanitize=address,undefined)
endif()
# Self-contained executables: no external libstdc++/libgcc/winpthread DLLs.
if(WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND NOT LOB_ASAN)
  target_link_options(lob_flags INTERFACE -static -static-libgcc -static-libstdc++)
endif()

# --- core library ---------------------------------------------------------
add_library(lob_core STATIC
  src/types.cpp
  src/clock.cpp
  src/matching_engine.cpp
  src/wire.cpp
  src/net.cpp
  src/order_flow.cpp
  src/telemetry.cpp
  src/engine_server.cpp
  src/affinity.cpp
)
target_include_directories(lob_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(lob_core PUBLIC lob_flags)

find_package(Threads REQUIRED)
target_link_libraries(lob_core PUBLIC Threads::Threads)
if(WIN32)
  target_link_libraries(lob_core PUBLIC ws2_32)
endif()

# --- apps ----------------------------------------------------------------
if(LOB_BUILD_APPS)
  add_executable(engine_server apps/engine_server/main.cpp)
  target_link_libraries(engine_server PRIVATE lob_core)

  add_executable(md_simulator apps/md_simulator/main.cpp)
  target_link_libraries(md_simulator PRIVATE lob_core)

  add_executable(lob_bench apps/bench/main.cpp)
  target_link_libraries(lob_bench PRIVATE lob_core)
endif()

# --- tests -------------------------------------------------------------
if(LOB_BUILD_TESTS)
  include(FetchContent)
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.15.2
    GIT_SHALLOW    TRUE
  )
  FetchContent_MakeAvailable(googletest)

  enable_testing()
  add_executable(lob_tests
    tests/test_order_book.cpp
    tests/test_price_time_priority.cpp
    tests/test_matching_engine.cpp
    tests/test_spsc_ring.cpp
    tests/test_flat_hash_map.cpp
    tests/test_latency_histogram.cpp
    tests/test_wire.cpp
    tests/test_determinism.cpp
    tests/test_book_equivalence.cpp
  )
  target_link_libraries(lob_tests PRIVATE lob_core GTest::gtest_main)

  include(GoogleTest)
  gtest_discover_tests(lob_tests DISCOVERY_TIMEOUT 30)
endif()
