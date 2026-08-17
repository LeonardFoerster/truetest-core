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
// via on_record only; series must not grow unboundedly — docs/internal/data-pipeline.md D-06).
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
	using idle_callback = std::function<void()>;

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

	// Optional research tap (footprint.md §2.1). Fires once per streamed
	// record, right after it is parsed, from the same thread that drives
	// run_streaming_impl - never a second producer thread. Default is unset
	// (nullptr check only, same cost as the existing on_record hook below).
	// Whatever is installed here owns the "must not allocate, lock, format,
	// log, retry, aggregate, or block" contract - DataBridge itself stays a
	// generic pass-through and knows nothing about footprint/PublicTrade
	// types, so this template does not gain a dependency on any one
	// consumer.
	using research_tap_fn = std::function<void(const T&)>;
	void set_research_tap(research_tap_fn tap) { research_tap_ = std::move(tap); }

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
			// Empty parse_records = unparseable/malformed frame (nullopt from
			// parse_record, or multi-record parser returned nothing). Count as
			// rejected so garbage CSV lines do not silently thin the sample
			// (DR-REPLAY-02). Emit failures (sink validation) also reject.
			auto records = parser_->parse_records(frame);
			if (records.empty())
			{
				++rejected;
				continue;
			}
			for (auto& record : records)
			{
				if (detail::emit_record(record, sink))
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
		std::cout << "  DataBridge: loaded " << count << " records";
		if (rejected > 0)
			std::cout << " (" << rejected << " rejected)";
		std::cout << "\n";
		return count > 0;
	}

	bool supports_stream() const override { return true; }

	StreamResult stream_into(IMarketSink& sink,
	                         std::atomic<bool>* halt = nullptr,
	                         LoadStats* stats = nullptr) override
	{
		// Prefer explicit halt pointer; fall back to member set by engine.
		if (halt) halt_flag_ = halt;
		return run_streaming_impl(
			sink, /*on_record=*/nullptr, /*on_idle=*/nullptr, stats);
	}

	void set_halt_flag(std::atomic<bool>* halt) { halt_flag_ = halt; }

	// Engine streaming entry: optionally retain into handler (D-06).
	StreamResult run_streaming(
		std::shared_ptr<data_handler> handler,
		record_callback on_record = nullptr,
		idle_callback on_idle = nullptr)
	{
		if (retain_streamed_ && handler)
		{
			return run_streaming_impl(*handler, on_record, on_idle, nullptr);
		}
		else
		{
			// Process via callback only; series stays empty (or untouched).
			DiscardMarketSink discard;
			// Still need to call legacy sink_ for L2-unrelated records if provided
			// when retain is false — engine uses on_record for process_single_*.
			return run_streaming_impl(discard, on_record, on_idle, nullptr);
		}
	}

	void stop()
	{
		stop_requested_.store(true, std::memory_order_release);
		if (transport_)
			transport_->request_stop();
	}

