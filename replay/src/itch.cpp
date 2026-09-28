#include "replay/itch.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <unordered_map>

namespace replay {

namespace {

bool fail(std::string* error, std::string reason) {
  if (error) *error = std::move(reason);
  return false;
}

std::uint64_t be(const unsigned char* p, int bytes) {
  std::uint64_t v = 0;
  for (int i = 0; i < bytes; ++i) v = (v << 8) | p[i];
  return v;
}

// Minimum length per message type for the fields read below (ITCH 5.0 spec sizes). Every
// message has the 11-byte header (type, stock locate, tracking number, 6-byte timestamp).
std::size_t required_length(unsigned char type) {
  switch (type) {
    case 'R': return 39;
    case 'A': return 36;
    case 'F': return 40;
    case 'E': return 31;
    case 'C': return 36;
    case 'X': return 23;
    case 'D': return 19;
    case 'U': return 35;
    default: return 11;
  }
}

// Streams length-prefixed messages through a 1 MiB buffer (the file can be ~12 GB).
class MessageReader {
 public:
  explicit MessageReader(std::FILE* f) : f_(f), buf_(1u << 20) {}

  // 1 = message, 0 = clean end of file, -1 = truncated file.
  int next(const unsigned char*& msg, std::size_t& length) {
    if (!ensure(2)) return avail() == 0 ? 0 : -1;
    length = static_cast<std::size_t>(be(buf_.data() + pos_, 2));
    if (!ensure(2 + length)) return -1;
    msg = buf_.data() + pos_ + 2;
    pos_ += 2 + length;
    return 1;
  }

 private:
  std::size_t avail() const { return len_ - pos_; }
  bool ensure(std::size_t n) {
    if (avail() >= n) return true;
    std::memmove(buf_.data(), buf_.data() + pos_, avail());
    len_ = avail();
    pos_ = 0;
    while (len_ < n) {
      const std::size_t got = std::fread(buf_.data() + len_, 1, buf_.size() - len_, f_);
      if (got == 0) return false;
      len_ += got;
    }
    return true;
  }

