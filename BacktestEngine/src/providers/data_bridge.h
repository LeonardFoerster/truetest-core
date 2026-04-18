#pragma once

#include "transport.h"
#include "parser.h"
#include "data/data_source.h"
#include "data/data_handler.h"

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

	void run_streaming(
		std::shared_ptr<data_handler> handler,
		record_callback on_record = nullptr)
	{
		if (!transport_->open())
		{
			std::cerr << "DataBridge: transport failed to open\n";
			return;
		}

		if (!transport_->is_streaming())
		{
			if (auto header_line = transport_->read_line())
				parser_->parse_header(*header_line);
		}

		std::string_view frame;
		while (transport_->is_open())
		{
			if (!transport_->read_frame_blocking(frame))
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
};
