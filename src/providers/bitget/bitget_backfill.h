#pragma once
#ifdef HAS_BITGET

// REST candle backfill for Bitget UTA public market data.
// GET /api/v3/market/candles → prepend frames that BitgetKlineParser accepts
// (arg.topic=kline + data[] with confirm:true so the closed-bar gate emits).

#include "providers/bitget/bitget_parser.h"
#include "providers/bitget/bitget_rest_client.h"
#include "providers/bitget/bitget_transport.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace bitget {

struct backfill_bar
{
    int64_t open_time = 0;
    double  open = 0;
    double  high = 0;
    double  low = 0;
    double  close = 0;
    double  volume = 0;
};

// Pure: parse UTA candles envelope (data[] of [ts,o,h,l,c,vol,turnover]).
// Returns bars oldest→newest when the venue returns newest-first (we reverse).
inline std::vector<backfill_bar> parse_candles_response(std::string_view body)
{
    std::vector<backfill_bar> out;
    if (!is_business_success(200, body))
        return out;

    // data is an array of arrays — walk raw text for [ ... ] rows that
    // contain enough commas (not the outer data array alone).
    auto data_arr = detail::extract_array(body, "data");
    if (data_arr.size() < 2 || data_arr.front() != '[')
        return out;

    // data is an array of arrays: [ [ts,o,h,l,c,vol,...], ... ]
    std::size_t pos = 1;
    while (pos < data_arr.size())
    {
        detail::skip_ws(data_arr, pos);
        if (pos >= data_arr.size() || data_arr[pos] == ']')
            break;
        if (data_arr[pos] == ',')
        {
            ++pos;
            continue;
        }
        if (data_arr[pos] != '[')
            break;
        auto close = detail::match_container(data_arr, pos);
        if (close == std::string_view::npos)
            break;
        std::string_view row = data_arr.substr(pos + 1, close - pos - 1);
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
                // trim spaces
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
        if (fields.size() < 6)
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
        if (!parse_double_sv(fields[5], b.volume))
            b.volume = 0;
        out.push_back(b);
    }

    // Venue often returns newest first; prepend wants chronological order.
    if (out.size() >= 2 && out.front().open_time > out.back().open_time)
        std::reverse(out.begin(), out.end());
    return out;
}

// Encode a closed kline frame accepted by BitgetKlineParser (confirm:true).
inline std::string encode_kline_frame(const backfill_bar& b,
                                      std::string_view symbol,
                                      std::string_view interval)
{
    char buf[768];
    std::snprintf(
        buf, sizeof(buf),
        R"({"arg":{"instType":"usdt-futures","topic":"kline","symbol":"%.*s","interval":"%.*s"},"data":[{"start":"%lld","open":"%.8f","high":"%.8f","low":"%.8f","close":"%.8f","volume":"%.8f","confirm":true}],"ts":%lld})",
        static_cast<int>(symbol.size()), symbol.data(),
        static_cast<int>(interval.size()), interval.data(),
        static_cast<long long>(b.open_time),
        b.open, b.high, b.low, b.close, b.volume,
        static_cast<long long>(b.open_time));
    return std::string(buf);
}

inline std::string candles_query(std::string_view category,
                                 std::string_view symbol,
                                 std::string_view interval,
                                 int limit,
                                 int64_t end_time_ms = 0)
{
    // Alphabetical keys for sign stability (unsigned still fine).
    std::string q;
    q.reserve(96 + category.size() + symbol.size() + interval.size());
    q.append("category=");
    q.append(category);
    q.append("&interval=");
    q.append(interval);
    q.append("&limit=");
    q.append(std::to_string(std::max(1, std::min(limit, 1000))));
    q.append("&symbol=");
    q.append(symbol);
    if (end_time_ms > 0)
    {
        q.append("&endTime=");
        q.append(std::to_string(end_time_ms));
    }
    return q;
}

// Fetch historical candles (cold path). Uses unsigned REST (public market).
class BitgetBackfill
{
public:
    BitgetBackfill(std::string rest_host = "api.bitget.com",
                   std::string rest_port = "443",
                   std::string category = "USDT-FUTURES")
        : rest_host_(std::move(rest_host))
        , rest_port_(std::move(rest_port))
        , category_(std::move(category))
    {}

    std::vector<backfill_bar> fetch(const std::string& symbol,
                                    const std::string& interval = "1m",
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

        // Empty credentials — only get_unsigned is used (public endpoint).
        BitgetRestClient rest(/*key=*/"", /*secret=*/"", /*pass=*/"",
                              rest_host_, rest_port_);

        const std::string iv = normalize_kline_interval(interval);
        const std::string sym = symbol; // caller should pass upper

        while (remaining > 0)
        {
            const int batch = std::min(remaining, 200);
            auto resp = rest.get_unsigned(
                "/api/v3/market/candles",
                candles_query(category_, sym, iv, batch, end_ms));
            if (resp.status < 200 || resp.status >= 300)
                break;

            auto bars = parse_candles_response(resp.body);
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

    // Convenience: bars → prepend frame lines for PrependTransport.
    static std::vector<std::string>
    to_prepend_frames(const std::vector<backfill_bar>& bars,
                      std::string_view symbol,
                      std::string_view interval)
    {
        std::vector<std::string> lines;
        lines.reserve(bars.size());
        const std::string iv = normalize_kline_interval(interval);
        for (const auto& b : bars)
            lines.push_back(encode_kline_frame(b, symbol, iv));
        return lines;
    }

private:
    std::string rest_host_;
    std::string rest_port_;
    std::string category_;
};

} // namespace bitget

#endif // HAS_BITGET
