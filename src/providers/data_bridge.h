#pragma once

#include "transport.h"
#include "parser.h"
#include "data/data_source.h"
#include "data/data_handler.h"

#include <atomic>
#include <memory>
#include <functional>
#include <iostream>

template <typename T>
class DataBridge : public IDataSource
{
public:
	using sink_fn = std::function<void(const T&, std::shared_ptr<data_handler>)>;

	DataBridge(
		std::shared_ptr<IDataTransport> transport,
		std::shared_ptr<IDataParser<T>> parser,
		sink_fn sink
	)
		: transport_(std::move(transport))
		, parser_(std::move(parser))
		, sink_(std::move(sink))
	{ }

	bool load_data(std::shared_ptr<data_handler> handler) override
	{
		if (!transport_->open())
		{
			std::cerr << "DataBridge: transport failed to open\n";
			return false;
		}

		auto header_line = transport_->read_line();
		if (!header_line)
		{
			transport_->close();
			return false;
		}

		if (!parser_->parse_header(*header_line))
		{
			std::cerr << "DataBridge: failed to parse header\n";
			transport_->close();
			return false;
		}

		size_t count = 0;
		std::string_view frame;
		while (transport_->read_frame(frame))
		{
			if (auto record = parser_->parse_record(frame))
			{
				sink_(*record, handler);
				++count;
			}
		}

		transport_->close();
		std::cout << "  DataBridge: loaded " << count << " records\n";
		return count > 0;
	}

	using record_callback = std::function<void(const T&)>;

	// Engine wires its halt_flag_ here in live mode so a fatal transport
	// disconnect drops the loop on the next iteration instead of waiting
	// for the transport's read to return. Pre-existing callers that pass
	// no halt flag keep the original "loop until is_open() goes false"
	// behaviour.
	void set_halt_flag(std::atomic<bool>* halt) { halt_flag_ = halt; }

	void run_streaming(
		std::shared_ptr<data_handler> handler,
		record_callback on_record = nullptr)
	{
		if (!transport_->open())
		{
			std::cerr << "DataBridge: transport failed to open\n";
			return;
		}

		std::string_view frame;
		if (!transport_->is_streaming())
		{
			if (auto header_line = transport_->read_line())
			{
				parser_->parse_header(*header_line);
				if (auto record = parser_->parse_record(*header_line))
				{
					sink_(*record, handler);
					if (on_record)
						on_record(*record);
				}
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
			{
				sink_(*record, handler);
				if (on_record)
					on_record(*record);
			}
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
			{
				sink_(*record, handler);
				if (on_record)
					on_record(*record);
			}
		}

		transport_->close();
	}

	void stop()
	{
		if (transport_)
			transport_->request_stop();
	}

private:
	std::shared_ptr<IDataTransport> transport_;
	std::shared_ptr<IDataParser<T>> parser_;
	sink_fn sink_;
	std::atomic<bool>* halt_flag_ = nullptr;
};
