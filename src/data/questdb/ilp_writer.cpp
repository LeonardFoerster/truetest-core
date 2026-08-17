#ifdef HAS_QUESTDB

#include "ilp_writer.h"
#include "tcp_client.h"

#include <cmath>
#include <cstdio>
#include <iostream>
#include <utility>

namespace truetest::questdb {

// ── LineBuilder ─────────────────────────────────────────────────────────────

LineBuilder::LineBuilder(std::string_view table)
{
    line_.reserve(256);
    escape_into(table, line_, /*is_tag=*/true);
}

void LineBuilder::escape_into(std::string_view s, std::string& out, bool is_tag)
{
    for (char c : s)
    {
        if (c == ' ' || c == ',' || (is_tag && c == '='))
        {
            out.push_back('\\');
        }
        out.push_back(c);
    }
}

void LineBuilder::escape_string_value(std::string_view s, std::string& out)
{
    for (char c : s)
    {
        if (c == '"' || c == '\\') out.push_back('\\');
        out.push_back(c);
    }
}

LineBuilder& LineBuilder::add_tag(std::string_view key, std::string_view value)
{
    line_.push_back(',');
    escape_into(key, line_, /*is_tag=*/true);
    line_.push_back('=');
    escape_into(value, line_, /*is_tag=*/true);
    return *this;
}

LineBuilder& LineBuilder::add_field_str(std::string_view key, std::string_view value)
{
    line_.push_back(in_fields_ ? ',' : ' ');
    in_fields_ = true;
    escape_into(key, line_, /*is_tag=*/true);
    line_.push_back('=');
    line_.push_back('"');
    escape_string_value(value, line_);
    line_.push_back('"');
    return *this;
}

LineBuilder& LineBuilder::add_field_double(std::string_view key, double value)
{
    line_.push_back(in_fields_ ? ',' : ' ');
    in_fields_ = true;
    escape_into(key, line_, /*is_tag=*/true);
    line_.push_back('=');
    char buf[64];
    // %.17g preserves IEEE-754 round-trip precision.
    std::snprintf(buf, sizeof(buf), "%.17g", value);
    line_.append(buf);
    return *this;
}

LineBuilder& LineBuilder::add_field_long(std::string_view key, std::int64_t value)
{
    line_.push_back(in_fields_ ? ',' : ' ');
    in_fields_ = true;
    escape_into(key, line_, /*is_tag=*/true);
    line_.push_back('=');
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%lldi", static_cast<long long>(value));
    line_.append(buf);
    return *this;
}

LineBuilder& LineBuilder::add_field_bool(std::string_view key, bool value)
{
    line_.push_back(in_fields_ ? ',' : ' ');
    in_fields_ = true;
    escape_into(key, line_, /*is_tag=*/true);
    line_.push_back('=');
    line_.push_back(value ? 't' : 'f');
    return *this;
}

std::string LineBuilder::finish(std::int64_t timestamp_ns)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), " %lld\n", static_cast<long long>(timestamp_ns));
    line_.append(buf);
    return std::move(line_);
}

// ── Production transport: thin wrapper over TcpClient ───────────────────────

namespace {

class TcpTransport : public IIlpTransport
{
public:
    bool connect(const std::string& host, std::uint16_t port) override
    {
        return tcp_.connect(host, port);
    }
    bool write_all(std::string_view data) override
    {
        return tcp_.write_all(data, kWriteTimeoutMs);
    }
    IlpWriteOutcome write_attempt(std::string_view data) override
    {
        const auto result = tcp_.write_attempt(data, kWriteTimeoutMs);
        switch (result.state)
        {
        case TcpClient::WriteState::complete:
            return IlpWriteOutcome::complete;
        case TcpClient::WriteState::no_bytes_sent:
            return IlpWriteOutcome::no_bytes_sent;
        case TcpClient::WriteState::delivery_ambiguous:
            return IlpWriteOutcome::delivery_ambiguous;
        }
        return IlpWriteOutcome::delivery_ambiguous;
    }
    void close() override { tcp_.close(); }
    bool is_connected() const override { return tcp_.is_connected(); }

private:
    static constexpr int kWriteTimeoutMs = 500;
    TcpClient tcp_;
};

}

// ── IlpWriter ───────────────────────────────────────────────────────────────

IlpWriter::IlpWriter(std::string host, std::uint16_t port,
                     std::size_t flush_every_n_lines,
                     std::chrono::milliseconds flush_every,
                     std::size_t max_pending_bytes)
    : host_(std::move(host))
    , port_(port)
    , flush_every_n_lines_(flush_every_n_lines)
    , flush_every_(flush_every)
    , tcp_(std::make_unique<TcpTransport>())
    , max_pending_bytes_(max_pending_bytes)
    , last_flush_(std::chrono::steady_clock::now())
{}

