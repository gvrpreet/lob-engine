cmake_minimum_required(VERSION 3.20)
project(lob_engine
  VERSION 0.9.0
  DESCRIPTION "Price-time priority limit order book and deterministic market-replay framework"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)  # for clangd / clang-tidy

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Tests, apps, benchmarks and install rules default to on only when this is the top-level
# project, so the libraries can be consumed with add_subdirectory() / FetchContent.
set(LOB_TOP_LEVEL OFF)
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
  set(LOB_TOP_LEVEL ON)
endif()
option(LOB_BUILD_TESTS "Build the unit test suite"                    ${LOB_TOP_LEVEL})
option(LOB_BUILD_APPS  "Build gen_events, replay_cli, itch_convert"   ${LOB_TOP_LEVEL})
option(LOB_BUILD_BENCH "Build the bench_match benchmark"              ${LOB_TOP_LEVEL})
option(LOB_INSTALL     "Generate install and CMake package rules"     ${LOB_TOP_LEVEL})
option(LOB_ASAN   "AddressSanitizer + UBSan"                          OFF)
option(LOB_TSAN   "ThreadSanitizer"                                   OFF)
option(LOB_NATIVE "-march=native (benchmarks only; not portable)"     OFF)
option(LOB_FUZZ   "libFuzzer harness tests/fuzz/fuzz_book.cpp (clang)" OFF)

include(GNUInstallDirs)

# Build-wide settings: these change code generation or ABI, so every target must agree.
add_compile_options(-fno-omit-frame-pointer)  # usable perf / flame-graph stacks
if(LOB_NATIVE)
  add_compile_options(-march=native)
endif()
if(LOB_ASAN)
  add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=all)
  add_link_options(-fsanitize=address,undefined)
endif()
if(LOB_TSAN)
  add_compile_options(-fsanitize=thread)
  add_link_options(-fsanitize=thread)
endif()

# Warnings for this project's own targets only (never propagated to consumers).
function(lob_target_defaults target)
  target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
endfunction()

# Executables land in the build root (build/<preset>/replay_cli, ...): scripts and docs rely on it.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR})

add_subdirectory(engine)   # lob::engine  - the order book
add_subdirectory(replay)   # lob::replay  - event logs, readers, replay loops, ITCH, strategies
if(LOB_BUILD_APPS)
  add_subdirectory(apps)
endif()
if(LOB_BUILD_BENCH)
  add_subdirectory(bench)
endif()
if(LOB_BUILD_TESTS OR LOB_FUZZ)
  enable_testing()
  add_subdirectory(tests)
endif()

# ---- install / find_package(lob_engine) --------------------------------------
if(LOB_INSTALL)
  include(CMakePackageConfigHelpers)
  set(LOB_CMAKE_DIR ${CMAKE_INSTALL_LIBDIR}/cmake/lob_engine)
  install(TARGETS lob_engine lob_replay EXPORT lob_engineTargets
          ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
          LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
  install(DIRECTORY engine/include/ replay/include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
  install(EXPORT lob_engineTargets NAMESPACE lob:: DESTINATION ${LOB_CMAKE_DIR})
  configure_package_config_file(cmake/lob_engineConfig.cmake.in
    ${PROJECT_BINARY_DIR}/lob_engineConfig.cmake INSTALL_DESTINATION ${LOB_CMAKE_DIR})
  write_basic_package_version_file(${PROJECT_BINARY_DIR}/lob_engineConfigVersion.cmake
    COMPATIBILITY SameMinorVersion)
  install(FILES ${PROJECT_BINARY_DIR}/lob_engineConfig.cmake
                ${PROJECT_BINARY_DIR}/lob_engineConfigVersion.cmake
          DESTINATION ${LOB_CMAKE_DIR})
  if(LOB_BUILD_APPS)
    install(TARGETS gen_events replay_cli itch_convert RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
  endif()
endif()
