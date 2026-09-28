// Phase 5(d): ITCH 5.0 converter, on ITCH messages built byte by byte (no binary fixture).
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "replay/itch.hpp"
#include "replay/replayer.hpp"
#include "testing.hpp"

using replay::Event;
using replay::EventType;

namespace {

class Itch {
 public:
  Itch& system(std::uint64_t ts) { begin('S', 0, ts); u(1, 'O'); return end(); }
  Itch& directory(std::uint16_t locate, const char* stock) {
    begin('R', locate, 0);
    sym(stock);
    for (int i = 0; i < 20; ++i) u(1, 0);  // market category ... etc.: unused
    return end();
  }
  Itch& add(std::uint16_t locate, std::uint64_t ts, std::uint64_t ref, char side,
            std::uint32_t shares, const char* stock, std::uint32_t price, bool mpid = false) {
    begin(mpid ? 'F' : 'A', locate, ts);
    u(8, ref); u(1, static_cast<std::uint64_t>(side)); u(4, shares); sym(stock); u(4, price);
    if (mpid) sym4("MPID");
    return end();
  }
  Itch& executed(std::uint16_t locate, std::uint64_t ts, std::uint64_t ref, std::uint32_t shares) {
    begin('E', locate, ts); u(8, ref); u(4, shares); u(8, 1); return end();
  }
  Itch& executed_price(std::uint16_t locate, std::uint64_t ts, std::uint64_t ref,
                       std::uint32_t shares, std::uint32_t price) {
    begin('C', locate, ts); u(8, ref); u(4, shares); u(8, 2); u(1, 'Y'); u(4, price); return end();
  }
  Itch& cancel(std::uint16_t locate, std::uint64_t ts, std::uint64_t ref, std::uint32_t shares) {
    begin('X', locate, ts); u(8, ref); u(4, shares); return end();
  }
  Itch& remove(std::uint16_t locate, std::uint64_t ts, std::uint64_t ref) {
    begin('D', locate, ts); u(8, ref); return end();
  }
  Itch& replace(std::uint16_t locate, std::uint64_t ts, std::uint64_t ref, std::uint64_t new_ref,
                std::uint32_t shares, std::uint32_t price) {
    begin('U', locate, ts); u(8, ref); u(8, new_ref); u(4, shares); u(4, price); return end();
  }
  const std::vector<char>& bytes() const { return bytes_; }

 private:
  void begin(char type, std::uint16_t locate, std::uint64_t ts) {
    msg_.clear();
    u(1, static_cast<std::uint64_t>(type)); u(2, locate); u(2, 0); u(6, ts);
  }
  Itch& end() {
    const auto n = msg_.size();
    bytes_.push_back(static_cast<char>(n >> 8));
    bytes_.push_back(static_cast<char>(n & 0xff));
    bytes_.insert(bytes_.end(), msg_.begin(), msg_.end());
    return *this;
  }
  void u(int bytes, std::uint64_t v) {  // big-endian
    for (int i = bytes - 1; i >= 0; --i) msg_.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
  }
  void sym(const char* s) {
    std::string p(s);
    p.resize(8, ' ');
    msg_.insert(msg_.end(), p.begin(), p.end());
  }
  void sym4(const char* s) { msg_.insert(msg_.end(), s, s + 4); }
  std::vector<char> msg_, bytes_;
};

std::string write_temp(const char* name, const std::vector<char>& bytes) {
  const std::string path = (std::filesystem::temp_directory_path() / name).string();
  std::ofstream(path, std::ios::binary).write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  return path;
}

Itch sample() {
  Itch f;
  f.system(1)
      .directory(7, "TEST")
      .directory(8, "OTHR")
      .add(7, 1000, 100, 'B', 50, "TEST", 1'000'000)
      .add(7, 1001, 200, 'S', 30, "TEST", 1'010'000)
      .add(7, 1002, 300, 'B', 20, "TEST", 1'000'000, true)
      .add(8, 1003, 999, 'B', 5, "OTHR", 50)            // other stock: ignored
      .executed(7, 1004, 100, 10)                        // 100: 50 -> 40
      .executed_price(7, 1005, 200, 5, 1'009'000)        // 200: 30 -> 25
      .cancel(7, 1006, 300, 20)                          // 300: gone
      .replace(7, 1007, 100, 101, 40, 1'000'000)         // same price, same shares: Cancel + New
      .replace(7, 1008, 101, 102, 60, 1'000'000)         // more shares: Modify
      .replace(7, 1009, 200, 201, 25, 1'005'000)         // new price: Modify
      .remove(7, 1010, 102)                              // deletes engine order 100
      .cancel(7, 1011, 555, 1)                           // never added: unknown ref
      .remove(8, 1012, 999);                             // other stock: ignored
  return f;
}

bool same(const Event& e, EventType type, std::uint64_t ts, std::uint64_t id, lob::Side side,
          lob::Price price, lob::Qty qty) {
  return e.type == type && e.ts == ts && e.id == id && e.side == side && e.price == price &&
         e.qty == qty && e.otype == lob::OrderType::Limit && e.reserved == 0;
}

}  // namespace

