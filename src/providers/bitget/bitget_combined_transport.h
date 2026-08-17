#pragma once
#ifdef HAS_BITGET

#include "providers/bitget/bitget_endpoints.h"
#include "providers/bitget/bitget_transport.h"
#include "providers/transport.h"
#include "providers/thread_safe_callback.h"
#include "providers/bounded_ws_open.h"
#include "providers/bounded_ws_frame_reader.h"
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
#include <cstdio>
#include <functional>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
namespace ssl = net::ssl;
using tcp = net::ip::tcp;

namespace bitget {

// ---------------------------------------------------------------------------
// BitgetCombinedTransport — multi-subscribe public WS (UTA v3)
// ---------------------------------------------------------------------------
//
// One connection, multi args[] subscribe (e.g. trade + books5).
// Same path-based URL, text ping/pong (poll-before-read wake), and
// fatal-disconnect semantics as BitgetTransport. Stream names are CLI-facing
// (trade, books5, kline1m, …) via map_stream_to_topic.

class BitgetCombinedTransport : public IDataTransport
{
public:
    BitgetCombinedTransport(const std::string& symbol,
                            const std::vector<std::string>& streams,
                            const std::string& host = "ws.bitget.com",
                            const std::string& port = "443",
                            const std::string& path = "/v3/ws/public")
        : symbol_(to_upper_ascii(symbol))
        , streams_(streams)
        , host_(host)
        , port_(port)
        , path_(path)
        , ctx_(ssl::context::tlsv12_client)
    {
        ctx_.set_default_verify_paths();
        ctx_.set_verify_mode(ssl::verify_peer);
    }

    BitgetCombinedTransport(const std::string& symbol,
                            const std::vector<std::string>& streams,
                            const endpoints& ep)
        : BitgetCombinedTransport(symbol,
                                  streams,
                                  ep.ws_public_host,
                                  ep.ws_port,
                                  ep.ws_public_path)
    {
    }

