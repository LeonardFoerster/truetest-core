#ifdef HAS_QUESTDB

#include "ilp_writer.h"

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
        return tcp_.write_all(data);
    }
    void close() override { tcp_.close(); }
    bool is_connected() const override { return tcp_.is_connected(); }

private:
    TcpClient tcp_;
};

}

// ── IlpWriter ───────────────────────────────────────────────────────────────

IlpWriter::IlpWriter(std::string host, std::uint16_t port,
                     std::size_t flush_every_n_lines,
                     std::chrono::milliseconds flush_every)
    : host_(std::move(host))
    , port_(port)
    , flush_every_n_lines_(flush_every_n_lines)
    , flush_every_(flush_every)
    , tcp_(std::make_unique<TcpTransport>())
    , last_flush_(std::chrono::steady_clock::now())
{}

IlpWriter::IlpWriter(std::string host, std::uint16_t port,
                     std::unique_ptr<IIlpTransport> transport,
                     std::size_t flush_every_n_lines,
                     std::chrono::milliseconds flush_every)
    : host_(std::move(host))
    , port_(port)
    , flush_every_n_lines_(flush_every_n_lines)
    , flush_every_(flush_every)
    , tcp_(std::move(transport))
    , last_flush_(std::chrono::steady_clock::now())
{}

bool IlpWriter::connect()
{
    return tcp_->connect(host_, port_);
}

bool IlpWriter::ensure_connected()
{
    if (tcp_->is_connected()) return true;
    return tcp_->connect(host_, port_);
}

void IlpWriter::enqueue(std::string line)
{
    buffer_.append(line);
    buffer_count_++;
    if (buffer_count_ >= flush_every_n_lines_)
    {
        flush();
    }
}

bool IlpWriter::flush()
{
    if (buffer_.empty())
    {
        last_flush_ = std::chrono::steady_clock::now();
        return true;
    }

    if (!ensure_connected())
    {
        consecutive_failures_++;
        if (fallback_sink_ && !buffer_.empty())
        {
            (*fallback_sink_) << buffer_;
            fallback_lines_ += buffer_count_;
            buffer_.clear();
            buffer_count_ = 0;
        }
        return false;
    }

    if (!tcp_->write_all(buffer_))
    {
        // Drop the connection so the next flush reconnects.
        tcp_->close();
        consecutive_failures_++;

        if (fallback_sink_ && !buffer_.empty())
        {
            (*fallback_sink_) << buffer_;
            fallback_lines_ += buffer_count_;
            buffer_.clear();
            buffer_count_ = 0;
        }
        return false;
    }

    buffer_.clear();
    buffer_count_ = 0;
    consecutive_failures_ = 0;
    last_flush_ = std::chrono::steady_clock::now();
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