TEST(itch_converts_one_symbol_with_exact_mapping) {
  const std::string path = write_temp("lob_itch_sample.bin", sample().bytes());
  std::vector<Event> ev;
  replay::ItchStats st;
  std::string error;
  CHECK(replay::convert_itch(path, {"TEST", 0}, ev, st, &error));
  std::remove(path.c_str());

  using S = lob::Side;
  CHECK(st.locate == 7 && st.messages == 16 && st.symbol_messages == 12);
  CHECK(st.adds == 3 && st.executions == 2 && st.cancels == 2 && st.deletes == 1 && st.replaces == 3);
  CHECK(st.unknown_refs == 1);
  CHECK(ev.size() == 11);
  if (ev.size() != 11) return;
  CHECK(same(ev[0], EventType::New, 1000, 100, S::Buy, 1'000'000, 50));
  CHECK(same(ev[1], EventType::New, 1001, 200, S::Sell, 1'010'000, 30));
  CHECK(same(ev[2], EventType::New, 1002, 300, S::Buy, 1'000'000, 20));
  CHECK(same(ev[3], EventType::Reduce, 1004, 100, S::Buy, 0, 10));
  CHECK(same(ev[4], EventType::Reduce, 1005, 200, S::Buy, 0, 5));
  CHECK(same(ev[5], EventType::Reduce, 1006, 300, S::Buy, 0, 20));
  CHECK(same(ev[6], EventType::Cancel, 1007, 100, S::Buy, 0, 0));
  CHECK(same(ev[7], EventType::New, 1007, 100, S::Buy, 1'000'000, 40));
  CHECK(same(ev[8], EventType::Modify, 1008, 100, S::Buy, 1'000'000, 60));
  CHECK(same(ev[9], EventType::Modify, 1009, 200, S::Buy, 1'005'000, 25));
  CHECK(ev[10].type == EventType::Cancel && ev[10].ts == 1010 && ev[10].id == 100);  // D of ref 102
}

TEST(itch_converted_log_replays_to_the_reconstructed_book) {
  const std::string path = write_temp("lob_itch_sample2.bin", sample().bytes());
  std::vector<Event> ev;
  replay::ItchStats st;
  CHECK(replay::convert_itch(path, {"TEST", 0}, ev, st));
  std::remove(path.c_str());
  CHECK(ev.size() == 11 && ev.back().type == EventType::Cancel && ev.back().id == 100);

  const auto r = replay::replay(ev);
  CHECK(r.trades == 0 && r.rejected == 0);  // a displayed feed never crosses
  CHECK(r.cancel_misses == 0 && r.modify_misses == 0 && r.reduce_misses == 0);
  CHECK(r.final_orders == 1);  // only order 200: sell 25 @ 1'005'000
}

