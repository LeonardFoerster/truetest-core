#pragma once

#include "transport.h"
#include "parser.h"
#include "data/data_source.h"
#include "data/data_handler.h"

#include <memory>
#include <functional>
#include <iostream>

// DataBridge: orchestrator that connects an IDataTransport to an IDataParser<T>
// and feeds parsed records into a data_handler via a user-supplied sink function.
//
// Implements IDataSource so it plugs directly into the existing engine wiring.
//
// Example:
//   auto transport = std::make_shared<FileTransport>("data.csv");
//   auto parser    = std::make_shared<CsvBarParser>();
//   auto bridge    = std::make_shared<DataBridge<BarRecord>>(transport, parser, bar_sink);
//   bridge->load_data(handler);
//
template <typename T>
class DataBridge : public IDataSource
{
public:
	// sink_fn receives each parsed record and the handler to populate.
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

		// Read and parse header (first line)
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

		// Read and parse data lines
		size_t count = 0;
		while (auto line = transport_->read_line())
		{
			if (auto record = parser_->parse_record(*line))
			{
				sink_(*record, handler);
				++count;
			}
		}

		transport_->close();
		std::cout << "  DataBridge: loaded " << count << " records\n";
		return count > 0;
	}

	// --- Streaming mode ---
	// Blocks the calling thread, continuously reading from the transport
	// and feeding parsed records into the handler via the sink function.
	// Returns when the transport closes, errors, or request_stop() is called.
	//
	// on_record is an optional per-record callback for real-time consumers
	// (e.g. the engine can process each record as it arrives).
	using record_callback = std::function<void(const T&)>;

	void run_streaming(
		std::shared_ptr<data_handler> handler,
		record_callback on_record = nullptr)
	{
		if (!transport_->open())
		{
			std::cerr << "DataBridge: transport failed to open\n";
			return;
		}

		// Parse header if the transport provides one
		if (auto header_line = transport_->read_line())
			parser_->parse_header(*header_line);

		while (transport_->is_open())
		{
			auto line = transport_->read_line_blocking();
			if (!line) break;  // transport closed or stopped

			if (auto record = parser_->parse_record(*line))
			{
				sink_(*record, handler);
				if (on_record)
					on_record(*record);
			}
		}

		transport_->close();
	}

	// Signal the bridge to stop streaming.
	void stop()
	{
		if (transport_)
			transport_->request_stop();
	}

private:
	std::shared_ptr<IDataTransport> transport_;
	std::shared_ptr<IDataParser<T>> parser_;
	sink_fn sink_;
};
