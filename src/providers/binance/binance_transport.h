#pragma once
#ifdef HAS_BINANCE

#include "providers/transport.h"
#include "utils/retry.h"

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <iostream>

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
namespace ssl = net::ssl;
using tcp = net::ip::tcp;

class BinanceTransport : public IDataTransport
{
public:
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
            tcp::resolver resolver(ioc_);
            auto results = resolver.resolve(host_, port_);

            ws_ = std::make_unique<
                websocket::stream<beast::ssl_stream<tcp::socket>>>(ioc_, ctx_);

            auto& lowest = beast::get_lowest_layer(*ws_);
            net::connect(lowest, results);

            // TCP keepalive: belt-and-suspenders detection for a dead
            // peer when the Beast WS-ping path is itself wedged (we are
            // primarily a receiver, so without keepalive the kernel
            // never retransmits and a cable-pull goes undetected for
            // ~indefinitely). Aggressive thresholds (1s idle / 1s probe
            // interval / 2 probes) bound kernel-side detection to ~3s.
            // Best-effort: setsockopt failures are non-fatal — the WS
            // idle_timeout below is the primary detector regardless.
            {
                const int yes = 1;
                const int idle = 1, intvl = 1, cnt = 2;
                const int fd = lowest.native_handle();
                ::setsockopt(fd, SOL_SOCKET,  SO_KEEPALIVE,  &yes,   sizeof(yes));
                ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE,  &idle,  sizeof(idle));
                ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
                ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT,   &cnt,   sizeof(cnt));
            }

            if (!SSL_set_tlsext_host_name(
                    ws_->next_layer().native_handle(), host_.c_str()))
            {
                std::cerr << "BinanceTransport: SNI setup failed\n";
                return false;
            }

            ws_->next_layer().handshake(ssl::stream_base::client);

            // Idle / handshake timeout: a dead stream errors out within
            // idle_timeout. keep_alive_pings=true makes Beast send WS
            // pings on idle and treat absence of pong as failure — the
            // primary detector for a silent cable-pull. handshake_timeout
            // bounds the WS upgrade so a hung server can't stall startup.
            {
                websocket::stream_base::timeout opt;
                opt.handshake_timeout = std::chrono::seconds(3);
                opt.idle_timeout      = std::chrono::milliseconds(1500);
                opt.keep_alive_pings  = true;
                ws_->set_option(opt);
            }

            ws_->set_option(websocket::stream_base::decorator(
                [](websocket::request_type& req) {
                    req.set(boost::beast::http::field::user_agent, "TrueTest/1.0");
                }));

            ws_->control_callback(
                [](websocket::frame_type kind, beast::string_view) {
                    (void)kind;
                });

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
        std::string_view view;
        if (!read_frame_blocking(view))
            return std::nullopt;
        return std::string(view);
    }

    bool read_frame_blocking(std::string_view& out) override
    {
        if (!ws_ || stopped_.load())
            return false;

        try
        {
            if (frame_buffer_.size() > 0)
                frame_buffer_.consume(frame_buffer_.size());

            ws_->read(frame_buffer_);

            if (stopped_.load())
                return false;

            auto const_buf = frame_buffer_.data();
            out = std::string_view(
                static_cast<const char*>(const_buf.data()),
                const_buf.size());
            return true;
        }
        catch (const beast::system_error& se)
        {
            const bool clean_close = (se.code() == websocket::error::closed);
            std::cerr << "BinanceTransport: websocket "
                      << (clean_close ? "closed by server" : "read error")
                      << " (" << se.code().message() << ")\n";

            open_ = false;

            if (stopped_.load())
                return false;

            if (fatal_cb_)
            {
                char buf[160];
                std::snprintf(buf, sizeof(buf),
                              "binance market-data WS lost: %s",
                              se.code().message().c_str());
                fatal_cb_(buf);
                stopped_ = true;
                return false;
            }

            if (reconnect())
                return read_frame_blocking(out);

            return false;
        }
        catch (const std::exception& e)
        {
            std::cerr << "BinanceTransport: read error: " << e.what() << "\n";
            open_ = false;

            if (stopped_.load())
                return false;

            if (fatal_cb_)
            {
                char buf[160];
                std::snprintf(buf, sizeof(buf),
                              "binance market-data WS lost: %s", e.what());
                fatal_cb_(buf);
                stopped_ = true;
                return false;
            }

            if (reconnect())
                return read_frame_blocking(out);

            return false;
        }
    }

    void request_stop() override
    {
        stopped_ = true;
        close();
    }

    bool reconnect_stream(const std::string& new_symbol,
                          const std::string& new_stream_type)
    {
        request_stop();

        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        symbol_ = new_symbol;
        stream_type_ = new_stream_type;

        stopped_ = false;

        ioc_.restart();
        ws_.reset();
        return open();
    }

    // Engine wires this in live mode. When set, a read/handshake error
    // routes here directly and the transport STOPS — no reconnect loop.
    // When unset (backtest/shadow paths), the existing reconnect-on-error
    // behaviour stands. The reason string is published verbatim through
    // engine::trigger_halt to the dashboard banner.
    void set_fatal_disconnect_callback(
        std::function<void(std::string_view reason)> cb)
    {
        fatal_cb_ = std::move(cb);
    }

private:
    std::string symbol_;
    std::string stream_type_;
    std::string host_;
    std::string port_;

    net::io_context ioc_;
    ssl::context ctx_;
    std::unique_ptr<websocket::stream<beast::ssl_stream<tcp::socket>>> ws_;

    beast::flat_buffer frame_buffer_;

    std::mutex mu_;
    std::atomic<bool> open_{false};
    std::atomic<bool> stopped_{false};

    std::function<void(std::string_view)> fatal_cb_;

    static constexpr unsigned MAX_RECONNECTS = 30;

    bool reconnect()
    {
        retry_config cfg;
        cfg.max_attempts = MAX_RECONNECTS;
        cfg.initial_delay = std::chrono::milliseconds(1000);
        cfg.max_delay = std::chrono::milliseconds(30000);
        cfg.on_retry = [this](unsigned attempt, std::exception_ptr) {
            if (stopped_.load()) return;
            std::cerr << "BinanceTransport: reconnecting (attempt "
                      << attempt << "/" << MAX_RECONNECTS << ")\n";
        };

        return retry_with_backoff([this]() {
            if (stopped_.load()) return true;  // bail out of retry loop
            ioc_.restart();
            ws_.reset();
            return open();
        }, cfg);
    }
};

#endif // HAS_BINANCE
