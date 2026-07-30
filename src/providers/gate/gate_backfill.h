#pragma once
#ifdef HAS_GATE

// REST candlesticks → prepend frames accepted by GateCombinedParser.
// Cold path only. Pure parse helpers are unit-testable offline.

#include "providers/gate/gate_endpoints.h"
#include "providers/gate/gate_parser.h"
#include "providers/gate/gate_transport.h"

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

namespace gate {

struct backfill_bar
{
    int64_t open_time = 0; // seconds (Gate candlestick t)
    double  open = 0;
    double  high = 0;
    double  low = 0;
    double  close = 0;
    double  volume = 0;
};

// Parse GET /futures/{settle}/candlesticks body.
// Array of objects: [{t,o,h,l,c,v,sum}, ...] (oldest or newest first).
inline std::vector<backfill_bar>
parse_candlesticks_response(std::string_view body)
{
    std::vector<backfill_bar> out;
    if (body.empty()) return out;

    // Body is a top-level array.
    std::string_view arr = body;
    std::size_t start = body.find('[');
    if (start == std::string_view::npos) return out;
    auto close = json_util::matching_close(body, start);
    if (close == std::string_view::npos) return out;
    arr = body.substr(start, close - start + 1);

    json_util::for_each_array_object(arr, [&](std::string_view obj) {
        backfill_bar b{};
        auto t_sv = extract_sv_number(obj, "t");
        if (t_sv.empty())
            t_sv = extract_sv_string(obj, "t");
        auto o_sv = extract_sv_string(obj, "o");
        if (o_sv.empty()) o_sv = extract_sv_number(obj, "o");
        auto h_sv = extract_sv_string(obj, "h");
        if (h_sv.empty()) h_sv = extract_sv_number(obj, "h");
        auto l_sv = extract_sv_string(obj, "l");
        if (l_sv.empty()) l_sv = extract_sv_number(obj, "l");
        auto c_sv = extract_sv_string(obj, "c");
        if (c_sv.empty()) c_sv = extract_sv_number(obj, "c");
        auto v_sv = extract_sv_string(obj, "v");
        if (v_sv.empty()) v_sv = extract_sv_number(obj, "v");

        if (t_sv.empty() || o_sv.empty() || h_sv.empty() || l_sv.empty()
            || c_sv.empty())
            return;
        if (!json_util::parse_intish(t_sv, b.open_time)) return;
        if (!json_util::parse_numberish(o_sv, b.open)) return;
        if (!json_util::parse_numberish(h_sv, b.high)) return;
        if (!json_util::parse_numberish(l_sv, b.low)) return;
        if (!json_util::parse_numberish(c_sv, b.close)) return;
        if (!v_sv.empty())
            json_util::parse_numberish(v_sv, b.volume);
        out.push_back(b);
    });

    // Prefer chronological oldest→newest for prepend.
    if (out.size() >= 2 && out.front().open_time > out.back().open_time)
        std::reverse(out.begin(), out.end());
    return out;
}

// Encode a closed candle as a futures.candlesticks update frame.
inline std::string encode_candlestick_frame(const backfill_bar& b,
                                            std::string_view symbol,
                                            std::string_view interval)
{
    char buf[768];
    // n = "{interval}_{symbol}" e.g. 1m_BTC_USDT
    std::snprintf(
        buf, sizeof(buf),
        R"({"time":%lld,"channel":"futures.candlesticks","event":"update","result":{"t":%lld,"v":"%.8f","c":"%.8f","h":"%.8f","l":"%.8f","o":"%.8f","n":"%.*s_%.*s","a":"0"}})",
        static_cast<long long>(b.open_time),
        static_cast<long long>(b.open_time),
        b.volume, b.close, b.high, b.low, b.open,
        static_cast<int>(interval.size()), interval.data(),
        static_cast<int>(symbol.size()), symbol.data());
    return std::string(buf);
}

inline std::string candlesticks_query(std::string_view contract,
                                      std::string_view interval,
                                      int limit,
                                      int64_t to_s = 0)
{
    std::string q;
    q.reserve(96 + contract.size() + interval.size());
    q.append("contract=");
    q.append(contract);
    q.append("&interval=");
    q.append(interval);
    q.append("&limit=");
    q.append(std::to_string(std::max(1, std::min(limit, 2000))));
    if (to_s > 0)
    {
        q.append("&to=");
        q.append(std::to_string(to_s));
    }
    return q;
}

// Normalize CLI interval for Gate (1m, 5m, 1h, 1d — lowercase units ok).
inline std::string normalize_candle_interval(std::string_view interval)
{
    if (interval.empty())
        return "1m";
    std::string out(interval);
    // Gate accepts 1m/5m/1h/4h/1d; keep as-is with lower unit letter.
    if (!out.empty())
    {
        char& last = out.back();
        if (last == 'M') last = 'm';
        if (last == 'H') last = 'h';
        if (last == 'D') last = 'd';
    }
    return out;
}

class GateBackfill
{
public:
    GateBackfill(std::string rest_host = "api.gateio.ws",
                 std::string rest_port = "443",
                 std::string rest_prefix = "/api/v4",
                 settle_ccy settle = settle_ccy::usdt)
        : rest_host_(std::move(rest_host))
        , rest_port_(std::move(rest_port))
        , rest_prefix_(std::move(rest_prefix))
        , settle_(settle)
    {
    }

    explicit GateBackfill(const endpoints& ep)
        : GateBackfill(ep.rest_host, ep.rest_port, ep.rest_prefix, ep.settle)
    {
    }

    std::vector<backfill_bar> fetch(const std::string& contract,
                                    const std::string& interval = "1m",
                                    int count = 500,
                                    int64_t to_s = 0) const
    {
        std::vector<backfill_bar> result;
        int remaining = count;
        int64_t end_s = to_s > 0
            ? to_s
            : static_cast<int64_t>(
                  std::chrono::duration_cast<std::chrono::seconds>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count());

        const std::string iv = normalize_candle_interval(interval);
        const std::string settle =
            settle_ == settle_ccy::usdt ? "usdt" : "btc";

        while (remaining > 0)
        {
            const int batch = std::min(remaining, 1000);
            auto bars = fetch_batch(contract, iv, batch, end_s, settle);
            if (bars.empty())
                break;

            result.insert(result.begin(), bars.begin(), bars.end());
            remaining -= static_cast<int>(bars.size());
            end_s = bars.front().open_time - 1;
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
        const std::string iv = normalize_candle_interval(interval);
        for (const auto& b : bars)
            lines.push_back(encode_candlestick_frame(b, symbol, iv));
        return lines;
    }

private:
    std::string rest_host_;
    std::string rest_port_;
    std::string rest_prefix_;
    settle_ccy settle_;

    std::vector<backfill_bar>
    fetch_batch(const std::string& contract,
                const std::string& interval,
                int limit,
                int64_t to_s,
                const std::string& settle) const
    {
        namespace beast = boost::beast;
        namespace http = beast::http;
        namespace net = boost::asio;
        namespace ssl = net::ssl;
        using tcp = net::ip::tcp;

        try
        {
            const std::string target =
                rest_prefix_ + "/futures/" + settle + "/candlesticks?"
                + candlesticks_query(contract, interval, limit, to_s);

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
            return parse_candlesticks_response(res.body());
        }
        catch (...)
        {
            return {};
        }
    }
};

} // namespace gate

#endif // HAS_GATE
