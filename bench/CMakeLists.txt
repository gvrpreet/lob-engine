# Throughput and latency across all storage variants (see docs/workflow.md, benchmark protocol).
add_executable(bench_match bench_match.cpp)
target_link_libraries(bench_match PRIVATE lob::replay)
lob_target_defaults(bench_match)
