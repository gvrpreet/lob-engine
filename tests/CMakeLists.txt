# unit/     one executable (lob_tests), zero-dependency harness in support/testing.hpp
# support/  the harness and the naive RefBook oracle used by differential tests and the fuzzer
# data/     committed golden event logs with pinned results
# fuzz/     libFuzzer harness (clang, -DLOB_FUZZ=ON; see scripts/fuzz.sh)
if(LOB_BUILD_TESTS)
  add_executable(lob_tests
    unit/test_main.cpp
    unit/test_order_book.cpp
    unit/test_differential.cpp
    unit/test_replay.cpp
    unit/test_latency.cpp
    unit/test_event_log.cpp
    unit/test_trading.cpp
    unit/test_itch.cpp
    unit/test_stream.cpp
    unit/test_paced.cpp)
  target_include_directories(lob_tests PRIVATE support)
  target_link_libraries(lob_tests PRIVATE lob::replay)
  target_compile_definitions(lob_tests PRIVATE LOB_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
  lob_target_defaults(lob_tests)
  add_test(NAME lob_tests COMMAND lob_tests)
endif()

if(LOB_FUZZ)
  add_executable(fuzz_book fuzz/fuzz_book.cpp)
  target_include_directories(fuzz_book PRIVATE support)
  target_link_libraries(fuzz_book PRIVATE lob::engine)
  target_compile_options(fuzz_book PRIVATE -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all)
  target_link_options(fuzz_book PRIVATE -fsanitize=fuzzer,address,undefined)
endif()
