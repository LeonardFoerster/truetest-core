#pragma once
#ifdef HAS_GATE

// Multi-channel public WS on one Gate futures socket (trades + book update).
// Same poll-before-read / futures.ping / fatal-disconnect semantics as
// GateTransport. One subscribe frame per channel after handshake.

#include "providers/gate/gate_endpoints.h"
#include "providers/gate/gate_transport.h"
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
#include <cstdio>
#include <functional>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
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

namespace gate {

class GateCombinedTransport : public IDataTransport
{
public:
    GateCombinedTransport(const std::string& symbol,
                          const std::vector<std::string>& streams,
                          const std::string& host = "fx-ws.gateio.ws",
                          const std::string& port = "443",
                          const std::string& path = "/v4/ws/usdt")
        : symbol_(symbol)
        , streams_(streams)
        , host_(host)
        , port_(port)
        , path_(path)
        , ctx_(ssl::context::tlsv12_client)
    {
        ctx_.set_default_verify_paths();
        ctx_.set_verify_mode(ssl::verify_peer);
    }

    GateCombinedTransport(const std::string& symbol,
                          const std::vector<std::string>& streams,
                          const endpoints& ep)
        : GateCombinedTransport(symbol,
                                streams,
                                ep.ws_host,
                                ep.ws_port,
                                ep.ws_path)
    {
    }

    bool open() override
    {
        try
        {
            if (streams_.empty())
            {
                std::cerr
                    << "GateCombinedTransport: no streams to subscribe\n";
                return false;
            }

            tcp::resolver resolver(ioc_);
            auto results = resolver.resolve(host_, port_);

            ws_ = std::make_unique<
                websocket::stream<beast::ssl_stream<tcp::socket>>>(ioc_,
                                                                  ctx_);

            auto& lowest = beast::get_lowest_layer(*ws_);
            net::connect(lowest, results);

            const int fd = lowest.native_handle();
            {
                const int yes = 1;
                const int idle = 1, intvl = 1, cnt = 2;
                ::setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &yes, sizeof(yes));
                ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle,
                             sizeof(idle));
                ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl,
                             sizeof(intvl));
                ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
            }

            if (!SSL_set_tlsext_host_name(
                    ws_->next_layer().native_handle(), host_.c_str()))
            {
                std::cerr << "GateCombinedTransport: SNI setup failed\n";
                return false;
            }

            ws_->next_layer().handshake(ssl::stream_base::client);

            {
                websocket::stream_base::timeout opt;
                opt.handshake_timeout = websocket::stream_base::none();
                opt.idle_timeout = websocket::stream_base::none();
                opt.keep_alive_pings = false;
                ws_->set_option(opt);
            }

            ws_->set_option(websocket::stream_base::decorator(
                [](websocket::request_type& req) {
                    req.set(boost::beast::http::field::user_agent,
                            "TrueTest/1.0");
                }));

            ws_->control_callback(
                [](websocket::frame_type kind, beast::string_view) {
                    (void)kind;
                });

            ws_->handshake(host_ + ":" + port_, path_);

            // One subscribe JSON per stream (Gate has no multi-args op).
            const auto subs =
                build_subscribe_jsons_for_streams(symbol_, streams_);
            for (const auto& sub : subs)
                ws_->write(net::buffer(sub));
            last_ping_ = std::chrono::steady_clock::now();

