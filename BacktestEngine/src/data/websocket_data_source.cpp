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

void WebSocketDataSource::on_message(const std::string& payload)
{
    // Parse the message and invoke the callback.
    // The actual parsing depends on the exchange's message format.
    // This is a placeholder that would be specialized per exchange.

    // Sequence number gap detection
    // (assumes messages contain a sequence number — parsing omitted)
    (void)payload;

    // Example: parse JSON/binary into market_event and invoke callback_
    // auto event = parse_market_event(payload, config_.binary_format);
    // callback_(event);
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
