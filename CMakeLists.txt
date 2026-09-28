cmake_minimum_required(VERSION 3.20)
project(lob_engine VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)  # for clangd / clang-tidy

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(LOB_ASAN   "AddressSanitizer + UBSan" OFF)
option(LOB_TSAN   "ThreadSanitizer"          OFF)
option(LOB_NATIVE "-march=native (benchmarks only; not portable)" OFF)

add_library(lob_flags INTERFACE)
target_compile_options(lob_flags INTERFACE -Wall -Wextra -Wpedantic -Wconversion -Wshadow
                                          -fno-omit-frame-pointer)   # good perf/flamegraph stacks
if(LOB_NATIVE)
  target_compile_options(lob_flags INTERFACE -march=native)
endif()
if(LOB_ASAN)
  target_compile_options(lob_flags INTERFACE -fsanitize=address,undefined -fno-sanitize-recover=all)
  target_link_options(lob_flags INTERFACE -fsanitize=address,undefined)
endif()
if(LOB_TSAN)
  target_compile_options(lob_flags INTERFACE -fsanitize=thread)
  target_link_options(lob_flags INTERFACE -fsanitize=thread)
endif()

# ---- engine ---------------------------------------------------------------
add_library(lob src/order_book.cpp)
target_include_directories(lob PUBLIC include)
target_link_libraries(lob PUBLIC lob_flags)

# ---- replay framework -----------------------------------------------------
add_library(lob_replay replay/src/amend.cpp replay/src/event_log.cpp replay/src/mapped_event_log.cpp
                       replay/src/replayer.cpp replay/src/synthetic.cpp replay/src/timer.cpp
                       replay/src/trading.cpp)
target_include_directories(lob_replay PUBLIC replay/include)
target_link_libraries(lob_replay PUBLIC lob)

# ---- tools / bench --------------------------------------------------------
add_executable(gen_events   tools/gen_events.cpp)
add_executable(replay_cli   tools/replay_cli.cpp)
add_executable(bench_match  bench/bench_match.cpp)
foreach(t gen_events replay_cli bench_match)
  target_link_libraries(${t} PRIVATE lob_replay)
endforeach()

# ---- tests ----------------------------------------------------------------
enable_testing()
add_executable(lob_tests tests/test_main.cpp tests/test_order_book.cpp
                         tests/test_differential.cpp tests/test_replay.cpp
                         tests/test_latency.cpp tests/test_event_log.cpp tests/test_trading.cpp)
target_link_libraries(lob_tests PRIVATE lob_replay)
target_compile_definitions(lob_tests PRIVATE LOB_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
add_test(NAME lob_tests COMMAND lob_tests)
