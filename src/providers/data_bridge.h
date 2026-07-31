#pragma once

#include "transport.h"
#include "parser.h"
#include "data/data_source.h"
#include "data/date_parse.h"
#include "data/market_series.h"
#include "data/market_sink.h"
#include "data/market_types.h"
#include "local/csv_parser.h"
#include "provider_event.h"
#include "provider_convert.h"

#include <atomic>
#include <memory>
#include <functional>
#include <iostream>
#include <type_traits>
#include <variant>

namespace detail {

// Emit a parsed record into IMarketSink (domain boundary).
inline bool emit_record(const bar_record& rec, IMarketSink& sink)
{
	Bar bar;
	bar.date = rec.date;
	bar.symbol = rec.symbol;
	bar.open = rec.open;
	bar.high = rec.high;
	bar.low = rec.low;
	bar.close = rec.close;
	bar.volume = rec.volume;
	// Prefer open_time (epoch ms) when present — Binance kline CSVs.
	if (rec.open_time_ms > 0)
	{
		bar.ts = std::chrono::system_clock::time_point{
			std::chrono::milliseconds{rec.open_time_ms}};
	}
	else if (auto tp = tt::date_parse::parse(rec.date))
	{
		bar.ts = *tp;
	}
	return sink.on_bar(bar);
}

inline bool emit_record(const tick_record& rec, IMarketSink& sink)
{
	return sink.on_tick(rec);
}

inline bool emit_record(const provider::event& ev, IMarketSink& sink)
{
	bool ok = false;
	std::visit([&](const auto& e) {
		using E = std::decay_t<decltype(e)>;
		if constexpr (std::is_same_v<E, provider::bar>)
		{
			ok = emit_record(provider::to_bar_record(e), sink);
		}
		else if constexpr (std::is_same_v<E, provider::tick>)
		{
			ok = sink.on_tick(provider::to_tick_record(e));
		}
		else
		{
			// L2 events are not series records
			ok = true;
		}
	}, ev);
	return ok;
}

} // namespace detail

// Null sink: used when streaming with retain_streamed=false (engine processes
// via on_record only; series must not grow unboundedly — docs/data.md D-06).
class DiscardMarketSink final : public IMarketSink
{
public:
	bool on_bar(const Bar&) override { return true; }
	bool on_tick(const Tick&) override { return true; }
};

template <typename T>
class DataBridge : public IDataSource
{
public:
	// Legacy sink_fn kept for callers that still capture shared_ptr handlers.
	using sink_fn = std::function<void(const T&, std::shared_ptr<data_handler>)>;
	using record_callback = std::function<void(const T&)>;

	DataBridge(
		std::shared_ptr<IDataTransport> transport,
		std::shared_ptr<IDataParser<T>> parser,
		sink_fn sink = nullptr
	)
		: transport_(std::move(transport))
		, parser_(std::move(parser))
		, sink_(std::move(sink))
	{ }

	void set_retain_streamed(bool retain) { retain_streamed_ = retain; }
	bool retain_streamed() const { return retain_streamed_; }

	bool load_data(std::shared_ptr<data_handler> handler) override
	{
		if (!handler) return false;
		return load_into(*handler, nullptr);
	}

	bool load_into(IMarketSink& sink, LoadStats* stats = nullptr) override
	{
		if (!transport_->open())
		{
			std::cerr << "DataBridge: transport failed to open\n";
			if (stats) stats->message = "transport failed to open";
			return false;
		}

		auto header_line = transport_->read_line();
		if (!header_line)
		{
			transport_->close();
			if (stats) stats->message = "empty transport";
			return false;
		}

		if (!parser_->parse_header(*header_line))
		{
			std::cerr << "DataBridge: failed to parse header\n";
			transport_->close();
			if (stats) stats->message = "failed to parse header";
			return false;
		}

		std::size_t count = 0;
		std::size_t rejected = 0;
		std::string_view frame;
		while (transport_->read_frame(frame))
		{
			if (auto record = parser_->parse_record(frame))
			{
				if (detail::emit_record(*record, sink))
					++count;
				else
					++rejected;
			}
		}

		transport_->close();
		if (stats)
		{
			stats->accepted = count;
			stats->rejected = rejected;
		}
		std::cout << "  DataBridge: loaded " << count << " records\n";
		return count > 0;
	}

	bool supports_stream() const override { return true; }

	bool stream_into(IMarketSink& sink,
	                 std::atomic<bool>* halt = nullptr,
	                 LoadStats* stats = nullptr) override
	{
		// Prefer explicit halt pointer; fall back to member set by engine.
		if (halt) halt_flag_ = halt;
		run_streaming_impl(sink, /*on_record=*/nullptr, stats);
		return true;
	}

	void set_halt_flag(std::atomic<bool>* halt) { halt_flag_ = halt; }

	// Engine streaming entry: optionally retain into handler (D-06).
	void run_streaming(
		std::shared_ptr<data_handler> handler,
		record_callback on_record = nullptr)
	{
		if (retain_streamed_ && handler)
		{
			run_streaming_impl(*handler, on_record, nullptr);
		}
		else
		{
			// Process via callback only; series stays empty (or untouched).
			DiscardMarketSink discard;
			// Still need to call legacy sink_ for L2-unrelated records if provided
			// when retain is false — engine uses on_record for process_single_*.
			run_streaming_impl(discard, on_record, nullptr);
		}
	}

	void stop()
	{
		if (transport_)
			transport_->request_stop();
	}

private:
	void run_streaming_impl(IMarketSink& sink,
	                        record_callback on_record,
	                        LoadStats* stats)
	{
		if (!transport_->open())
		{
			std::cerr << "DataBridge: transport failed to open\n";
			return;
		}

		std::size_t count = 0;
		std::string_view frame;

		auto handle = [&](const T& record) {
			detail::emit_record(record, sink);
			if (on_record)
				on_record(record);
			++count;
		};

		if (!transport_->is_streaming())
		{
			if (auto header_line = transport_->read_line())
			{
				parser_->parse_header(*header_line);
				if (auto record = parser_->parse_record(*header_line))
					handle(*record);
			}
		}
		else
		{
			if (!transport_->read_frame_blocking(frame))
			{
				transport_->close();
				return;
			}

			const std::string first_frame(frame);
			parser_->parse_header(first_frame);
			if (auto record = parser_->parse_record(first_frame))
				handle(*record);
		}

		while (transport_->is_open())
		{
			if (halt_flag_ && halt_flag_->load(std::memory_order_acquire))
				break;
			if (!transport_->read_frame_blocking(frame))
				break;
			if (halt_flag_ && halt_flag_->load(std::memory_order_acquire))
				break;

			if (auto record = parser_->parse_record(frame))
				handle(*record);
		}

		if (stats) stats->accepted = count;
		transport_->close();
	}

	std::shared_ptr<IDataTransport> transport_;
	std::shared_ptr<IDataParser<T>> parser_;
	sink_fn sink_;
	std::atomic<bool>* halt_flag_ = nullptr;
	bool retain_streamed_ = false; // D-06 default: do not grow series on stream
};