    bool open() override
    {
        try
        {
            if (streams_.empty())
            {
                std::cerr << "BitgetCombinedTransport: no streams to subscribe\n";
                return false;
            }

            ws_ = std::make_shared<
                websocket::stream<beast::ssl_stream<tcp::socket>>>(*ioc_, ctx_);

            // TCP keepalive — see BitgetTransport for rationale.
            const bool opened = provider_ws::open_tls_websocket(
                *ioc_, *ws_, host_, port_, path_, std::chrono::seconds(3),
                [&](auto& socket) {
                auto& lowest = beast::get_lowest_layer(socket);
                const int fd = lowest.native_handle();
                const int yes = 1;
                const int idle = 1, intvl = 1, cnt = 2;
                ::setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &yes, sizeof(yes));
                ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
                ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
                ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
                },
                [&](auto& socket) {
                websocket::stream_base::timeout opt;
                opt.handshake_timeout = websocket::stream_base::none();
                opt.idle_timeout = websocket::stream_base::none();
                opt.keep_alive_pings = false;
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

            const std::string sub =
                build_subscribe_json_for_streams(symbol_, streams_);
            auto subscription = std::make_shared<std::string>(sub);
            const bool subscribed = provider_ws::run_bounded(
                *ioc_, std::chrono::seconds(3),
                [&, subscription](auto done) {
                    ws_->async_write(net::buffer(*subscription),
                        [subscription, done](beast::error_code ec,
                                             std::size_t) mutable {
                            done(ec);
                        });
                },
                [&] {
                    beast::error_code ignored;
                    auto& lowest = beast::get_lowest_layer(*ws_);
                    lowest.cancel(ignored);
                    lowest.close(ignored);
                });
            if (!subscribed) return false;
            socket_interrupt_.publish(
                beast::get_lowest_layer(*ws_).native_handle());
            heartbeat_.reset(std::chrono::steady_clock::now());
            frame_reader_.reset();

            open_ = true;
            stopped_ = false;
            return true;
        }
        catch (const std::exception& e)
        {
            std::cerr << "BitgetCombinedTransport: open failed: " << e.what()
                      << "\n";
            return false;
        }
    }

    // Owner-thread finalization after request_stop() has woken the read via
    // native shutdown. Boost.Asio socket methods remain on the owner thread.
    void close() override
    {
        stopped_.store(true);
        open_.store(false);

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

    bool is_streaming() const override
    {
        return true;
    }

    std::optional<std::string> read_line_blocking() override
    {
        std::string_view view;
        if (!read_frame_blocking(view))
            return std::nullopt;
        return std::string(view);
    }

    // `out` views frame_buffer_ — valid only until the next
    // read_frame_blocking / consume (caller must copy).
    bool read_frame_blocking(std::string_view& out) override
    {
        if (!ws_ || stopped_.load())
            return false;

        while (!stopped_.load())
        {
            try
            {
                if (!maybe_send_ping())
                    throw std::runtime_error(
                        "Bitget combined heartbeat pong timeout");

                // poll-before-read — see BitgetTransport / poll helpers.
                if (!wait_ready_for_read())
                {
                    if (stopped_.load())
                        return false;
                    if (!maybe_send_ping(/*force=*/true))
                        throw std::runtime_error(
                            "Bitget combined heartbeat ping failed");
                    continue;
                }

                if (frame_buffer_.size() > 0)
                    frame_buffer_.consume(frame_buffer_.size());

                ws_->read(frame_buffer_);

                if (stopped_.load())
                    return false;

                auto const_buf = frame_buffer_.data();
                // View into frame_buffer_; invalidated by next read/consume.
                out = std::string_view(
                    static_cast<const char*>(const_buf.data()),
                    const_buf.size());

                if (is_pong_text(out))
                {
                    heartbeat_.pong_received();
                    continue;
                }
                if (is_ping_text(out))
                {
                    send_text("pong");
                    continue;
                }

                return true;
            }
            catch (const beast::system_error& se)
            {
                const bool clean_close = (se.code() == websocket::error::closed);
                std::cerr << "BitgetCombinedTransport: websocket "
                          << (clean_close ? "closed by server" : "read error")
                          << " (" << se.code().message() << ")\n";

                open_ = false;

                if (stopped_.load())
                    return false;

                if (auto fatal = fatal_cb_.load())
                {
                    char buf[160];
                    std::snprintf(buf, sizeof(buf),
                                  "bitget combined WS lost: %s",
                                  se.code().message().c_str());
                    (*fatal)(buf);
                    stopped_ = true;
                    return false;
                }

                if (reconnect())
                    continue;

                return false;
            }
            catch (const std::exception& e)
            {
                std::cerr << "BitgetCombinedTransport: read error: " << e.what()
                          << "\n";
                open_ = false;

                if (stopped_.load())
                    return false;

                if (auto fatal = fatal_cb_.load())
                {
                    char buf[160];
                    std::snprintf(buf, sizeof(buf),
                                  "bitget combined WS lost: %s", e.what());
                    (*fatal)(buf);
                    stopped_ = true;
                    return false;
                }

                if (reconnect())
                    continue;

                return false;
            }
        }
        return false;
    }

    bool supports_bounded_idle_read() const override { return true; }

    transport_read_result read_frame_until(
        std::string_view& out,
        std::chrono::steady_clock::time_point deadline) override
    {
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (!ws_ || stopped_.load()) return transport_read_result::terminal;
            if (!maybe_send_ping())
                return fail_heartbeat("bitget combined heartbeat failed");
            const auto result = frame_reader_.read_until(ioc_, ws_, out, deadline);
            if (result == transport_read_result::idle) return result;
            if (result == transport_read_result::frame)
            {
                if (is_pong_text(out))
                {
                    heartbeat_.pong_received();
                    continue;
                }
                if (is_ping_text(out))
                {
                    send_text("pong");
                    continue;
                }
                return result;
            }

            open_ = false;
            if (stopped_.load()) return result;
            if (auto fatal = fatal_cb_.load())
            {
                const auto ec = frame_reader_.last_error();
                char buf[160];
                std::snprintf(buf, sizeof(buf), "bitget combined WS lost: %s",
                              ec ? ec.message().c_str() : "bounded read failed");
                (*fatal)(buf);
                stopped_ = true;
                return result;
            }
            if (!reconnect()) return result;
        }
        return transport_read_result::idle;
    }

    void request_stop() override
    {
        stopped_ = true;
        open_ = false;
        (void)socket_interrupt_.request_shutdown();
    }

    // Engine wires this in live mode — see BitgetTransport for semantics.
    void set_fatal_disconnect_callback(
        std::function<void(std::string_view reason)> cb)
    {
        fatal_cb_.store(std::move(cb));
    }

private:
    std::string symbol_;
    std::vector<std::string> streams_;
    std::string host_;
    std::string port_;
    std::string path_;

    std::shared_ptr<net::io_context> ioc_ = std::make_shared<net::io_context>();
    ssl::context ctx_;
    std::shared_ptr<websocket::stream<beast::ssl_stream<tcp::socket>>> ws_;
    provider_ws::BoundedFrameReader<
        websocket::stream<beast::ssl_stream<tcp::socket>>> frame_reader_;

    beast::flat_buffer frame_buffer_;

    std::mutex mu_;
    provider_io::native_socket_interrupt socket_interrupt_;
    std::atomic<bool> open_{false};
    std::atomic<bool> stopped_{false};

    ThreadSafeCallback<void(std::string_view)> fatal_cb_;

    TextHeartbeat heartbeat_;
    static constexpr auto kPollWake = std::chrono::seconds(25);
    static constexpr unsigned MAX_RECONNECTS = 5;

    void send_text(std::string_view text)
    {
        if (!ws_)
            return;
        ws_->write(net::buffer(text.data(), text.size()));
    }

    bool maybe_send_ping(bool force = false)
    {
        if (!ws_ || stopped_.load())
            return false;
        const auto now = std::chrono::steady_clock::now();
        const auto action = heartbeat_.poll(now, force);
        if (action == TextHeartbeat::action::failed) return false;
        if (action == TextHeartbeat::action::idle)
            return true;
        try
        {
            send_text("ping");
            heartbeat_.ping_sent(now);
            return true;
        }
        catch (const std::exception& e)
        {
            std::cerr << "BitgetCombinedTransport: ping failed: " << e.what()
                      << "\n";
            return false;
        }
    }

    transport_read_result fail_heartbeat(std::string_view reason) noexcept
    {
        open_ = false;
        stopped_ = true;
        if (auto fatal = fatal_cb_.load())
        {
            try { (*fatal)(reason); }
            catch (...) {}
        }
        return transport_read_result::terminal;
    }

    bool wait_ready_for_read()
    {
        if (!ws_ || stopped_.load())
            return false;
        if (ssl_has_pending_app_data(ws_->next_layer().native_handle()))
            return true;
        const auto wait = std::chrono::duration_cast<std::chrono::milliseconds>(
            kPollWake);
        const int fd = beast::get_lowest_layer(*ws_).native_handle();
        const auto pr = poll_fd_readable(fd, wait);
        if (pr == poll_wait_result::timeout)
        {
            return false;
        }
        return true;
    }

    bool reconnect()
    {
        return retry_with_backoff(
            [this]() {
                if (stopped_.load())
                    return true;
                ioc_->restart();
                socket_interrupt_.clear();
                ws_.reset();
                return open();
            },
            MAX_RECONNECTS,
            std::chrono::milliseconds(1000),
            std::chrono::milliseconds(16000));
    }
};

} // namespace bitget

using BitgetCombinedTransport = bitget::BitgetCombinedTransport;

#endif // HAS_BITGET
