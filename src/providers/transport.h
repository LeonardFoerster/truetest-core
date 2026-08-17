#pragma once

#include <chrono>
#include <string>
#include <string_view>
#include <optional>

enum class transport_terminal_status
{
	unknown,
	clean_eof,
	operator_stop,
	failed
};

enum class transport_read_result
{
	frame,
	idle,
	terminal
};

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
	virtual transport_terminal_status terminal_status() const
	{
		return transport_terminal_status::unknown;
	}

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

	// Optional bounded-silence read used by DataBridge control callbacks. A
	// transport advertising support must return `idle` at the deadline without
	// closing/reconnecting the venue stream. The default preserves legacy
	// blocking behavior and is rejected when a live funding ingress requires
	// idle draining.
	virtual bool supports_bounded_idle_read() const { return false; }
	virtual transport_read_result read_frame_until(
		std::string_view& out,
		std::chrono::steady_clock::time_point /*deadline*/)
	{
		return read_frame_blocking(out)
			? transport_read_result::frame
			: transport_read_result::terminal;
	}

protected:
	std::string frame_buf_;
};
