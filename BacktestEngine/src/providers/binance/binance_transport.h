#pragma once
#ifdef HAS_BINANCE

#include "providers/transport.h"

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>

#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <iostream>

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
namespace ssl = net::ssl;
using tcp = net::ip::tcp;

// BinanceTransport: WebSocket client connecting to Binance's stream API.
//
// Endpoint: wss://stream.binance.com:9443/ws/<streamName>
// Stream names: <symbol>@trade, <symbol>@kline_<interval>, <symbol>@depth
//
// This is a streaming transport — is_streaming() returns true.
// read_line_blocking() blocks on ws.read() until data arrives.
// request_stop() closes the WebSocket to unblock.
class BinanceTransport : public IDataTransport
{
public:
    // symbol: lowercase, e.g. "btcusdt"
    // stream_type: "trade", "kline_1m", "depth", etc.
    // host/port: override for testnet usage
    BinanceTransport(
        const std::string& symbol,
        const std::string& stream_type,
        const std::string& host = "stream.binance.com",
        const std::string& port = "9443")
        : symbol_(symbol)
        , stream_type_(stream_type)
        , host_(host)
        , port_(port)
        , ctx_(ssl::context::tlsv12_client)
    {
        ctx_.set_default_verify_paths();
        ctx_.set_verify_mode(ssl::verify_peer);
    }

    bool open() override
    {
        try
        {
            // Resolve
            tcp::resolver resolver(ioc_);
            auto results = resolver.resolve(host_, port_);

            // Create the SSL+WS stream
            ws_ = std::make_unique<
                websocket::stream<beast::ssl_stream<tcp::socket>>>(ioc_, ctx_);

            // Connect TCP
            auto& lowest = beast::get_lowest_layer(*ws_);
            net::connect(lowest, results);

            // SNI hostname
            if (!SSL_set_tlsext_host_name(
                    ws_->next_layer().native_handle(), host_.c_str()))
            {
                std::cerr << "BinanceTransport: SNI setup failed\n";
                return false;
            }

            // TLS handshake
            ws_->next_layer().handshake(ssl::stream_base::client);

            // Set WebSocket options
            ws_->set_option(websocket::stream_base::decorator(
                [](websocket::request_type& req) {
                    req.set(boost::beast::http::field::user_agent, "TrueTest/1.0");
                }));

            // Enable automatic pong responses to server pings.
            // Binance requires responding to pings within 10 minutes.
            ws_->control_callback(
                [](websocket::frame_type kind, beast::string_view) {
                    // Beast auto-responds to pings with pongs when using
                    // synchronous reads. This callback is informational.
                    (void)kind;
                });

            // WebSocket handshake
            std::string target = "/ws/" + symbol_ + "@" + stream_type_;
            ws_->handshake(host_ + ":" + port_, target);

            open_ = true;
            stopped_ = false;
            return true;
        }
        catch (const std::exception& e)
        {
            std::cerr << "BinanceTransport: open failed: " << e.what() << "\n";
            return false;
        }
    }

    void close() override
    {
        std::lock_guard<std::mutex> lk(mu_);
        open_ = false;

        if (ws_)
        {
            try
            {
                beast::error_code ec;
                ws_->close(websocket::close_code::normal, ec);
            }
            catch (...) {}
        }
    }

    bool is_open() const override
    {
        return open_.load();
    }

    std::optional<std::string> read_line() override
    {
        return read_line_blocking();
    }

    bool is_streaming() const override { return true; }

    std::optional<std::string> read_line_blocking() override
    {
        if (!ws_ || stopped_.load())
            return std::nullopt;

        try
        {
            beast::flat_buffer buffer;
            ws_->read(buffer);

            if (stopped_.load())
                return std::nullopt;

            return beast::buffers_to_string(buffer.data());
        }
        catch (const beast::system_error& se)
        {
            if (se.code() == websocket::error::closed)
            {
                open_ = false;
                return std::nullopt;
            }

            // Connection lost — attempt reconnect
            if (!stopped_.load() && reconnect())
                return read_line_blocking(); // retry after reconnect

            open_ = false;
            return std::nullopt;
        }
        catch (const std::exception& e)
        {
            std::cerr << "BinanceTransport: read error: " << e.what() << "\n";
            open_ = false;
            return std::nullopt;
        }
    }

    void request_stop() override
    {
        stopped_ = true;
        close();
    }

private:
    std::string symbol_;
    std::string stream_type_;
    std::string host_;
    std::string port_;

    net::io_context ioc_;
    ssl::context ctx_;
    std::unique_ptr<websocket::stream<beast::ssl_stream<tcp::socket>>> ws_;

    std::mutex mu_;
    std::atomic<bool> open_{false};
    std::atomic<bool> stopped_{false};

    int reconnect_attempts_ = 0;
    static constexpr int MAX_RECONNECTS = 5;

    bool reconnect()
    {
        if (reconnect_attempts_ >= MAX_RECONNECTS)
        {
            std::cerr << "BinanceTransport: max reconnect attempts reached\n";
            return false;
        }
        reconnect_attempts_++;

        // Exponential backoff: 1s, 2s, 4s, 8s, 16s
        int delay_ms = 1000 * (1 << (reconnect_attempts_ - 1));
        std::cerr << "BinanceTransport: reconnecting in " << delay_ms << "ms (attempt "
                  << reconnect_attempts_ << "/" << MAX_RECONNECTS << ")\n";

        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));

        // Re-create io_context and try again
        ioc_.restart();
        ws_.reset();
        return open();
    }
};

#endif // HAS_BINANCE
