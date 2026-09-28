// usage: itch_convert <in.NASDAQ_ITCH50> --list [top=20]
//        itch_convert <in.NASDAQ_ITCH50> <out.bin> <SYMBOL> [max_events=0] [--tick N] [--drop-off-grid]
// Converts one stock's order flow from a Nasdaq TotalView-ITCH 5.0 file into an event log
// (mapping in replay/itch.hpp). --list counts Add messages per symbol to help pick one.
// --tick 100 writes prices in cents (fails if any price is not a whole cent, unless
// --drop-off-grid, which drops such orders and reports how many).
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "replay/event_log.hpp"
#include "replay/itch.hpp"

int main(int argc, char** argv) {
  if (argc >= 3 && std::strcmp(argv[2], "--list") == 0) {
    const std::size_t top = argc > 3 ? std::strtoull(argv[3], nullptr, 10) : 20;
    std::vector<std::pair<std::string, std::uint64_t>> counts;
    std::string error;
    if (!replay::itch_add_counts(argv[1], counts, &error)) {
      std::fprintf(stderr, "cannot read %s: %s\n", argv[1], error.c_str());
      return 1;
    }
    for (std::size_t i = 0; i < counts.size() && i < top; ++i)
      std::printf("%-8s %llu adds\n", counts[i].first.c_str(), (unsigned long long)counts[i].second);
    return 0;
  }
  if (argc < 4) {
    std::fprintf(stderr, "usage: %s <in.itch> --list [top]\n       %s <in.itch> <out.bin> <SYMBOL> [max_events] [--tick N] [--drop-off-grid]\n",
                 argv[0], argv[0]);
    return 2;
  }
  replay::ItchOptions options;
  options.symbol = argv[3];
  for (int i = 4; i < argc; ++i) {
    if (std::strcmp(argv[i], "--tick") == 0 && i + 1 < argc) {
      options.tick = static_cast<std::uint32_t>(std::strtoul(argv[++i], nullptr, 10));
    } else if (std::strcmp(argv[i], "--drop-off-grid") == 0) {
      options.drop_off_grid = true;
    } else {
      options.max_events = std::strtoull(argv[i], nullptr, 10);
    }
  }

  const auto start = std::chrono::steady_clock::now();
  std::vector<replay::Event> events;
  replay::ItchStats stats;
  std::string error;
  if (!replay::convert_itch(argv[1], options, events, stats, &error)) {
    std::fprintf(stderr, "cannot convert %s: %s\n", argv[1], error.c_str());
    return 1;
  }
  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  if (!replay::write_events(argv[2], events)) {
    std::fprintf(stderr, "write failed: %s\n", argv[2]);
    return 1;
  }
  std::printf("symbol        : %s (stock locate %u), tick = %u ITCH units\n", options.symbol.c_str(),
              unsigned{stats.locate}, options.tick);
  std::printf("messages      : %llu read, %llu for the symbol (%.1f s)\n",
              (unsigned long long)stats.messages, (unsigned long long)stats.symbol_messages, secs);
  std::printf("order flow    : %llu adds, %llu executions, %llu cancels, %llu deletes, %llu replaces\n",
              (unsigned long long)stats.adds, (unsigned long long)stats.executions,
              (unsigned long long)stats.cancels, (unsigned long long)stats.deletes,
              (unsigned long long)stats.replaces);
  std::printf("unknown refs  : %llu (should be 0)\n", (unsigned long long)stats.unknown_refs);
  if (options.drop_off_grid)
    std::printf("off-grid      : %llu orders dropped (price not a multiple of the tick)\n",
                (unsigned long long)stats.off_grid_orders);
  std::printf("wrote         : %zu events to %s\n", events.size(), argv[2]);
}
