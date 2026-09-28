# lob::replay - event-log format and readers (read, mmap, stream), replay loops (plain, paced,
# trading), the ITCH 5.0 converter, the synthetic generator and example strategies.
add_library(lob_replay
  src/amend.cpp
  src/event_log.cpp
  src/event_stream.cpp
  src/itch.cpp
  src/mapped_event_log.cpp
  src/paced.cpp
  src/replayer.cpp
  src/synthetic.cpp
  src/timer.cpp
  src/trading.cpp)
add_library(lob::replay ALIAS lob_replay)
set_target_properties(lob_replay PROPERTIES EXPORT_NAME replay)
target_include_directories(lob_replay PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(lob_replay PUBLIC lob::engine)
lob_target_defaults(lob_replay)
