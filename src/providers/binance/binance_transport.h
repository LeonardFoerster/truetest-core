#pragma once
#ifdef HAS_BINANCE

#include "providers/transport.h"
#include "providers/thread_safe_callback.h"
#include "providers/bounded_ws_open.h"
#include "providers/bounded_ws_frame_reader.h"
#include "providers/socket_readiness.h"
#include "utils/retry.h"

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>

#include <algorithm>
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
#include <openssl/ssl.h>
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
            ws_ = std::make_shared<
                websocket::stream<beast::ssl_stream<tcp::socket>>>(*ioc_, ctx_);

            // TCP keepalive: belt-and-suspenders detection for a dead
            // peer when the Beast WS-ping path is itself wedged (we are
            // primarily a receiver, so without keepalive the kernel
            // never retransmits and a cable-pull goes undetected for
            // ~indefinitely). Aggressive thresholds (1s idle / 1s probe
            // interval / 2 probes) bound kernel-side detection to ~3s.
            // Best-effort: setsockopt failures are non-fatal - the WS
            // idle_timeout below is the primary detector regardless.
            const std::string target = "/ws/" + symbol_ + "@" + stream_type_;
            const bool opened = provider_ws::open_tls_websocket(
                *ioc_, *ws_, host_, port_, target, std::chrono::seconds(3),
                [&](auto& socket) {
                auto& lowest = beast::get_lowest_layer(socket);
                const int yes = 1;
                const int idle = 1, intvl = 1, cnt = 2;
                const int fd = lowest.native_handle();
                ::setsockopt(fd, SOL_SOCKET,  SO_KEEPALIVE,  &yes,   sizeof(yes));
                ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE,  &idle,  sizeof(idle));
                ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
                ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT,   &cnt,   sizeof(cnt));
                },
                [&](auto& socket) {
                websocket::stream_base::timeout opt;
                opt.handshake_timeout = std::chrono::seconds(3);
                opt.idle_timeout      = std::chrono::milliseconds(1500);
                opt.keep_alive_pings  = true;
                socket.set_option(opt);
                socket.set_option(websocket::stream_base::decorator(
                [](websocket::request_type& req) {
                    req.set(boost::beast::http::field::user_agent, "TrueTest/1.0");
                }));
                socket.control_callback(
                [](websocket::frame_type kind, beast::string_view) {
                    (void)kind;
                });
                });
            if (!opened) return false;

            socket_interrupt_.publish(
                beast::get_lowest_layer(*ws_).native_handle());
            frame_reader_.reset();
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
        stopped_ = true;
        open_ = false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            socket_interrupt_.clear();
            if (ws_)
            {
                beast::error_code ec;
                auto& lowest = beast::get_lowest_layer(*ws_);
                lowest.cancel(ec);
                lowest.close(ec);
            }
        }
        (void)frame_reader_.drain_after_cancel(
            ioc_, std::chrono::steady_clock::now() + std::chrono::milliseconds{250});
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

            if (auto fatal = fatal_cb_.load())
            {
                char buf[160];
                std::snprintf(buf, sizeof(buf),
                              "binance market-data WS lost: %s",
                              se.code().message().c_str());
                (*fatal)(buf);
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

            if (auto fatal = fatal_cb_.load())
            {
                char buf[160];
                std::snprintf(buf, sizeof(buf),
                              "binance market-data WS lost: %s", e.what());
                (*fatal)(buf);
                stopped_ = true;
                return false;
            }

            if (reconnect())
                return read_frame_blocking(out);

            return false;
        }
    }

    bool supports_bounded_idle_read() const override { return true; }

    transport_read_result read_frame_until(
        std::string_view& out,
        std::chrono::steady_clock::time_point deadline) override
    {
        if (!ws_ || stopped_.load()) return transport_read_result::terminal;
        const auto result = frame_reader_.read_until(ioc_, ws_, out, deadline);
        if (result != transport_read_result::terminal) return result;

        open_ = false;
        if (stopped_.load()) return result;
        if (auto fatal = fatal_cb_.load())
        {
            const auto ec = frame_reader_.last_error();
            char buf[160];
            std::snprintf(buf, sizeof(buf), "binance market-data WS lost: %s",
                          ec ? ec.message().c_str() : "bounded read failed");
            (*fatal)(buf);
            stopped_ = true;
            return result;
        }
        if (reconnect()) return read_frame_until(out, deadline);
        return result;
    }

    void request_stop() override
    {
        stopped_ = true;
        open_ = false;
        // May run on the signal-monitor thread. shutdown(2) touches only the
        // published native descriptor; it never races a Boost.Asio socket
        // method against the engine-owned async_read.
        (void)socket_interrupt_.request_shutdown();
    }

    bool reconnect_stream(const std::string& new_symbol,
                          const std::string& new_stream_type)
    {
        request_stop();

        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        symbol_ = new_symbol;
        stream_type_ = new_stream_type;

        stopped_ = false;

        ioc_->restart();
        socket_interrupt_.clear();
        ws_.reset();
        return open();
    }

    // Engine wires this in live mode. When set, a read/handshake error
    // routes here directly and the transport STOPS - no reconnect loop.
    // When unset (backtest/shadow paths), the existing reconnect-on-error
    // behaviour stands. The reason string is published verbatim through
    // engine::trigger_halt to the dashboard banner.
    void set_fatal_disconnect_callback(
        std::function<void(std::string_view reason)> cb)
    {
        fatal_cb_.store(std::move(cb));
    }

private:
    std::string symbol_;
    std::string stream_type_;
    std::string host_;
    std::string port_;

    std::shared_ptr<net::io_context> ioc_ = std::make_shared<net::io_context>();
    ssl::context ctx_;
    std::shared_ptr<websocket::stream<beast::ssl_stream<tcp::socket>>> ws_;

    beast::flat_buffer frame_buffer_;
    provider_ws::BoundedFrameReader<
        websocket::stream<beast::ssl_stream<tcp::socket>>> frame_reader_;

    std::mutex mu_;
    provider_io::native_socket_interrupt socket_interrupt_;
    std::atomic<bool> open_{false};
    std::atomic<bool> stopped_{false};

    ThreadSafeCallback<void(std::string_view)> fatal_cb_;

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
            ioc_->restart();
            socket_interrupt_.clear();
            ws_.reset();
            return open();
        }, cfg);
    }
};

#endif // HAS_BINANCE