private:
		StreamResult run_streaming_impl(IMarketSink& sink,
		                        record_callback on_record,
		                        idle_callback on_idle,
		                        LoadStats* stats)
	{
		StreamResult result;
		std::size_t count = 0;
		struct TransportCloseGuard
		{
			std::shared_ptr<IDataTransport> transport;
			~TransportCloseGuard()
			{
				if (!transport) return;
				try { transport->close(); }
				catch (...) {}
			}
		} close_guard{transport_};

		try
		{
		if (halt_flag_ && halt_flag_->load(std::memory_order_acquire))
		{
			result.termination = stream_termination::engine_halt;
			return result;
		}
		if (stop_requested_.load(std::memory_order_acquire))
		{
			result.termination = stream_termination::operator_stop;
			return result;
		}
			if (on_idle && !transport_->supports_bounded_idle_read())
			{
				std::cerr << "DataBridge: funding/control idle drain requires a bounded transport read\n";
				result.termination = stream_termination::transport_open_failure;
				return result;
			}
			if (!transport_->open())
		{
			std::cerr << "DataBridge: transport failed to open\n";
			result.termination = stream_termination::transport_open_failure;
			return result;
		}
		if (halt_flag_ && halt_flag_->load(std::memory_order_acquire))
		{
			transport_->request_stop();
			result.termination = stream_termination::engine_halt;
			transport_->close();
			return result;
		}
		if (stop_requested_.load(std::memory_order_acquire))
		{
			transport_->request_stop();
			result.termination = stream_termination::operator_stop;
			transport_->close();
			return result;
		}

			std::string_view frame;
			auto read_next_frame = [&]() {
				for (;;)
				{
					if (halt_flag_ && halt_flag_->load(std::memory_order_acquire))
						return false;
					if (stop_requested_.load(std::memory_order_acquire))
						return false;
					if (!on_idle)
						return transport_->read_frame_blocking(frame);
					const auto read = transport_->read_frame_until(
						frame, std::chrono::steady_clock::now()
						       + std::chrono::milliseconds{100});
					if (read == transport_read_result::frame) return true;
					if (read == transport_read_result::terminal) return false;
					on_idle();
				}
			};

		bool record_failed = false;
		auto handle = [&](const T& record) {
			if (!detail::emit_record(record, sink))
			{
				++result.rejected;
				record_failed = true;
				return false;
			}
			if (on_record)
				on_record(record);
			if (research_tap_)
				 research_tap_(record);
			++count;
			return true;
		};

		if (!transport_->is_streaming())
		{
			if (auto header_line = transport_->read_line())
			{
				if (!parser_->parse_header(*header_line))
				{
					transport_->close();
					result.termination = stream_termination::header_failure;
					return result;
				}
				auto records = parser_->parse_records(*header_line);
				for (auto& record : records)
					if (!handle(record))
						break;
			}
			else
			{
				result.termination = classify_terminal();
			}
		}
		else
		{
			if (!read_next_frame())
			{
				result.termination = classify_terminal();
				transport_->close();
				return result;
			}

			const std::string first_frame(frame);
			if (!parser_->parse_header(first_frame))
			{
				transport_->close();
				result.termination = stream_termination::header_failure;
				return result;
			}
			if (parser_->header_frame_contains_records())
			{
				auto records = parser_->parse_records(first_frame);
				if (records.empty()
				    && parser_->classify_empty_frame(first_frame)
				       == empty_parse_status::malformed)
				{
					++result.rejected;
					record_failed = true;
				}
				for (auto& record : records)
					if (!handle(record))
						break;
			}
		}

		auto finish_result = [&] {
			result.accepted = count;
			if (stats) {
				stats->accepted = count;
				stats->rejected = result.rejected;
			}
		};

		while (!record_failed && transport_->is_open())
		{
			if (halt_flag_ && halt_flag_->load(std::memory_order_acquire))
			{
				break;
			}
			if (!read_next_frame())
			{
				break;
			}
			if (halt_flag_ && halt_flag_->load(std::memory_order_acquire))
				break;

			auto records = parser_->parse_records(frame);
			if (records.empty()
			    && parser_->classify_empty_frame(frame)
			       == empty_parse_status::malformed)
			{
				++result.rejected;
				record_failed = true;
				break;
			}
			for (auto& record : records)
				if (!handle(record))
					break;
		}

		finish_result();
		if (record_failed || result.rejected > 0)
			result.termination = stream_termination::parse_or_sink_failure;
		else if (halt_flag_ && halt_flag_->load(std::memory_order_acquire))
			result.termination = stream_termination::engine_halt;
		else if (stop_requested_.load(std::memory_order_acquire))
			result.termination = stream_termination::operator_stop;
		else
			result.termination = classify_terminal();
		transport_->close();
		return result;
		}
		catch (const std::exception& e)
		{
			std::cerr << "DataBridge: streaming exception: " << e.what() << "\n";
		}
		catch (...)
		{
			std::cerr << "DataBridge: unknown streaming exception\n";
		}
		result.accepted = count;
		if (stats)
		{
			stats->accepted = count;
			stats->rejected = result.rejected;
		}
		result.termination = stream_termination::runtime_failure;
		return result;
	}

	stream_termination classify_terminal() const
	{
		if (halt_flag_ && halt_flag_->load(std::memory_order_acquire))
			return stream_termination::engine_halt;
		if (stop_requested_.load(std::memory_order_acquire))
			return stream_termination::operator_stop;
		switch (transport_->terminal_status())
		{
		case transport_terminal_status::clean_eof:
			return stream_termination::clean_eof;
		case transport_terminal_status::operator_stop:
			return stream_termination::operator_stop;
		case transport_terminal_status::failed:
		case transport_terminal_status::unknown:
			return transport_->is_streaming()
				? stream_termination::transport_failure
				: stream_termination::clean_eof;
		}
		return stream_termination::transport_failure;
	}

	std::shared_ptr<IDataTransport> transport_;
	std::shared_ptr<IDataParser<T>> parser_;
	sink_fn sink_;
	research_tap_fn research_tap_;
	std::atomic<bool>* halt_flag_ = nullptr;
	std::atomic<bool> stop_requested_{false};
	bool retain_streamed_ = false; // D-06 default: do not grow series on stream
};