  std::FILE* f_;
  std::vector<unsigned char> buf_;
  std::size_t pos_ = 0, len_ = 0;
};

struct FileCloser {
  void operator()(std::FILE* f) const { if (f) std::fclose(f); }
};

// Walks every message, checking lengths; `fn(type, msg)` sees each well-formed one.
template <class Fn>
bool for_each_message(const std::string& path, std::string* error, Fn&& fn) {
  std::unique_ptr<std::FILE, FileCloser> f(std::fopen(path.c_str(), "rb"));
  if (!f) return fail(error, "cannot open " + path);
  MessageReader reader(f.get());
  const unsigned char* msg = nullptr;
  std::size_t length = 0;
  std::uint64_t index = 0;
  for (;;) {
    const int r = reader.next(msg, length);
    if (r == 0) return true;
    if (r < 0) return fail(error, "truncated message " + std::to_string(index));
    if (length == 0 || length < required_length(msg[0]))  // length 0 is checked first: msg[0]
      return fail(error, "message " + std::to_string(index) + " is too short for its type");
    if (!fn(msg, length)) return true;  // caller asked to stop
    ++index;
  }
}

std::string padded_symbol(const std::string& symbol) {
  std::string s = symbol.substr(0, 8);
  s.resize(8, ' ');
  return s;
}

std::string trimmed(const unsigned char* p) {
  std::string s(reinterpret_cast<const char*>(p), 8);
  s.erase(s.find_last_not_of(' ') + 1);
  return s;
}

}  // namespace

bool convert_itch(const std::string& path, const ItchOptions& options, std::vector<Event>& out,
                  ItchStats& stats, std::string* error) {
  out.clear();
  stats = {};
  const std::string symbol = padded_symbol(options.symbol);
  if (options.tick == 0) return fail(error, "tick must be at least 1");
  std::string tick_error;  // strict mode: set (and conversion stopped) at the first off-grid price
  // False if `itch_price` is off the tick grid. In strict mode that is an error; with
  // drop_off_grid the caller drops the order instead.
  const auto to_ticks = [&](std::uint64_t itch_price, lob::Price& ticks) {
    if (itch_price % options.tick != 0) {
      if (!options.drop_off_grid)
        tick_error = "price " + std::to_string(itch_price) + " (message " +
                     std::to_string(stats.messages - 1) + ") is not a multiple of tick " +
                     std::to_string(options.tick);
      return false;
    }
    ticks = static_cast<lob::Price>(itch_price / options.tick);
    return true;
  };

  struct Live {
    lob::OrderId id;
    lob::Price price;
    lob::Qty remaining;
    lob::Side side;
  };
  std::unordered_map<std::uint64_t, Live> live;         // ITCH reference -> order state
  std::unordered_map<std::uint64_t, lob::Side> dropped;  // drop_off_grid: references skipped

  const auto emit = [&](EventType type, lob::Timestamp ts, lob::OrderId id, lob::Side side,
                        lob::Price price, lob::Qty qty) {
    Event e{};
    e.ts = ts;
    e.id = id;
    e.type = type;
    if (type == EventType::New || type == EventType::Modify) {
      e.side = type == EventType::New ? side : lob::Side::Buy;
      e.price = price;
      e.otype = lob::OrderType::Limit;
    }
    if (type != EventType::Cancel) e.qty = qty;
    out.push_back(e);
  };
  // E/C (Execute) and X (Reduce): take shares off an order (never more than it has left).
  const auto reduce = [&](EventType type, std::uint64_t ref, lob::Qty shares, lob::Timestamp ts) {
    auto it = live.find(ref);
    if (it == live.end()) {
      if (dropped.count(ref) == 0) ++stats.unknown_refs;
      return;
    }
    shares = std::min(shares, it->second.remaining);
    emit(type, ts, it->second.id, lob::Side::Buy, 0, shares);
    it->second.remaining -= shares;
    if (it->second.remaining == 0) live.erase(it);
  };

  const bool ok = for_each_message(path, error, [&](const unsigned char* m, std::size_t) {
    ++stats.messages;
    const unsigned char type = m[0];
    if (type == 'R' && stats.locate == 0 && std::memcmp(m + 11, symbol.data(), 8) == 0)
      stats.locate = static_cast<std::uint16_t>(be(m + 1, 2));
    if (stats.locate == 0 || be(m + 1, 2) != stats.locate) return true;
    ++stats.symbol_messages;
    const lob::Timestamp ts = be(m + 5, 6);

    switch (type) {
      case 'A':
      case 'F': {
        const std::uint64_t ref = be(m + 11, 8);
        const lob::Side side = m[19] == 'B' ? lob::Side::Buy : lob::Side::Sell;
        const auto shares = static_cast<lob::Qty>(be(m + 20, 4));
        ++stats.adds;
        lob::Price price = 0;
        if (!to_ticks(be(m + 32, 4), price)) {
          if (!tick_error.empty()) return false;
          ++stats.off_grid_orders;
          dropped[ref] = side;
          break;
        }
        if (shares == 0) break;
        live[ref] = Live{ref, price, shares, side};
        emit(EventType::New, ts, ref, side, price, shares);
        break;
      }
      case 'E':
      case 'C':
        ++stats.executions;
        reduce(EventType::Execute, be(m + 11, 8), static_cast<lob::Qty>(be(m + 19, 4)), ts);
        break;
      case 'X':
        ++stats.cancels;
        reduce(EventType::Reduce, be(m + 11, 8), static_cast<lob::Qty>(be(m + 19, 4)), ts);
        break;
      case 'D': {
        ++stats.deletes;
        const std::uint64_t ref = be(m + 11, 8);
        auto it = live.find(ref);
        if (it == live.end()) {
          if (dropped.erase(ref) == 0) ++stats.unknown_refs;
          break;
        }
        emit(EventType::Cancel, ts, it->second.id, lob::Side::Buy, 0, 0);
        live.erase(it);
        break;
      }
      case 'U': {
        ++stats.replaces;
        const std::uint64_t ref = be(m + 11, 8);
        const std::uint64_t new_ref = be(m + 19, 8);
        const auto shares = static_cast<lob::Qty>(be(m + 27, 4));
        lob::Price price = 0;
        const bool on_grid = to_ticks(be(m + 31, 4), price);
        if (!tick_error.empty()) return false;
        auto it = live.find(ref);
        if (it == live.end()) {
          const auto d = dropped.find(ref);
          if (d == dropped.end()) {
            ++stats.unknown_refs;
            break;
          }
          const lob::Side side = d->second;
          dropped.erase(d);
          if (!on_grid) {
            dropped[new_ref] = side;  // still off the grid: stays dropped
          } else if (shares != 0) {  // moved onto the grid: it enters the book now
            live[new_ref] = Live{ref, price, shares, side};
            emit(EventType::New, ts, ref, side, price, shares);
          }
          break;
        }
        Live order = it->second;
        live.erase(it);
        if (!on_grid) {  // moved off the grid: leaves the book
          ++stats.off_grid_orders;
          dropped[new_ref] = order.side;
          emit(EventType::Cancel, ts, order.id, lob::Side::Buy, 0, 0);
          break;
        }
        if (shares == 0) {
          emit(EventType::Cancel, ts, order.id, lob::Side::Buy, 0, 0);
          break;
        }
        if (price != order.price || shares > order.remaining) {
          emit(EventType::Modify, ts, order.id, lob::Side::Buy, price, shares);  // re-queues
        } else {
          emit(EventType::Cancel, ts, order.id, lob::Side::Buy, 0, 0);  // same price, fewer or
          emit(EventType::New, ts, order.id, order.side, price, shares);  // equal: lose priority
        }
        live[new_ref] = Live{order.id, price, shares, order.side};
        break;
      }
      default:
        break;
    }
    return options.max_events == 0 || out.size() < options.max_events;
  });
  if (!ok) return false;
  if (!tick_error.empty()) {
    out.clear();
    return fail(error, tick_error);
  }
  if (stats.locate == 0) return fail(error, "symbol " + options.symbol + " not in the stock directory");
  return true;
}

bool itch_add_counts(const std::string& path,
                     std::vector<std::pair<std::string, std::uint64_t>>& out,
                     std::string* error) {
  out.clear();
  std::vector<std::string> names(65536);
  std::vector<std::uint64_t> counts(65536, 0);
  const bool ok = for_each_message(path, error, [&](const unsigned char* m, std::size_t) {
    const auto locate = static_cast<std::size_t>(be(m + 1, 2));
    if (m[0] == 'R') names[locate] = trimmed(m + 11);
    else if (m[0] == 'A' || m[0] == 'F') ++counts[locate];
    return true;
  });
  if (!ok) return false;
  for (std::size_t i = 0; i < counts.size(); ++i)
    if (counts[i] != 0) out.emplace_back(names[i].empty() ? "#" + std::to_string(i) : names[i], counts[i]);
  std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
    return a.second != b.second ? a.second > b.second : a.first < b.first;
  });
  return true;
}

}  // namespace replay
