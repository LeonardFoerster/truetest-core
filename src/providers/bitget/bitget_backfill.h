#pragma once
#ifdef HAS_BITGET

// REST candle backfill for Bitget UTA public market data.
// GET /api/v3/market/candles parsing utilities. Production prepend remains
// explicitly unsupported until the engine has a non-trading warmup phase and
// a causal known-at timestamp; treating REST rows as confirmed live events can
// submit orders from historical or still-forming candles.

#include "providers/bitget/bitget_parser.h"
#include "data/strict_market_csv.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <stdexcept>
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

namespace backfill_detail {

inline bool business_success(std::string_view body) noexcept
{
    if (!provider_recovery::is_authoritative_object(body))
        return false;
    std::string_view code;
    return provider_recovery::top_level_scalar_text(body, "code", code)
        && code == "00000";
}

inline bool parse_row_fields(
    std::string_view row,
    std::array<std::string_view, 7>& fields) noexcept
{
    std::size_t pos = 0;
    std::size_t count = 0;
    while (pos < row.size())
    {
        while (pos < row.size()
               && (row[pos] == ' ' || row[pos] == '\t'
                   || row[pos] == '\n' || row[pos] == '\r'))
            ++pos;
        if (pos >= row.size() || count >= fields.size() || row[pos] != '"')
            return false;
        const std::size_t begin = ++pos;
        const std::size_t end = row.find('"', begin);
        if (end == std::string_view::npos)
            return false;
        fields[count++] = row.substr(begin, end - begin);
        pos = end + 1;
        while (pos < row.size()
               && (row[pos] == ' ' || row[pos] == '\t'
                   || row[pos] == '\n' || row[pos] == '\r'))
            ++pos;
        if (pos == row.size())
            break;
        if (row[pos] != ',')
            return false;
        ++pos;
        if (pos == row.size())
            return false;
    }
    return count == fields.size();
}

inline bool parse_row(std::string_view row, backfill_bar& out) noexcept
{
    std::array<std::string_view, 7> fields{};
    if (!parse_row_fields(row, fields)
        || !parse_int64_sv(fields[0], out.open_time)
        || !detail::parse_ts_ms(fields[0])
        || !parse_double_sv(fields[1], out.open)
        || !parse_double_sv(fields[2], out.high)
        || !parse_double_sv(fields[3], out.low)
        || !parse_double_sv(fields[4], out.close)
        || !parse_double_sv(fields[5], out.volume))
        return false;

    double turnover = 0.0;
    if (!parse_double_sv(fields[6], turnover)
        || !(out.open > 0.0) || !(out.high > 0.0)
        || !(out.low > 0.0) || !(out.close > 0.0)
        || out.volume < 0.0 || turnover < 0.0
        || out.high < std::max(out.open, out.close)
        || out.low > std::min(out.open, out.close)
        || out.high < out.low)
        return false;

    // Validate the venue decimal before retaining the presentation double.
    // Binary conversion followed by rounding would otherwise turn a positive
    // sub-atom quantity into zero and silently change the economic record.
    std::int64_t volume_atoms = 0;
    std::uint64_t volume_scale = 0;
    return tt::strict_market_csv::parse_decimal_atoms(
        fields[5], true, volume_atoms, volume_scale);
}

} // namespace backfill_detail

// Pure: parse UTA candles envelope (data[] of [ts,o,h,l,c,vol,turnover]).
// Returns bars oldest→newest when the venue returns newest-first (we reverse).
inline std::vector<backfill_bar> parse_candles_response(std::string_view body)
{
    std::vector<backfill_bar> out;
    if (!backfill_detail::business_success(body))
        return out;

    // data is an array of arrays — walk raw text for [ ... ] rows that
    // contain enough commas (not the outer data array alone).
    std::string_view data_arr;
    provider_recovery::payload_parser envelope(body);
    if (envelope.inspect_top_level_member("data", data_arr)
            != provider_recovery::payload_parser::member_result::unique
        || data_arr.size() < 2 || data_arr.front() != '['
        || data_arr.back() != ']')
        return out;

    // data is an exact array of seven-field rows:
    // [ [ts,o,h,l,c,vol,turnover], ... ]. A single malformed row rejects the
    // complete response; partial history must never masquerade as success.
    std::size_t pos = 1;
    bool first_row = true;
    while (pos < data_arr.size())
    {
        detail::skip_ws(data_arr, pos);
        if (pos >= data_arr.size())
            return {};
        if (data_arr[pos] == ']')
            break;
        if (!first_row)
        {
            if (data_arr[pos] != ',')
                return {};
            ++pos;
            detail::skip_ws(data_arr, pos);
            if (pos >= data_arr.size() || data_arr[pos] == ']')
                return {};
        }
        if (data_arr[pos] != '[')
            return {};
        auto close = detail::match_container(data_arr, pos);
        if (close == std::string_view::npos)
            return {};
        std::string_view row = data_arr.substr(pos + 1, close - pos - 1);
        pos = close + 1;
        backfill_bar b{};
        if (!backfill_detail::parse_row(row, b))
            return {};
        out.push_back(b);
        first_row = false;
    }

    // Only a strictly monotone venue page is authoritative. Normalize the
    // documented newest-first variant; reject duplicates and mixed ordering.
    if (out.size() >= 2)
    {
        const bool ascending = out[0].open_time < out[1].open_time;
        for (std::size_t i = 1; i < out.size(); ++i)
        {
            if (ascending
                    ? out[i - 1].open_time >= out[i].open_time
                    : out[i - 1].open_time <= out[i].open_time)
                return {};
        }
        if (!ascending)
            std::reverse(out.begin(), out.end());
    }
    return out;
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
    {
        (void)rest_host;
        (void)rest_port;
        (void)category;
    }

    std::vector<backfill_bar> fetch(const std::string& symbol,
                                    const std::string& interval = "1m",
                                    int count = 500,
                                    int64_t end_time_ms = 0) const
    {
        (void)symbol;
        (void)interval;
        (void)end_time_ms;
        if (count <= 0)
            return {};
        throw std::logic_error(
            "Bitget candle backfill is unsupported until a non-trading "
            "warmup barrier and causal known-at clock are available");
    }

    // Convenience: bars → prepend frame lines for PrependTransport.
    static std::vector<std::string>
    to_prepend_frames(const std::vector<backfill_bar>& bars,
                      std::string_view symbol,
                      std::string_view interval)
    {
        std::vector<std::string> lines;
        (void)symbol;
        (void)interval;
        if (!bars.empty())
            throw std::logic_error(
                "Bitget candle backfill prepend is unsupported without a "
                "non-trading warmup barrier");
        return lines;
    }

};

} // namespace bitget

#endif // HAS_BITGET
