#pragma once
#ifdef HAS_BINANCE

// REST candle backfill parsing/encoding scaffold for Binance spot/futures.
// Production fetch remains explicitly unsupported until provider startup has
// a non-trading warmup barrier, a causal close watermark, and atomic REST/WS
// overlap reconciliation. Treating REST rows as closed prepend frames can
// otherwise create lookahead, duplicates, or gaps before live streaming.

#include "binance_rest_client.h"
#include "binance_kline_interval.h"
#include "../../data/data_handler.h"

#include <string>
#include <string_view>
#include <vector>
#include <cstdint>
#include <chrono>
#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <optional>
#include <stdexcept>

struct backfill_bar {
    int64_t open_time;
    int64_t close_time;
    double open, high, low, close;
    double volume;
};

namespace binance
{
inline bool backfill_bar_is_valid(const backfill_bar& b) noexcept
{
    return b.open_time > 0 && b.close_time >= b.open_time
        && std::isfinite(b.open) && b.open > 0.0
        && std::isfinite(b.high) && b.high > 0.0
        && std::isfinite(b.low) && b.low > 0.0
        && std::isfinite(b.close) && b.close > 0.0
        && std::isfinite(b.volume) && b.volume >= 0.0
        && b.high >= std::max(b.open, b.close)
        && b.low <= std::min(b.open, b.close)
        && b.high >= b.low;
}

inline bool is_kline_wire_token(std::string_view value) noexcept
{
    // Both values are written into a fixed 1 KiB frame below. This bound also
    // keeps the later size_t-to-printf-precision conversion representable.
    constexpr std::size_t max_token_size = 128;
    if (value.empty() || value.size() > max_token_size) return false;
    for (const unsigned char c : value) {
        const bool ascii_alnum =
            (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
        if (!ascii_alnum && c != '_' && c != '-') return false;
    }
    return true;
}

inline std::optional<std::string> encode_backfill_kline_json(const backfill_bar& b,
                                                             std::string_view symbol,
                                                             std::string_view interval)
{
    if (!is_kline_wire_token(symbol) || !is_kline_wire_token(interval)
        || !backfill_bar_is_valid(b) || !(b.volume > 0.0)
        || !kline_times_match_fixed_interval(
            b.open_time, b.close_time, interval))
        return std::nullopt;

    // Binance's live payloads carry the venue-canonical upper-case symbol.
    // Backfill must use the same identity even when operator configuration
    // used the documented lower-case spelling, otherwise warmup and live
    // observations populate different per-symbol strategy/indicator states.
    std::string canonical_symbol(symbol);
    std::transform(canonical_symbol.begin(), canonical_symbol.end(), canonical_symbol.begin(),
                   [](const unsigned char c) {
                       return c >= 'a' && c <= 'z' ? static_cast<char>(c - ('a' - 'A'))
                                                   : static_cast<char>(c);
                   });

    char buf[1024];
    const int written = std::snprintf(
        buf, sizeof(buf),
        "{\"e\":\"kline\",\"E\":%lld,\"s\":\"%.*s\",\"k\":{"
        "\"t\":%lld,\"T\":%lld,\"s\":\"%.*s\",\"i\":\"%.*s\","
        "\"o\":\"%.8f\",\"c\":\"%.8f\",\"h\":\"%.8f\",\"l\":\"%.8f\","
        "\"v\":\"%.8f\",\"x\":true}}",
        static_cast<long long>(b.close_time), static_cast<int>(canonical_symbol.size()),
        canonical_symbol.data(), static_cast<long long>(b.open_time),
        static_cast<long long>(b.close_time), static_cast<int>(canonical_symbol.size()),
        canonical_symbol.data(), static_cast<int>(interval.size()), interval.data(), b.open,
        b.close, b.high, b.low, b.volume);
    if (written < 0 || static_cast<std::size_t>(written) >= sizeof(buf)) return std::nullopt;
    return std::string(buf, static_cast<std::size_t>(written));
}
} // namespace binance

class BinanceBackfill {
public:
    explicit BinanceBackfill(const std::string& host = "api.binance.com",
                             const std::string& port = "443",
                             const std::string& klines_path = "/api/v3/klines")
        : host_(host), port_(port), klines_path_(klines_path)
    {}

