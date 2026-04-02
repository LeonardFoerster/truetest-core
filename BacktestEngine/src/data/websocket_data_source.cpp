#ifdef HAS_LIVE_DATA

#include "websocket_data_source.h"

#include <iostream>
#include <thread>

// Boost.Beast / Asio includes
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/strand.hpp>

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = net::ip::tcp;

WebSocketDataSource::WebSocketDataSource(config cfg)
    : config_(std::move(cfg))
    , current_backoff_(config_.initial_backoff) {}

WebSocketDataSource::~WebSocketDataSource()
{
    stop();
}

void WebSocketDataSource::start(event_callback cb)
{
    callback_ = std::move(cb);
    running_.store(true, std::memory_order_release);
    io_thread_ = std::thread(&WebSocketDataSource::io_thread_main, this);
}

void WebSocketDataSource::stop()
{
    running_.store(false, std::memory_order_release);
    if (io_thread_.joinable())
        io_thread_.join();
}

void WebSocketDataSource::io_thread_main()
{
    while (running_.load(std::memory_order_acquire))
    {
        try
        {
            connect();
        }
        catch (const std::exception& e)
        {
            std::cerr << "  WebSocket error: " << e.what() << "\n";
            connected_.store(false, std::memory_order_release);
            schedule_reconnect();
        }
    }
}

void WebSocketDataSource::connect()
{
    net::io_context ioc;
    tcp::resolver resolver(ioc);

    // Parse URL into host:port (simplified — assumes ws://host:port/path)
    // A production implementation would use a proper URL parser
    std::string host = config_.endpoint_url;
    std::string port = "443";
    std::string path = "/";

    auto const results = resolver.resolve(host, port);

    websocket::stream<tcp::socket> ws(ioc);
    net::connect(ws.next_layer(), results.begin(), results.end());

    ws.handshake(host, path);
    connected_.store(true, std::memory_order_release);
    current_backoff_ = config_.initial_backoff; // reset backoff on success

    beast::flat_buffer buffer;
    while (running_.load(std::memory_order_acquire))
    {
        ws.read(buffer);
        std::string payload = beast::buffers_to_string(buffer.data());
        buffer.consume(buffer.size());
        on_message(payload);
    }

    // Clean shutdown
    beast::error_code ec;
    ws.close(websocket::close_code::normal, ec);
    connected_.store(false, std::memory_order_release);
}

// ── Lightweight JSON extraction (same pattern as BinanceParser) ────────────
namespace {

inline std::string ws_extract_string(const std::string& json, const std::string& key)
{
    std::string needle = "\"" + key + "\":\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return {};
    pos += needle.size();
    auto end = json.find('"', pos);
    return (end == std::string::npos) ? std::string{} : json.substr(pos, end - pos);
}

inline std::string ws_extract_number(const std::string& json, const std::string& key)
{
    std::string needle = "\"" + key + "\":";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return {};
    pos += needle.size();
    // Skip whitespace
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) ++pos;
    // If quoted number
    if (pos < json.size() && json[pos] == '"') {
        ++pos;
        auto end = json.find('"', pos);
        return (end == std::string::npos) ? std::string{} : json.substr(pos, end - pos);
    }
    auto end = json.find_first_of(",}] \t\n\r", pos);
    return (end == std::string::npos) ? json.substr(pos) : json.substr(pos, end - pos);
}

} // anonymous namespace

void WebSocketDataSource::on_message(const std::string& payload)
{
    if (payload.empty() || !callback_) return;

    // Sequence number gap detection
    auto seq_str = ws_extract_number(payload, "seq");
    if (!seq_str.empty()) {
        uint64_t seq = std::stoull(seq_str);
        uint64_t prev = last_seq_.load(std::memory_order_relaxed);
        if (prev > 0 && seq > prev + 1)
            gap_count_.fetch_add(1, std::memory_order_relaxed);
        last_seq_.store(seq, std::memory_order_relaxed);
    }

    auto msg_type = ws_extract_string(payload, "type");

    if (msg_type == "tick") {
        // {"type":"tick","symbol":"...","price":...,"qty":...,"ts":...}
        auto symbol = ws_extract_string(payload, "symbol");
        auto price_str = ws_extract_number(payload, "price");
        auto qty_str = ws_extract_number(payload, "qty");
        auto ts_str = ws_extract_number(payload, "ts");

        if (symbol.empty() || price_str.empty()) return;

        double price = std::stod(price_str);
        double qty = qty_str.empty() ? 0.0 : std::stod(qty_str);
        int64_t ts_ms = ts_str.empty() ? 0 : std::stoll(ts_str);

        auto timestamp = (ts_ms > 0)
            ? std::chrono::system_clock::time_point(std::chrono::milliseconds(ts_ms))
            : std::chrono::system_clock::now();

        // Create a market_event with the tick price as OHLC (single-point bar)
        market_event ev(timestamp, symbol, price, price, price, price,
                        static_cast<int64_t>(qty));
        callback_(ev);
    }
    else if (msg_type == "bar") {
        // {"type":"bar","symbol":"...","o":...,"h":...,"l":...,"c":...,"v":...,"ts":...}
        auto symbol = ws_extract_string(payload, "symbol");
        auto o_str = ws_extract_number(payload, "o");
        auto h_str = ws_extract_number(payload, "h");
        auto l_str = ws_extract_number(payload, "l");
        auto c_str = ws_extract_number(payload, "c");
        auto v_str = ws_extract_number(payload, "v");
        auto ts_str = ws_extract_number(payload, "ts");

        if (symbol.empty() || o_str.empty() || h_str.empty() ||
            l_str.empty() || c_str.empty()) return;

        double o = std::stod(o_str);
        double h = std::stod(h_str);
        double l = std::stod(l_str);
        double c = std::stod(c_str);
        int64_t v = v_str.empty() ? 0 : std::stoll(v_str);
        int64_t ts_ms = ts_str.empty() ? 0 : std::stoll(ts_str);

        auto timestamp = (ts_ms > 0)
            ? std::chrono::system_clock::time_point(std::chrono::milliseconds(ts_ms))
            : std::chrono::system_clock::now();

        market_event ev(timestamp, symbol, o, h, l, c, v);
        callback_(ev);
    }
    // Unrecognized message types are silently ignored (e.g., heartbeats)
}

void WebSocketDataSource::schedule_reconnect()
{
    if (!running_.load(std::memory_order_acquire))
        return;

    std::cerr << "  Reconnecting in " << current_backoff_.count() << "s...\n";
    std::this_thread::sleep_for(current_backoff_);

    // Exponential backoff with cap
    current_backoff_ = std::min(current_backoff_ * 2, config_.max_backoff);
}

#endif // HAS_LIVE_DATA
