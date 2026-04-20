#pragma once
#ifdef HAS_BINANCE

#include "binance_rest_client.h"
#include "../../data/data_handler.h"

#include <string>
#include <vector>
#include <cstdint>
#include <chrono>

struct backfill_bar {
    int64_t open_time;
    double open, high, low, close;
    double volume;
};

class BinanceBackfill {
public:
    explicit BinanceBackfill(const std::string& host = "api.binance.com",
                             const std::string& port = "443")
        : host_(host), port_(port)
    {}

    std::vector<backfill_bar> fetch(
        const std::string& symbol,
        const std::string& interval = "1m",
        int count = 500,
        int64_t end_time_ms = 0) const
    {
        std::vector<backfill_bar> result;

        int remaining = count;
        int64_t end_ms = end_time_ms > 0
            ? end_time_ms
            : std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::system_clock::now().time_since_epoch()).count();

        while (remaining > 0) {
            int batch = std::min(remaining, 1000);
            auto bars = fetch_batch(symbol, interval, batch, end_ms);
            if (bars.empty()) break;

            result.insert(result.begin(), bars.begin(), bars.end());
            remaining -= static_cast<int>(bars.size());

            end_ms = bars.front().open_time - 1;

            if (static_cast<int>(bars.size()) < batch) break;
        }

        return result;
    }

private:
    std::string host_;
    std::string port_;

    std::vector<backfill_bar> fetch_batch(
        const std::string& symbol,
        const std::string& interval,
        int limit,
        int64_t end_time_ms) const
    {
        std::string query = "symbol=" + to_upper(symbol)
            + "&interval=" + interval
            + "&limit=" + std::to_string(limit);
        if (end_time_ms > 0) {
            query += "&endTime=" + std::to_string(end_time_ms);
        }

        namespace beast = boost::beast;
        namespace http = beast::http;
        namespace net = boost::asio;
        namespace ssl = net::ssl;
        using tcp = net::ip::tcp;

        net::io_context ioc;
        ssl::context ctx(ssl::context::tlsv12_client);
        ctx.set_default_verify_paths();

        ssl::stream<tcp::socket> stream(ioc, ctx);
        SSL_set_tlsext_host_name(stream.native_handle(), host_.c_str());

        tcp::resolver resolver(ioc);
        auto results = resolver.resolve(host_, port_);
        net::connect(stream.next_layer(), results);
        stream.handshake(ssl::stream_base::client);

        http::request<http::string_body> req(
            http::verb::get, "/api/v3/klines?" + query, 11);
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

        if (fields.size() < 6) return false;

        try {
            bar.open_time = std::stoll(fields[0]);
            bar.open      = std::stod(fields[1]);
            bar.high      = std::stod(fields[2]);
            bar.low       = std::stod(fields[3]);
            bar.close     = std::stod(fields[4]);
            bar.volume    = std::stod(fields[5]);
            return true;
        } catch (...) {
            return false;
        }
    }

    static std::string to_upper(std::string s)
    {
        for (auto& c : s) c = static_cast<char>(std::toupper(c));
        return s;
    }
};

#endif // HAS_BINANCE
