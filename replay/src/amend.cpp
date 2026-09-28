#include "amend.hpp"

namespace replay::detail {

template <class Book>
void apply_amend(Book& book, const Event& event, ReplayStats& st, IStrategy* strat) {
  if (event.type == EventType::Modify) {
    const auto sink = [&](const lob::Trade& trade) {  // same accounting as the replay loop's sink
      ++st.trades;
      st.volume += trade.qty;
      if (strat) strat->on_trade(trade);
    };
    const auto r = book.modify(event.id, event.price, event.qty, event.ts, sink);
    if (r == lob::ModifyResult::NotFound) ++st.modify_misses;
    else if (r == lob::ModifyResult::Rejected) ++st.rejected;
  } else if (!book.reduce(event.id, event.qty)) {
    ++st.reduce_misses;
  }
}

template void apply_amend(lob::OrderBook&, const Event&, ReplayStats&, IStrategy*);
template void apply_amend(lob::NaiveOrderBook&, const Event&, ReplayStats&, IStrategy*);
template void apply_amend(lob::OrderBook48&, const Event&, ReplayStats&, IStrategy*);
template void apply_amend(lob::OrderBook64&, const Event&, ReplayStats&, IStrategy*);
template void apply_amend(lob::HotColdOrderBook&, const Event&, ReplayStats&, IStrategy*);
template void apply_amend(lob::DenseOrderBook&, const Event&, ReplayStats&, IStrategy*);
template void apply_amend(lob::SortedOrderBook&, const Event&, ReplayStats&, IStrategy*);
template void apply_amend(lob::HugePageOrderBook&, const Event&, ReplayStats&, IStrategy*);

}  // namespace replay::detail
