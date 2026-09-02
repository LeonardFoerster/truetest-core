#pragma once

// IDataSource — legacy batch load into shared_ptr<data_handler>.
// Prefer IMarketSource (market_source.h). Shim kept for one migration cycle.

#include "data/market_source.h"
#include "data/market_series.h"

#include <memory>

// data_handler is a typedef for MarketSeries (market_series.h). Do not
// forward-declare it as `class data_handler`.

class IDataSource : public IMarketSource
{
public:
	~IDataSource() override = default;

	// Legacy entry point used by CLI / TUI / API.
	virtual bool load_data(std::shared_ptr<data_handler> handler) = 0;

	// IMarketSource default: if sink is a MarketSeries, route via load_data.
	// This compatibility path receives the real series; implementations used
	// through DataWrapper must therefore honor IMarketSource's append-only
	// contract even though legacy load_data exposes the concrete type.
	bool load_into(IMarketSink& sink, LoadStats* stats = nullptr) override
	{
		auto* series = dynamic_cast<MarketSeries*>(&sink);
		if (!series)
		{
			if (stats) stats->message = "IDataSource shim requires MarketSeries sink";
			return false;
		}
		const auto checkpoint = series->append_checkpoint();
		// Non-owning shared_ptr alias so legacy load_data can accept it.
		auto sp = std::shared_ptr<data_handler>(series, [](data_handler*) {});
		const bool ok = load_data(sp);
		if (stats)
		{
			stats->accepted =
				(series->bar_count() >= checkpoint.bar_count
				 && series->tick_count() >= checkpoint.tick_count)
				? (series->bar_count() - checkpoint.bar_count)
				  + (series->tick_count() - checkpoint.tick_count)
				: 0;
			stats->rejected =
				series->validation_errors() >= checkpoint.validation_errors
				? series->validation_errors() - checkpoint.validation_errors
				: 0;
			if (!ok && stats->message.empty())
				stats->message = "load_data failed";
		}
		return ok;
	}
};
