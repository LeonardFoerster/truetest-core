#pragma once

#include "provider_event.h"
#include "provider_convert.h"
#include "data/market_series.h"
#include "data/market_sink.h"
#include "data/date_parse.h"
#include "data/quantity_scale.h"
#include "../orderbook/orderbook.h"
#include "../types/price.h"

#include <memory>
#include <cmath>
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
			b.quantity_scale = e.quantity_scale;
			const auto tp = tt::date_parse::parse(e.date);
			if (!tp) return;
			b.ts = *tp;
			(void)sink.on_bar(b);
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
			if (e.symbol.empty()) return;
			std::vector<std::pair<Price, quantity>> bids;
			bids.reserve(e.bids.size());
			for (const auto& lvl : e.bids)
			{
				Price price;
				if (lvl.price <= 0.0
				    || !Price::try_from_double(lvl.price, price))
					return;
				std::uint64_t qty = 0;
				if (!tt::quantity_scale::rescale_nonnegative(
				        lvl.quantity, e.quantity_scale,
				        static_cast<double>(tt::quantity_scale::canonical_atoms), qty))
					return;
				bids.emplace_back(price, qty);
			}

			std::vector<std::pair<Price, quantity>> asks;
			asks.reserve(e.asks.size());
			for (const auto& lvl : e.asks)
			{
				Price price;
				if (lvl.price <= 0.0
				    || !Price::try_from_double(lvl.price, price))
					return;
				std::uint64_t qty = 0;
				if (!tt::quantity_scale::rescale_nonnegative(
				        lvl.quantity, e.quantity_scale,
				        static_cast<double>(tt::quantity_scale::canonical_atoms), qty))
					return;
				asks.emplace_back(price, qty);
			}

			ob->apply_l2_snapshot(bids, asks);
		}
		else if constexpr (std::is_same_v<E, l2_update>)
		{
			Price price;
			if (e.symbol.empty() || (e.side != 0 && e.side != 1)
			    || e.price <= 0.0
			    || !Price::try_from_double(e.price, price))
				return;
			side ob_side = (e.side == 0) ? side::buy : side::sell;
			std::uint64_t qty = 0;
			if (tt::quantity_scale::rescale_nonnegative(
			        e.new_quantity, e.quantity_scale,
			        static_cast<double>(tt::quantity_scale::canonical_atoms), qty))
				ob->apply_l2_update(ob_side, price, qty);
		}
	}, ev);
}

}
