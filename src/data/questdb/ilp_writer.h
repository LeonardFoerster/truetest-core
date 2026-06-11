#pragma once
#ifdef HAS_QUESTDB

#include "tcp_client.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace truetest::questdb {

// Builds a single InfluxDB Line Protocol row.
// Reference:
//   table_name,tag1=v1,tag2=v2 field1=x,field2="y" 1234567890000000000\n
// Rules:
//   - tags are dictionary-encoded (SYMBOL in QuestDB)
//   - field string values must be quoted
//   - field LONG values need a trailing 'i'
//   - field bool values are 't' / 'f'
//   - commas, spaces, and '=' in tags/keys must be backslash-escaped
//   - '"' and '\' inside string field values must be backslash-escaped
class LineBuilder
{
public:
    explicit LineBuilder(std::string_view table);

    LineBuilder& add_tag(std::string_view key, std::string_view value);
    LineBuilder& add_field_str(std::string_view key, std::string_view value);
    LineBuilder& add_field_double(std::string_view key, double value);
    LineBuilder& add_field_long(std::string_view key, std::int64_t value);
    LineBuilder& add_field_bool(std::string_view key, bool value);

    // Emits "...<sp><ns>\n". timestamp_ns is wall-clock nanoseconds.
    std::string finish(std::int64_t timestamp_ns);

private:
    std::string line_;
    bool in_fields_ = false;

    static void escape_into(std::string_view s, std::string& out, bool is_tag);
    static void escape_string_value(std::string_view s, std::string& out);
};

// Transport seam for IlpWriter. Production wraps a TcpClient; tests inject
// a fake.
class IIlpTransport
{
public:
    virtual ~IIlpTransport() = default;
    virtual bool connect(const std::string& host, std::uint16_t port) = 0;
    virtual bool write_all(std::string_view data) = 0;
    virtual void close() = 0;
    virtual bool is_connected() const = 0;
};

// Persistent ILP connection with write-buffering + reconnect-on-failure.
class IlpWriter
{
public:
    IlpWriter(std::string host, std::uint16_t port,
              std::size_t flush_every_n_lines = 1000,
              std::chrono::milliseconds flush_every = std::chrono::milliseconds(50));

    // Test-only constructor: inject a custom transport.
    IlpWriter(std::string host, std::uint16_t port,
              std::unique_ptr<IIlpTransport> transport,
              std::size_t flush_every_n_lines = 1000,
              std::chrono::milliseconds flush_every = std::chrono::milliseconds(50));

    ~IlpWriter() = default;

    // Initial connect attempt. Returns false on failure.
    bool connect();

    // Append a pre-built line (must end with '\n'). Flushes the buffer if
    // the threshold is reached. Safe to call after a connection drop -
    // the buffer accumulates and a reconnect is attempted on the next
    // flush.
    void enqueue(std::string line);

    // Force-flush. Returns false if the socket write or reconnect failed;
    // the buffer is retained for the next attempt.
    bool flush();

    std::size_t pending_lines() const { return buffer_count_; }
    std::size_t dropped_lines() const { return dropped_; }

    // Time-based flush honour. Calls flush() if last flush was longer than
    // flush_every_ ago AND there is buffered content. Cheap to call often.
    void maybe_time_flush();

private:
    std::string host_;
    std::uint16_t port_;
    std::size_t flush_every_n_lines_;
    std::chrono::milliseconds flush_every_;
    std::unique_ptr<IIlpTransport> tcp_;
    std::string buffer_;
    std::size_t buffer_count_ = 0;
    std::size_t dropped_ = 0;
    std::chrono::steady_clock::time_point last_flush_;
    int consecutive_failures_ = 0;

    bool ensure_connected();
};

}

#endif // HAS_QUESTDB