TEST(itch_tick_divides_prices_and_rejects_off_grid) {
  const std::string path = write_temp("lob_itch_tick.bin", sample().bytes());
  std::vector<Event> ev;
  replay::ItchStats st;
  std::string error;
  replay::ItchOptions cents{"TEST", 0, 100};
  CHECK(replay::convert_itch(path, cents, ev, st, &error));
  CHECK(ev.size() == 11 && ev[0].price == 10'000 && ev[1].price == 10'100 && ev[9].price == 10'050);
  CHECK(replay::replay(ev).trades == 0);

  replay::ItchOptions dollars{"TEST", 0, 10'000};  // 1'005'000 is not a whole dollar
  CHECK(!replay::convert_itch(path, dollars, ev, st, &error) && ev.empty());
  CHECK(error.find("1005000") != std::string::npos);
  replay::ItchOptions zero{"TEST", 0, 0};
  CHECK(!replay::convert_itch(path, zero, ev, st, &error));
  std::remove(path.c_str());
}

TEST(itch_drop_off_grid_skips_sub_penny_orders_and_their_messages) {
  Itch f;
  f.directory(7, "TEST")
      .add(7, 100, 10, 'B', 5, "TEST", 1)             // $0.0001 bid: off the cent grid
      .executed(7, 101, 10, 1)                        // for the dropped order: skipped
      .add(7, 102, 20, 'S', 5, "TEST", 1'010'000)
      .replace(7, 103, 20, 21, 5, 1'010'050)          // moves off the grid: leaves the book
      .replace(7, 104, 10, 11, 5, 990'000)            // dropped order moves onto the grid: enters
      .remove(7, 105, 21)                             // dropped: skipped
      .remove(7, 106, 11);
  const std::string path = write_temp("lob_itch_offgrid.bin", f.bytes());
  std::vector<Event> ev;
  replay::ItchStats st;
  std::string error;
  CHECK(!replay::convert_itch(path, {"TEST", 0, 100}, ev, st, &error));  // strict by default

  replay::ItchOptions drop{"TEST", 0, 100};
  drop.drop_off_grid = true;
  CHECK(replay::convert_itch(path, drop, ev, st, &error));
  std::remove(path.c_str());
  using S = lob::Side;
  CHECK(st.off_grid_orders == 2 && st.unknown_refs == 0);
  CHECK(ev.size() == 4);
  if (ev.size() != 4) return;
  CHECK(same(ev[0], EventType::New, 102, 20, S::Sell, 10'100, 5));
  CHECK(ev[1].type == EventType::Cancel && ev[1].id == 20);
  CHECK(same(ev[2], EventType::New, 104, 10, S::Buy, 9'900, 5));
  CHECK(ev[3].type == EventType::Cancel && ev[3].id == 10);
  const auto r = replay::replay(ev);
  CHECK(r.cancel_misses == 0 && r.final_orders == 0 && r.trades == 0);
}

TEST(itch_max_events_and_errors) {
  const std::string path = write_temp("lob_itch_sample3.bin", sample().bytes());
  std::vector<Event> ev;
  replay::ItchStats st;
  std::string error;
  CHECK(replay::convert_itch(path, {"TEST", 4}, ev, st) && ev.size() == 4);
  CHECK(!replay::convert_itch(path, {"NOPE", 0}, ev, st, &error) && !error.empty());

  std::vector<std::pair<std::string, std::uint64_t>> counts;
  CHECK(replay::itch_add_counts(path, counts));
  CHECK(counts.size() == 2 && counts[0].first == "TEST" && counts[0].second == 3 &&
        counts[1].first == "OTHR" && counts[1].second == 1);
  std::remove(path.c_str());

  auto truncated = sample().bytes();
  truncated.resize(truncated.size() - 3);
  const std::string bad = write_temp("lob_itch_truncated.bin", truncated);
  error.clear();
  CHECK(!replay::convert_itch(bad, {"TEST", 0}, ev, st, &error) && !error.empty());
  std::remove(bad.c_str());

  Itch short_msg;
  short_msg.system(1);
  auto bytes = short_msg.bytes();
  bytes[1] = 5;  // claims a 5-byte message: shorter than the 11-byte header
  bytes.resize(2 + 5);
  const std::string tiny = write_temp("lob_itch_short.bin", bytes);
  error.clear();
  CHECK(!replay::itch_add_counts(tiny, counts, &error) && !error.empty());
  std::remove(tiny.c_str());
}