    std::vector<backfill_bar> fetch(
        const std::string& symbol,
        const std::string& interval = "1m",
        int count = 500,
        int64_t end_time_ms = 0) const
    {
        (void)symbol;
        (void)interval;
        (void)end_time_ms;
        if (count <= 0) return {};
        throw std::logic_error(
            "Binance candle backfill is unsupported until a non-trading "
            "warmup barrier and atomic REST/WS reconciliation are available");
    }

private:
    std::string host_;
    std::string port_;
    std::string klines_path_;

    std::vector<backfill_bar> fetch_batch(
        const std::string& symbol,
        const std::string& interval,
        int limit,
        int64_t end_time_ms) const
    {
        std::string query;
        binance::append_param(query, "symbol", to_upper(symbol));
        binance::append_param(query, "interval", interval);
        binance::append_param(query, "limit", std::to_string(limit));
        if (end_time_ms > 0) {
            binance::append_param(query, "endTime", std::to_string(end_time_ms));
        }

        namespace beast = boost::beast;
        namespace http = beast::http;
        namespace net = boost::asio;
        namespace ssl = net::ssl;
        using tcp = net::ip::tcp;

        net::io_context ioc;
        ssl::context ctx(ssl::context::tlsv12_client);
        ctx.set_default_verify_paths();
        ctx.set_verify_mode(ssl::verify_peer);

        ssl::stream<tcp::socket> stream(ioc, ctx);
        if (!provider_ws::configure_tls_peer_identity(
                stream.native_handle(), host_))
            return {};

        tcp::resolver resolver(ioc);
        auto results = resolver.resolve(host_, port_);
        net::connect(stream.next_layer(), results);
        stream.handshake(ssl::stream_base::client);

        http::request<http::string_body> req(
            http::verb::get, klines_path_ + "?" + query, 11);
        req.set(http::field::host, host_);
        req.set(http::field::user_agent, "TrueTest/1.0");

        http::write(stream, req);

        beast::flat_buffer buffer;
        http::response<http::string_body> res;
        http::read(stream, buffer, res);

        beast::error_code ec;
        stream.shutdown(ec);

        if (res.result() != http::status::ok) {
            return {};
        }

        return parse_klines_array(res.body());
    }

    std::vector<backfill_bar> parse_klines_array(const std::string& body) const
    {
        std::vector<backfill_bar> bars;

        std::size_t pos = 0;
        while (pos < body.size()) {
            pos = body.find('[', pos);
            if (pos == std::string::npos) break;
            if (body[pos + 1] == '[') { pos++; continue; }

            std::size_t end = body.find(']', pos);
            if (end == std::string::npos) break;

            std::string element = body.substr(pos + 1, end - pos - 1);
            pos = end + 1;

            backfill_bar bar{};
            if (parse_kline_element(element, bar)) {
                bars.push_back(bar);
            }
        }

        return bars;
    }

    bool parse_kline_element(const std::string& csv, backfill_bar& bar) const
    {
        std::vector<std::string> fields;
        std::size_t start = 0;
        bool in_quote = false;
        for (std::size_t i = 0; i <= csv.size(); ++i) {
            if (i < csv.size() && csv[i] == '"') { in_quote = !in_quote; continue; }
            if (i == csv.size() || (!in_quote && csv[i] == ',')) {
                std::string f = csv.substr(start, i - start);
                if (f.size() >= 2 && f.front() == '"' && f.back() == '"')
                    f = f.substr(1, f.size() - 2);
                fields.push_back(f);
                start = i + 1;
            }
        }

        if (fields.size() < 7) return false;

        const auto parse_i64 = [](const std::string& value,
                                  std::int64_t& out) noexcept {
            const auto [end, ec] = std::from_chars(
                value.data(), value.data() + value.size(), out);
            return ec == std::errc{} && end == value.data() + value.size();
        };
        const auto parse_double = [](const std::string& value,
                                     double& out) noexcept {
            const auto [end, ec] = std::from_chars(
                value.data(), value.data() + value.size(), out);
            return ec == std::errc{} && end == value.data() + value.size()
                && std::isfinite(out);
        };
        if (!parse_i64(fields[0], bar.open_time)
            || !parse_double(fields[1], bar.open)
            || !parse_double(fields[2], bar.high)
            || !parse_double(fields[3], bar.low)
            || !parse_double(fields[4], bar.close)
            || !parse_double(fields[5], bar.volume)
            || !parse_i64(fields[6], bar.close_time))
            return false;
        return binance::backfill_bar_is_valid(bar);
    }

    static std::string to_upper(std::string s)
    {
        for (auto& c : s) c = static_cast<char>(std::toupper(c));
        return s;
    }
};

#endif // HAS_BINANCE