IlpWriter::IlpWriter(std::string host, std::uint16_t port,
                     std::unique_ptr<IIlpTransport> transport,
                     std::size_t flush_every_n_lines,
                     std::chrono::milliseconds flush_every,
                     std::size_t max_pending_bytes)
    : host_(std::move(host))
    , port_(port)
    , flush_every_n_lines_(flush_every_n_lines)
    , flush_every_(flush_every)
    , tcp_(std::move(transport))
    , max_pending_bytes_(max_pending_bytes)
    , last_flush_(std::chrono::steady_clock::now())
{}

bool IlpWriter::connect()
{
    const bool ok = tcp_->connect(host_, port_);
    if (!ok) latch_failure();
    return ok;
}

void IlpWriter::latch_failure() noexcept
{
    if (!failure_latched_.exchange(true, std::memory_order_acq_rel)
        && failure_callback_)
    {
        // Persistence diagnostics must not prevent close/fallback cleanup.
        // Strict mode also observes failure_latched() on its regular cadence.
        try
        {
            failure_callback_();
        }
        catch (...)
        {
        }
    }
}

bool IlpWriter::ensure_connected()
{
    if (tcp_->is_connected()) return true;
    return tcp_->connect(host_, port_);
}

bool IlpWriter::has_capacity_for(std::size_t bytes) const
{
    return max_pending_bytes_ > 0
        && bytes <= max_pending_bytes_
        && buffer_.size() <= max_pending_bytes_ - bytes;
}

bool IlpWriter::make_room_for(std::size_t line_bytes)
{
    if (!has_capacity_for(line_bytes))
    {
        // An oversized single line can never fit; preserve the current FIFO
        // batch instead of performing unrelated I/O before rejecting it.
        if (line_bytes > max_pending_bytes_) return false;

        // A full, connected batch may be delivered immediately. Once a
        // connection has already failed, retry cadence remains owned by
        // maybe_time_flush()/flush(), not each newly rejected record.
        if (is_connected())
        {
            (void)flush();
        }
        else if (fallback_sink_)
        {
            // At pressure, a healthy fallback can free FIFO space without
            // forcing a reconnect attempt for every incoming line.
            (void)write_fallback();
        }
    }
    return has_capacity_for(line_bytes);
}

bool IlpWriter::enqueue(std::string line)
{
    if (delivery_ambiguous_)
    {
        ++dropped_;
        return false;
    }
    if (!make_room_for(line.size()))
    {
        ++dropped_;
        latch_failure();
        return false;
    }
    buffer_.append(line);
    buffer_count_++;
    if (buffer_count_ >= flush_every_n_lines_)
    {
        flush();
    }
    return true;
}

bool IlpWriter::flush()
{
    if (delivery_ambiguous_) return false;
    if (buffer_.empty())
    {
        last_flush_ = std::chrono::steady_clock::now();
        return true;
    }

    if (!ensure_connected())
    {
        latch_failure();
        consecutive_failures_++;
        (void)write_fallback();
        return false;
    }

    const auto write = tcp_->write_attempt(buffer_);
    if (write != IlpWriteOutcome::complete)
    {
        // Drop the connection so the next flush reconnects.
        tcp_->close();
        consecutive_failures_++;

        if (write == IlpWriteOutcome::delivery_ambiguous)
        {
            delivery_ambiguous_ = true;
            latch_failure();
            std::cerr << "[questdb] ambiguous ILP delivery; automatic retry "
                         "and fallback replay disabled; reconcile the binary event log\n";
            return false;
        }

        latch_failure();
        (void)write_fallback();
        return false;
    }

    buffer_.clear();
    buffer_count_ = 0;
    consecutive_failures_ = 0;
    last_flush_ = std::chrono::steady_clock::now();
    return true;
}

bool IlpWriter::write_fallback()
{
    if (!fallback_sink_ || buffer_.empty()) return false;
    try
    {
        (*fallback_sink_) << buffer_;
        fallback_sink_->flush();
    }
    catch (...)
    {
        latch_failure();
        return false;
    }
    if (!fallback_sink_->good())
    {
        latch_failure();
        return false;
    }
    fallback_lines_ += buffer_count_;
    buffer_.clear();
    buffer_count_ = 0;
    return true;
}

void IlpWriter::enable_fallback(std::unique_ptr<std::ostream> fallback_sink)
{
    fallback_sink_ = std::move(fallback_sink);
    fallback_lines_ = 0;
}

void IlpWriter::maybe_time_flush()
{
    if (buffer_.empty()) return;
    const auto now = std::chrono::steady_clock::now();
    if (now - last_flush_ >= flush_every_) flush();
}

}

#endif // HAS_QUESTDB
