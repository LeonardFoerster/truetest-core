#pragma once

#include <string>
#include <string_view>
#include <optional>

class IDataTransport
{
public:
	virtual ~IDataTransport() = default;

	virtual bool open() = 0;
	virtual void close() = 0;
	virtual bool is_open() const = 0;

	virtual std::optional<std::string> read_line() = 0;


	virtual bool is_streaming() const { return false; }

	virtual std::optional<std::string> read_line_blocking()
	{
		return read_line();
	}

	virtual void request_stop() { close(); }

	virtual bool read_frame(std::string_view& out)
	{
		auto line = read_line();
		if (!line) return false;
		frame_buf_ = std::move(*line);
		out = frame_buf_;
		return true;
	}

	virtual bool read_frame_blocking(std::string_view& out)
	{
		auto line = read_line_blocking();
		if (!line) return false;
		frame_buf_ = std::move(*line);
		out = frame_buf_;
		return true;
	}

protected:
	std::string frame_buf_;
};
