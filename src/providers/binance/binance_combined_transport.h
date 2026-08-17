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

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <openssl/ssl.h>
#include <sys/socket.h>

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
namespace ssl = net::ssl;
using tcp = net::ip::tcp;

class BinanceCombinedTransport : public IDataTransport
{
public:
    BinanceCombinedTransport(
        const std::vector<std::string>& streams,
        const std::string& host = "stream.binance.com",
        const std::string& port = "9443")
        : streams_(streams)
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

            std::string target = "/stream?streams=";
            for (size_t i = 0; i < streams_.size(); ++i)
            {
                if (i > 0) target += "/";
                target += streams_[i];
            }

            // TCP keepalive on the underlying socket - see BinanceTransport
            // for rationale. 1s idle / 1s probe / 2 probes -> kernel-side
            // detection within ~3s. Best-effort.
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
            std::cerr << "BinanceCombinedTransport: open failed: " << e.what() << "\n";
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

    bool is_open() const override { return open_.load(); }

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

            open_ = false;

            if (stopped_.load())
                return std::nullopt;

            if (auto fatal = fatal_cb_.load())
            {
                char buf[160];
                std::snprintf(buf, sizeof(buf),
                              "binance combined WS lost: %s",
                              se.code().message().c_str());
                (*fatal)(buf);
                stopped_ = true;
                return std::nullopt;
            }

            if (reconnect())
                return read_line_blocking();

            return std::nullopt;
        }
        catch (const std::exception& e)
        {
            open_ = false;

            if (stopped_.load())
                return std::nullopt;

            if (auto fatal = fatal_cb_.load())
            {
                char buf[160];
                std::snprintf(buf, sizeof(buf),
                              "binance combined WS lost: %s", e.what());
                (*fatal)(buf);
                stopped_ = true;
            }
            return std::nullopt;
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
            std::snprintf(buf, sizeof(buf), "binance combined WS lost: %s",
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
        (void)socket_interrupt_.request_shutdown();
    }

    // Engine wires this in live mode - see BinanceTransport for semantics.
    void set_fatal_disconnect_callback(
        std::function<void(std::string_view reason)> cb)
    {
        fatal_cb_.store(std::move(cb));
    }

private:
    std::vector<std::string> streams_;
    std::string host_;
    std::string port_;

    std::shared_ptr<net::io_context> ioc_ = std::make_shared<net::io_context>();
    ssl::context ctx_;
    std::shared_ptr<websocket::stream<beast::ssl_stream<tcp::socket>>> ws_;
    provider_ws::BoundedFrameReader<
        websocket::stream<beast::ssl_stream<tcp::socket>>> frame_reader_;

    std::mutex mu_;
    provider_io::native_socket_interrupt socket_interrupt_;
    std::atomic<bool> open_{false};
    std::atomic<bool> stopped_{false};

    ThreadSafeCallback<void(std::string_view)> fatal_cb_;

    static constexpr unsigned MAX_RECONNECTS = 5;

    bool reconnect()
    {
        return retry_with_backoff([this]() {
            ioc_->restart();
            socket_interrupt_.clear();
            ws_.reset();
            return open();
        }, MAX_RECONNECTS,
           std::chrono::milliseconds(1000),
           std::chrono::milliseconds(16000));
    }
};

#endif // HAS_BINANCE
