#pragma once

#include "provider_event.h"
#include "provider_convert.h"
#include "data/market_series.h"
#include "data/market_sink.h"
#include "data/date_parse.h"
#include "../orderbook/orderbook.h"
#include "../types/price.h"

#include <memory>
#include <type_traits>

namespace provider {

// Domain-first sink: provider events → IMarketSink (preferred).
inline void event_sink(const event& ev, IMarketSink& sink)
{
	std::visit([&](const auto& e) {
		using E = std::decay_t<decltype(e)>;

		if constexpr (std::is_same_v<E, bar>)
		{
			Bar b;
			b.date = e.date;
			b.symbol = e.symbol;
			b.open = e.open;
			b.high = e.high;
			b.low = e.low;
			b.close = e.close;
			b.volume = e.volume;
			if (auto tp = tt::date_parse::parse(e.date))
				b.ts = *tp;
			sink.on_bar(b);
		}
		else if constexpr (std::is_same_v<E, tick>)
		{
			sink.on_tick(to_tick_record(e));
		}
	}, ev);
}

// Legacy shared_ptr overload for existing DataBridge sink_fn call sites.
inline void event_sink(const event& ev, std::shared_ptr<data_handler> handler)
{
	if (!handler) return;
	event_sink(ev, static_cast<IMarketSink&>(*handler));
}

inline void event_sink_l2(const event& ev, std::shared_ptr<orderbook> ob)
{
	if (!ob) return;

	std::visit([&](const auto& e) {
		using E = std::decay_t<decltype(e)>;

		if constexpr (std::is_same_v<E, l2_snapshot>)
		{
			std::vector<std::pair<Price, quantity>> bids;
			bids.reserve(e.bids.size());
			for (const auto& lvl : e.bids)
				bids.emplace_back(Price::from_double(lvl.price),
				                  static_cast<quantity>(lvl.quantity));

			std::vector<std::pair<Price, quantity>> asks;
			asks.reserve(e.asks.size());
			for (const auto& lvl : e.asks)
				asks.emplace_back(Price::from_double(lvl.price),
				                  static_cast<quantity>(lvl.quantity));

			ob->apply_l2_snapshot(bids, asks);
		}
		else if constexpr (std::is_same_v<E, l2_update>)
		{
			side ob_side = (e.side == 0) ? side::buy : side::sell;
			ob->apply_l2_update(ob_side, Price::from_double(e.price),
			                    static_cast<quantity>(e.new_quantity));
		}
	}, ev);
}

}
