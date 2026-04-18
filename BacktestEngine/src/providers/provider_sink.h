#pragma once

#include "provider_event.h"
#include "provider_convert.h"
#include "data/data_handler.h"
#include "../orderbook/orderbook.h"
#include "../types/price.h"

#include <memory>

namespace provider {

inline void event_sink(const event& ev, std::shared_ptr<data_handler> handler)
{
	std::visit([&](const auto& e) {
		using E = std::decay_t<decltype(e)>;

		if constexpr (std::is_same_v<E, bar>)
		{
			auto rec = to_bar_record(e);
			handler->load_into_queue(rec.date, rec.symbol, rec.open,
			                         rec.high, rec.low, rec.close, rec.volume);
		}
		else if constexpr (std::is_same_v<E, tick>)
		{
			handler->add_tick(to_tick_record(e));
		}
	}, ev);
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

} // namespace provider
