#pragma once

#include "provider_event.h"
#include "provider_convert.h"
#include "data/data_handler.h"

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
			handler->tick_data.push_back(to_tick_record(e));
		}
		// l2_snapshot, l2_update, status: not stored in data_handler today.
		// Future: add l2 storage to data_handler or route directly to engine.
	}, ev);
}

} // namespace provider