            open_ = true;
            stopped_ = false;
            return true;
        }
        catch (const std::exception& e)
        {
            std::cerr << "GateCombinedTransport: open failed: " << e.what()
                      << "\n";
            return false;
        }
    }

    void close() override
    {
        stopped_.store(true);
        open_.store(false);

        std::lock_guard<std::mutex> lk(mu_);
        if (ws_)
        {
            try
            {
                beast::error_code ec;
                auto& lowest = beast::get_lowest_layer(*ws_);
                lowest.cancel(ec);
                lowest.close(ec);
            }
            catch (...)
            {
            }
        }
    }

    bool is_open() const override { return open_.load(); }

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

        while (!stopped_.load())
        {
            try
            {
                maybe_send_ping();

                if (!wait_ready_for_read())
                {
                    if (stopped_.load())
                        return false;
                    maybe_send_ping(/*force=*/true);
                    continue;
                }

                if (frame_buffer_.size() > 0)
                    frame_buffer_.consume(frame_buffer_.size());

                ws_->read(frame_buffer_);

                if (stopped_.load())
                    return false;

                auto const_buf = frame_buffer_.data();
                out = std::string_view(
                    static_cast<const char*>(const_buf.data()),
                    const_buf.size());

                if (is_pong_frame(out))
                    continue;
                if (is_ping_frame(out))
                {
                    send_text(build_ping_json());
                    continue;
                }

                return true;
            }
            catch (const beast::system_error& se)
            {
                const bool clean_close =
                    (se.code() == websocket::error::closed);
                std::cerr << "GateCombinedTransport: websocket "
                          << (clean_close ? "closed by server" : "read error")
                          << " (" << se.code().message() << ")\n";

                open_ = false;
                if (stopped_.load())
                    return false;

                if (fatal_cb_)
                {
                    char buf[160];
                    std::snprintf(buf, sizeof(buf),
                                  "gate combined WS lost: %s",
                                  se.code().message().c_str());
                    fatal_cb_(buf);
                    stopped_ = true;
                    return false;
                }

                if (reconnect())
                    continue;
                return false;
            }
            catch (const std::exception& e)
            {
                std::cerr << "GateCombinedTransport: read error: "
                          << e.what() << "\n";
                open_ = false;
                if (stopped_.load())
                    return false;

                if (fatal_cb_)
                {
                    char buf[160];
                    std::snprintf(buf, sizeof(buf),
                                  "gate combined WS lost: %s", e.what());
                    fatal_cb_(buf);
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

    void request_stop() override
    {
        stopped_ = true;
        close();
    }

    void set_fatal_disconnect_callback(
        std::function<void(std::string_view reason)> cb)
    {
        fatal_cb_ = std::move(cb);
    }

private:
    std::string symbol_;
    std::vector<std::string> streams_;
    std::string host_;
    std::string port_;
    std::string path_;

    net::io_context ioc_;
    ssl::context ctx_;
    std::unique_ptr<websocket::stream<beast::ssl_stream<tcp::socket>>> ws_;

    beast::flat_buffer frame_buffer_;

    std::mutex mu_;
    std::atomic<bool> open_{false};
    std::atomic<bool> stopped_{false};

    std::function<void(std::string_view)> fatal_cb_;

    std::chrono::steady_clock::time_point last_ping_{};

    static constexpr auto kPingInterval = std::chrono::seconds(30);
    static constexpr auto kPollWake = std::chrono::seconds(25);
    static constexpr unsigned MAX_RECONNECTS = 5;

    void send_text(std::string_view text)
    {
        if (!ws_)
            return;
        ws_->write(net::buffer(text.data(), text.size()));
    }

    void maybe_send_ping(bool force = false)
    {
        if (!ws_ || stopped_.load())
            return;
        const auto now = std::chrono::steady_clock::now();
        if (!force && (now - last_ping_) < kPingInterval)
            return;
        try
        {
            send_text(build_ping_json());
            last_ping_ = now;
        }
        catch (const std::exception& e)
        {
            std::cerr << "GateCombinedTransport: ping failed: " << e.what()
                      << "\n";
        }
    }

    bool wait_ready_for_read()
    {
        if (!ws_ || stopped_.load())
            return false;
        if (ssl_has_pending_app_data(ws_->next_layer().native_handle()))
            return true;
        const int fd = beast::get_lowest_layer(*ws_).native_handle();
        const auto pr = poll_fd_readable(fd, kPollWake);
        if (pr == poll_wait_result::timeout)
            return false;
        return true;
    }

    bool reconnect()
    {
        return retry_with_backoff(
            [this]() {
                if (stopped_.load())
                    return true;
                ioc_.restart();
                ws_.reset();
                return open();
            },
            MAX_RECONNECTS,
            std::chrono::milliseconds(1000),
            std::chrono::milliseconds(16000));
    }
};

} // namespace gate

using GateCombinedTransport = gate::GateCombinedTransport;

#endif // HAS_GATE
