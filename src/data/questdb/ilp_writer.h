#pragma once
#ifdef HAS_QUESTDB

#include <chrono>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>

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

// Result of one ILP write attempt. A delivery-ambiguous result means bytes
// reached the local TCP stack, but QuestDB persistence cannot be proven.
enum class IlpWriteOutcome
{
    complete,
    no_bytes_sent,
    delivery_ambiguous,
};

// Transport seam for IlpWriter. Production wraps a TcpClient; tests inject
// a fake.
class IIlpTransport
{
public:
    virtual ~IIlpTransport() = default;
    virtual bool connect(const std::string& host, std::uint16_t port) = 0;
    virtual bool write_all(std::string_view data) = 0;
    // Legacy/custom transports may only know success/failure. A false result
    // is deliberately conservative: never retry a possibly partial batch.
    virtual IlpWriteOutcome write_attempt(std::string_view data)
    {
        return write_all(data) ? IlpWriteOutcome::complete
                               : IlpWriteOutcome::delivery_ambiguous;
    }
    virtual void close() = 0;
    virtual bool is_connected() const = 0;
};

// Persistent ILP connection with write-buffering + reconnect-on-failure.
class IlpWriter
{
public:
    // Bounds retained ILP payload during a transport outage. The queue is
    // cold-path storage, but it must never become an unbounded RAM sink.
    static constexpr std::size_t kDefaultMaxPendingBytes = 4U * 1024U * 1024U;

    IlpWriter(std::string host, std::uint16_t port,
              std::size_t flush_every_n_lines = 1000,
              std::chrono::milliseconds flush_every = std::chrono::milliseconds(50),
              std::size_t max_pending_bytes = kDefaultMaxPendingBytes);

    // Test-only constructor: inject a custom transport.
    IlpWriter(std::string host, std::uint16_t port,
              std::unique_ptr<IIlpTransport> transport,
              std::size_t flush_every_n_lines = 1000,
              std::chrono::milliseconds flush_every = std::chrono::milliseconds(50),
              std::size_t max_pending_bytes = kDefaultMaxPendingBytes);

    ~IlpWriter() = default;

    // Initial connect attempt. Returns false on failure.
    bool connect();

    // Append a pre-built line (must end with '\n'). Flushes the buffer if
    // the threshold is reached. After a no-bytes-sent connection drop, the
    // bounded buffer accumulates and a reconnect is attempted on the next
    // flush. A delivery-ambiguous batch is quarantined instead. Returns false
    // only when the new line is refused at the configured pending-payload
    // limit or after such a quarantine.
    bool enqueue(std::string line);

    // Force-flush. Returns false if the socket write or reconnect failed.
    // A no-bytes-sent failure retains the buffer for the next attempt unless
    // a configured fallback accepts it; a delivery-ambiguous write retains it
    // without automatic replay.
    bool flush();

    std::size_t pending_lines() const { return buffer_count_; }
    std::size_t pending_bytes() const { return buffer_.size(); }
    std::size_t max_pending_bytes() const { return max_pending_bytes_; }
    std::size_t dropped_lines() const { return dropped_; }
    std::size_t fallback_lines() const { return fallback_lines_; }
    bool delivery_ambiguous() const { return delivery_ambiguous_; }

    // Phase 2: Enable writing raw ILP lines to a local fallback file on persistent failures.
    // The file is opened in append mode by the caller.
    void enable_fallback(std::unique_ptr<std::ostream> fallback_sink);

    bool is_connected() const { return tcp_ && tcp_->is_connected(); }
    bool failure_latched() const
    {
        return failure_latched_.load(std::memory_order_acquire);
    }
    void set_failure_callback(std::function<void()> callback)
    {
        failure_callback_ = std::move(callback);
    }

    std::chrono::steady_clock::time_point last_successful_flush() const {
        return last_flush_;
    }

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
    std::size_t max_pending_bytes_;
    std::size_t dropped_ = 0;
    bool delivery_ambiguous_ = false;
    std::chrono::steady_clock::time_point last_flush_;
    int consecutive_failures_ = 0;

    std::unique_ptr<std::ostream> fallback_sink_;  // Phase 2
    std::size_t fallback_lines_ = 0;
    std::atomic<bool> failure_latched_{false};
    std::function<void()> failure_callback_;

    bool ensure_connected();
    bool has_capacity_for(std::size_t bytes) const;
    bool make_room_for(std::size_t line_bytes);
    bool write_fallback();
    void latch_failure() noexcept;
};

}

#endif // HAS_QUESTDB
