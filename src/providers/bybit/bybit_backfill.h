#pragma once
#ifdef HAS_BYBIT

// REST kline backfill → prepend frames accepted by BybitKlineParser /
// BybitCombinedParser (confirm:true closed candles).
// Cold path only. Pure parse helpers are unit-testable offline.

#include "providers/bybit/bybit_endpoints.h"
#include "providers/bybit/bybit_parser.h"
#include "providers/bybit/bybit_transport.h"

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace bybit {

struct backfill_bar
{
    int64_t open_time = 0; // ms
    double  open = 0;
    double  high = 0;
    double  low = 0;
    double  close = 0;
    double  volume = 0;
};

// Parse GET /v5/market/kline body.
// result.list[] = [start, open, high, low, close, volume, turnover] strings.
// Venue returns newest-first; we reverse to oldest→newest for prepend.
inline std::vector<backfill_bar> parse_kline_response(std::string_view body)
{
    std::vector<backfill_bar> out;
    if (body.empty()) return out;

    // Fail closed on non-zero retCode when present.
    auto rc_sv = extract_sv_number(body, "retCode");
    if (!rc_sv.empty())
    {
        int64_t rc = -1;
        if (!parse_int64_sv(rc_sv, rc) || rc != 0)
            return out;
    }

    // Prefer result.list; fall back to top-level list/data.
    auto result_obj = detail::extract_object(body, "result");
    std::string_view list_src = result_obj.empty() ? body : result_obj;
    auto list = detail::extract_array(list_src, "list");
    if (list.empty())
        list = detail::extract_array(list_src, "data");
    if (list.size() < 2 || list.front() != '[')
        return out;

    // list is an array of arrays: [ [start,o,h,l,c,v,turnover], ... ]
    std::size_t pos = 1;
    while (pos < list.size())
    {
        detail::skip_ws(list, pos);
        if (pos >= list.size() || list[pos] == ']')
            break;
        if (list[pos] == ',')
        {
            ++pos;
            continue;
        }
        if (list[pos] != '[')
            break;
        auto close = detail::match_container(list, pos);
        if (close == std::string_view::npos)
            break;
        std::string_view row = list.substr(pos + 1, close - pos - 1);
        pos = close + 1;

        // Split CSV-ish fields (quoted or bare).
        std::vector<std::string_view> fields;
        std::size_t s = 0;
        bool in_q = false;
        for (std::size_t i = 0; i <= row.size(); ++i)
        {
            if (i < row.size() && row[i] == '"')
            {
                in_q = !in_q;
                continue;
            }
            if (i == row.size() || (!in_q && row[i] == ','))
            {
                auto f = row.substr(s, i - s);
                while (!f.empty() && (f.front() == ' ' || f.front() == '\t'))
                    f.remove_prefix(1);
                while (!f.empty() && (f.back() == ' ' || f.back() == '\t'))
                    f.remove_suffix(1);
                if (f.size() >= 2 && f.front() == '"' && f.back() == '"')
                    f = f.substr(1, f.size() - 2);
                fields.push_back(f);
                s = i + 1;
            }
        }
        if (fields.size() < 5)
            continue;

        backfill_bar b{};
        if (!parse_int64_sv(fields[0], b.open_time))
            continue;
        if (!parse_double_sv(fields[1], b.open))
            continue;
        if (!parse_double_sv(fields[2], b.high))
            continue;
        if (!parse_double_sv(fields[3], b.low))
            continue;
        if (!parse_double_sv(fields[4], b.close))
            continue;
        if (fields.size() >= 6)
            parse_double_sv(fields[5], b.volume);
        out.push_back(b);
    }

    if (out.size() >= 2 && out.front().open_time > out.back().open_time)
        std::reverse(out.begin(), out.end());
    return out;
}

// Encode a closed kline frame accepted by BybitKlineParser (confirm:true).
inline std::string encode_kline_frame(const backfill_bar& b,
                                      std::string_view symbol,
                                      std::string_view interval)
{
    char buf[768];
    std::snprintf(
        buf, sizeof(buf),
        R"({"topic":"kline.%.*s.%.*s","type":"snapshot","ts":%lld,"data":[{"start":%lld,"open":"%.8f","high":"%.8f","low":"%.8f","close":"%.8f","volume":"%.8f","confirm":true,"interval":"%.*s"}]})",
        static_cast<int>(interval.size()), interval.data(),
        static_cast<int>(symbol.size()), symbol.data(),
        static_cast<long long>(b.open_time),
        static_cast<long long>(b.open_time),
        b.open, b.high, b.low, b.close, b.volume,
        static_cast<int>(interval.size()), interval.data());
    return std::string(buf);
}

inline std::string build_kline_query(std::string_view symbol,
                                     std::string_view interval,
                                     int limit,
                                     int64_t end_time_ms = 0)
{
    std::string q;
    q.reserve(96 + symbol.size() + interval.size());
    q.append("category=linear");
    q.append("&interval=");
    q.append(interval);
    q.append("&limit=");
    q.append(std::to_string(std::max(1, std::min(limit, 1000))));
    q.append("&symbol=");
    q.append(symbol);
    if (end_time_ms > 0)
    {
        q.append("&end=");
        q.append(std::to_string(end_time_ms));
    }
    return q;
}

class BybitBackfill
{
public:
    BybitBackfill(std::string rest_host = "api.bybit.com",
                  std::string rest_port = "443")
        : rest_host_(std::move(rest_host))
        , rest_port_(std::move(rest_port))
    {
    }

    explicit BybitBackfill(const endpoints& ep)
        : BybitBackfill(ep.rest_host, ep.rest_port)
    {
    }

    std::vector<backfill_bar> fetch(const std::string& symbol,
                                    const std::string& interval = "1",
                                    int count = 500,
                                    int64_t end_time_ms = 0) const
    {
        std::vector<backfill_bar> result;
        int remaining = count;
        int64_t end_ms = end_time_ms > 0
            ? end_time_ms
            : std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::system_clock::now().time_since_epoch())
                  .count();

        const std::string iv = normalize_kline_interval(interval);
        const std::string norm_iv = iv.empty() ? "1" : iv;
        const std::string sym = to_upper_ascii(symbol);

        while (remaining > 0)
        {
            const int batch = std::min(remaining, 1000);
            auto bars = fetch_batch(sym, norm_iv, batch, end_ms);
            if (bars.empty())
                break;

            result.insert(result.begin(), bars.begin(), bars.end());
            remaining -= static_cast<int>(bars.size());
            end_ms = bars.front().open_time - 1;
            if (static_cast<int>(bars.size()) < batch)
                break;
        }
        return result;
    }

    static std::vector<std::string>
    to_prepend_frames(const std::vector<backfill_bar>& bars,
                      std::string_view symbol,
                      std::string_view interval)
    {
        std::vector<std::string> lines;
        lines.reserve(bars.size());
        auto iv = normalize_kline_interval(interval);
        if (iv.empty()) iv = "1";
        const std::string sym = to_upper_ascii(std::string(symbol));
        for (const auto& b : bars)
            lines.push_back(encode_kline_frame(b, sym, iv));
        return lines;
    }

private:
    std::string rest_host_;
    std::string rest_port_;

    std::vector<backfill_bar>
    fetch_batch(const std::string& symbol,
                const std::string& interval,
                int limit,
                int64_t end_time_ms) const
    {
        namespace beast = boost::beast;
        namespace http = beast::http;
        namespace net = boost::asio;
        namespace ssl = net::ssl;
        using tcp = net::ip::tcp;

        try
        {
            const std::string target =
                std::string(paths::kline) + "?"
                + build_kline_query(symbol, interval, limit, end_time_ms);

            net::io_context ioc;
            ssl::context ctx(ssl::context::tlsv12_client);
            ctx.set_default_verify_paths();

            ssl::stream<tcp::socket> stream(ioc, ctx);
            SSL_set_tlsext_host_name(stream.native_handle(),
                                     rest_host_.c_str());

            tcp::resolver resolver(ioc);
            auto results = resolver.resolve(rest_host_, rest_port_);
            net::connect(stream.next_layer(), results);
            stream.handshake(ssl::stream_base::client);

            http::request<http::string_body> req(http::verb::get, target, 11);
            req.set(http::field::host, rest_host_);
            req.set(http::field::user_agent, "TrueTest/1.0");
            req.set("Accept", "application/json");

            http::write(stream, req);

            beast::flat_buffer buffer;
            http::response<http::string_body> res;
            http::read(stream, buffer, res);

            beast::error_code ec;
            stream.shutdown(ec);

            if (res.result() != http::status::ok)
                return {};
            return parse_kline_response(res.body());
        }
        catch (...)
        {
            return {};
        }
    }
};

} // namespace bybit

#endif // HAS_BYBIT
