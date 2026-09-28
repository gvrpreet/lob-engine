#include "replay/event_stream.hpp"

#include <algorithm>
#include <chrono>
#include <utility>

#include "amend.hpp"
#include "replay/event_log.hpp"

#if defined(__unix__)
#include <fcntl.h>
#endif

namespace replay {

namespace {
bool fail(std::string* error, std::string reason) {
  if (error) *error = std::move(reason);
  return false;
}
}  // namespace

void EventLogStream::FileCloser::operator()(std::FILE* f) const noexcept {
  if (f) std::fclose(f);
}

EventLogStream::EventLogStream(std::size_t window_events)
    : window_(std::max<std::size_t>(window_events, 1)) {}

bool EventLogStream::open(const std::string& path, std::string* error) {
  file_.reset(std::fopen(path.c_str(), "rb"));
  count_ = consumed_ = 0;
  version_ = 0;
  if (!file_) return fail(error, "cannot open " + path);
  std::FILE* f = file_.get();
  if (std::fseek(f, 0, SEEK_END) != 0) return fail(error, "cannot seek");
  const long end = std::ftell(f);
  if (end < 0) return fail(error, "cannot determine file size");
  std::rewind(f);
  char header[kEventLogHeaderBytes] = {};
  if (std::fread(header, 1, sizeof header, f) != sizeof header &&
      static_cast<std::uint64_t>(end) >= kEventLogHeaderBytes)
    return fail(error, "short read");  // a genuinely short file fails the size check below instead
  if (!check_event_log_header(header, static_cast<std::uint64_t>(end), count_, version_, error)) {
    file_.reset();
    return false;
  }
#if defined(__unix__) && defined(POSIX_FADV_SEQUENTIAL)
  (void)posix_fadvise(fileno(f), 0, 0, POSIX_FADV_SEQUENTIAL);  // a hint: larger read-ahead
#endif
  return true;
}

bool EventLogStream::next(std::span<const Event>& chunk, std::string* error) {
  chunk = {};
  if (!file_) return fail(error, "stream is not open");
  const auto n = static_cast<std::size_t>(
      std::min<std::uint64_t>(window_.size(), count_ - consumed_));
  if (n == 0) return true;
  if (std::fread(window_.data(), sizeof(Event), n, file_.get()) != n)
    return fail(error, "short read");
  const std::span<const Event> records(window_.data(), n);
  if (const std::size_t bad = first_invalid_event(records, version_); bad != n) {
    return fail(error, "record " + std::to_string(consumed_ + bad) +
                           " has an invalid type/side/order-type or reserved byte");
  }
  consumed_ += n;
  chunk = records;
  return true;
}

bool replay_stream(EventLogStream& stream, const ReplayConfig& cfg, ReplayStats& st,
                   IStrategy* strat, std::string* error) {
  st = {};
  lob::OrderBook book(cfg.book_capacity);
  const auto sink = [&](const lob::Trade& trade) {
    ++st.trades;
    st.volume += trade.qty;
    if (strat) strat->on_trade(trade);
  };

  bool ok = true;
  const auto t0 = std::chrono::steady_clock::now();
  for (;;) {
    std::span<const Event> chunk;
    if (!stream.next(chunk, error)) {
      ok = false;
      break;
    }
    if (chunk.empty()) break;
    for (const Event& event : chunk) {
      switch (event.type) {
        case EventType::New:
          if (book.add(event.id, event.side, event.otype, event.price, event.qty, event.ts, sink) ==
              lob::AddResult::Rejected)
            ++st.rejected;
          break;
        case EventType::Cancel:
          if (!book.cancel(event.id)) ++st.cancel_misses;
          break;
        case EventType::Modify:
        case EventType::Reduce:
          detail::apply_amend(book, event, st, strat);
          break;
      }
      if (strat) strat->on_event(event, book);
      ++st.events;
    }
  }
  st.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  st.final_orders = book.order_count();
  st.checksum = book.checksum();
  return ok;
}

}  // namespace replay
