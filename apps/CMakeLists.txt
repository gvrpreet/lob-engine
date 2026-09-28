# Command-line tools: make a synthetic feed, convert Nasdaq ITCH 5.0, replay and report.
foreach(app gen_events replay_cli itch_convert)
  add_executable(${app} ${app}.cpp)
  target_link_libraries(${app} PRIVATE lob::replay)
  lob_target_defaults(${app})
endforeach()
