#pragma once

#include <string>
#include <optional>

// IDataTransport: handles *where* data comes from (files, sockets, DB cursors).
// Delivers raw lines/buffers to a parser without knowing the format.
class IDataTransport
{
public:
	virtual ~IDataTransport() = default;

	virtual bool open() = 0;
	virtual void close() = 0;
	virtual bool is_open() const = 0;

	// Returns the next line of raw data, or nullopt when exhausted.
	virtual std::optional<std::string> read_line() = 0;

	// --- Streaming support ---

	// Returns true if this transport delivers data continuously
	// (WebSocket, named pipe, etc.) vs. batch (file, DB query).
	// Default: false (batch).
	virtual bool is_streaming() const { return false; }

	// Blocking read: waits for the next line/message. Returns nullopt
	// only when the transport is closed or the connection is lost.
	// Default: falls back to read_line() for batch sources.
	virtual std::optional<std::string> read_line_blocking()
	{
		return read_line();
	}

	// Request graceful shutdown of a blocking read. After this call,
	// read_line_blocking() must return nullopt promptly.
	// Default: calls close().
	virtual void request_stop() { close(); }
};
