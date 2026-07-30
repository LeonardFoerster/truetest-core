#pragma once
#ifdef HAS_GATE

// Gate.io public market WebSocket (USDT-M futures).
// Connect → subscribe JSON → poll-before-read + futures.ping heartbeat.
// Pure subscribe helpers are unit-testable without network.

#include "providers/gate/gate_endpoints.h"
#include "providers/transport.h"
#include "utils/retry.h"

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <functional>
#include <iostream>
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
#include <poll.h>
#include <sys/socket.h>

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
namespace ssl = net::ssl;
using tcp = net::ip::tcp;

namespace gate {

// ---------------------------------------------------------------------------
// Pure helpers (unit-testable without network)
// ---------------------------------------------------------------------------

struct mapped_channel
{
    std::string channel;  // e.g. futures.trades
    std::string interval; // candlesticks only (e.g. 1m)
    std::string depth_level; // order_book_update level (e.g. 100)
    std::string depth_freq;  // order_book_update freq (e.g. 100ms)
};

// CLI stream name → Gate public channel.
//   trades / trade          → futures.trades
//   order_book_update / depth / books → futures.order_book_update
//   100ms:100 / depth:100ms:100 → order_book_update with freq+level
//   kline1m / candle1m / candlesticks → futures.candlesticks
// Unknown names pass through as channel.
inline mapped_channel map_stream_to_channel(std::string_view stream)
{
    mapped_channel m;
    if (stream.empty() || stream == "trades" || stream == "trade")
    {
        m.channel = "futures.trades";
        return m;
    }
    if (stream == "order_book_update" || stream == "depth"
        || stream == "books" || stream == "book")
    {
        m.channel = "futures.order_book_update";
        m.depth_freq = "100ms";
        m.depth_level = "100";
        return m;
    }

    // depth_spec form: "100ms:100" or "20ms:20"
    {
        auto colon = stream.find(':');
        if (colon != std::string_view::npos
            && stream.find("ms") != std::string_view::npos)
        {
            m.channel = "futures.order_book_update";
            m.depth_freq.assign(stream.substr(0, colon));
            m.depth_level.assign(stream.substr(colon + 1));
            return m;
        }
    }

    auto strip_prefix =
        [](std::string_view s,
           std::string_view prefix) -> std::optional<std::string_view> {
        if (s.size() > prefix.size()
            && s.substr(0, prefix.size()) == prefix)
            return s.substr(prefix.size());
        return std::nullopt;
    };

    if (auto iv = strip_prefix(stream, "kline"))
    {
        m.channel = "futures.candlesticks";
        m.interval.assign(iv->data(), iv->size());
        return m;
    }
    if (auto iv = strip_prefix(stream, "candle"))
    {
        m.channel = "futures.candlesticks";
        m.interval.assign(iv->data(), iv->size());
        return m;
    }
    if (stream == "candlesticks" || stream == "candles")
    {
        m.channel = "futures.candlesticks";
        m.interval = "1m";
        return m;
    }

    // Already a Gate channel name.
    if (stream.find("futures.") == 0)
    {
        m.channel.assign(stream.data(), stream.size());
        return m;
    }

    m.channel.assign(stream.data(), stream.size());
    return m;
}

// Unix time in seconds (Gate WS `time` field).
inline std::int64_t unix_time_s()
{
    return static_cast<std::int64_t>(std::time(nullptr));
}

// Single-channel subscribe frame:
// {"time":T,"channel":"futures.trades","event":"subscribe","payload":["BTC_USDT"]}
// order_book_update payload: ["BTC_USDT","100ms","100"]
// candlesticks payload: ["1m","BTC_USDT"]
inline std::string build_subscribe_json(std::string_view symbol,
                                        const mapped_channel& ch,
                                        std::int64_t time_s = 0)
{
    if (time_s <= 0)
        time_s = unix_time_s();

    char tbuf[32];
    std::snprintf(tbuf, sizeof(tbuf), "%lld",
                  static_cast<long long>(time_s));

    std::string j;
    j.reserve(128 + symbol.size() + ch.channel.size());
    j += "{\"time\":";
    j += tbuf;
    j += ",\"channel\":\"";
    j += ch.channel;
    j += "\",\"event\":\"subscribe\",\"payload\":[";

    if (ch.channel == "futures.candlesticks"
        || ch.channel == "futures.candle")
    {
        const std::string iv =
            ch.interval.empty() ? std::string("1m") : ch.interval;
        j += '"';
        j += iv;
        j += "\",\"";
        j += symbol;
        j += '"';
    }
    else if (ch.channel == "futures.order_book_update"
             || ch.channel == "futures.order_book")
    {
        const std::string freq =
            ch.depth_freq.empty() ? std::string("100ms") : ch.depth_freq;
        const std::string level =
            ch.depth_level.empty() ? std::string("100") : ch.depth_level;
        j += '"';
        j += symbol;
        j += "\",\"";
        j += freq;
        j += "\",\"";
        j += level;
        j += '"';
    }
    else
    {
        // trades and default: ["BTC_USDT"]
        j += '"';
        j += symbol;
        j += '"';
    }
    j += "]}";
    return j;
}

inline std::string build_subscribe_json(std::string_view symbol,
                                        std::string_view stream,
                                        std::int64_t time_s = 0)
{
    return build_subscribe_json(symbol, map_stream_to_channel(stream),
                                time_s);
}

// Multi-channel: one subscribe frame per channel (Gate has no multi-args
// op). Combined transport sends them sequentially after handshake.
inline std::vector<std::string>
build_subscribe_jsons_for_streams(std::string_view symbol,
                                  const std::vector<std::string>& streams,
                                  std::int64_t time_s = 0)
{
    std::vector<std::string> out;
    out.reserve(streams.size());
    for (const auto& s : streams)
        out.push_back(build_subscribe_json(symbol, s, time_s));
    return out;
}

// futures.ping request body.
inline std::string build_ping_json(std::int64_t time_s = 0)
{
    if (time_s <= 0)
        time_s = unix_time_s();
    char buf[80];
    std::snprintf(buf, sizeof(buf),
                  "{\"time\":%lld,\"channel\":\"futures.ping\"}",
                  static_cast<long long>(time_s));
    return std::string(buf);
}

inline bool is_pong_frame(std::string_view msg)
{
    // {"channel":"futures.pong",...} or bare channel match.
    return msg.find("futures.pong") != std::string_view::npos;
}

inline bool is_ping_frame(std::string_view msg)
{
    // Server may push futures.ping — answer with pong-shaped client ping
    // is not required; we just re-ping. Detect so caller can skip.
    // Avoid matching our own subscribe containing "ping" substring falsely:
    // require channel value.
    auto pos = msg.find("\"channel\"");
    if (pos == std::string_view::npos) return false;
    return msg.find("futures.ping", pos) != std::string_view::npos
        && msg.find("futures.pong", pos) == std::string_view::npos;
}

// Subscribe ack / non-data control frame (skip in parser path at transport
// level only when clearly not an update).
inline bool is_control_event_frame(std::string_view msg)
{
    // event":"subscribe" without result trades — still deliver to parser;
    // parser drops. Keep transport simple: only filter ping/pong.
    (void)msg;
    return false;
}

// ---------------------------------------------------------------------------
// Pure-sync WS wake helpers (poll-before-read) — same rationale as Bitget.
// ---------------------------------------------------------------------------

enum class poll_wait_result
{
    ready,
    timeout,
    error,
};

inline poll_wait_result poll_fd_readable(int fd,
                                         std::chrono::milliseconds timeout)
{
    if (fd < 0)
        return poll_wait_result::error;

    pollfd pfd{};
    pfd.fd = fd;
    pfd.events = POLLIN;

    int ms = static_cast<int>(timeout.count());
    if (ms < 0)
        ms = 0;

    for (;;)
    {
        const int rc = ::poll(&pfd, 1, ms);
        if (rc < 0)
        {
            if (errno == EINTR)
                continue;
            return poll_wait_result::error;
        }
        if (rc == 0)
            return poll_wait_result::timeout;
        if (pfd.revents & (POLLERR | POLLNVAL))
            return poll_wait_result::error;
        if (pfd.revents & (POLLIN | POLLHUP))
            return poll_wait_result::ready;
        return poll_wait_result::error;
    }
}

inline bool ssl_has_pending_app_data(SSL* ssl)
{
    return ssl != nullptr && ::SSL_pending(ssl) > 0;
}

// ---------------------------------------------------------------------------
// GateTransport — single-channel public WS
// ---------------------------------------------------------------------------

class GateTransport : public IDataTransport
{
public:
    GateTransport(const std::string& symbol,
                  const std::string& stream_type,
                  const std::string& host = "fx-ws.gateio.ws",
                  const std::string& port = "443",
                  const std::string& path = "/v4/ws/usdt")
        : symbol_(symbol)
        , stream_type_(stream_type)
        , host_(host)
        , port_(port)
        , path_(path)
        , ctx_(ssl::context::tlsv12_client)
    {
        ctx_.set_default_verify_paths();
        ctx_.set_verify_mode(ssl::verify_peer);
    }

    GateTransport(const std::string& symbol,
                  const std::string& stream_type,
                  const endpoints& ep)
        : GateTransport(symbol,
                        stream_type,
                        ep.ws_host,
                        ep.ws_port,
                        ep.ws_path)
    {
    }

    bool open() override
    {
        try
        {
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
                std::cerr << "GateTransport: SNI setup failed\n";
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

            const std::string sub =
                build_subscribe_json(symbol_, stream_type_);
            ws_->write(net::buffer(sub));
            last_ping_ = std::chrono::steady_clock::now();

            open_ = true;
            stopped_ = false;
            return true;
        }
        catch (const std::exception& e)
        {
            std::cerr << "GateTransport: open failed: " << e.what() << "\n";
            return false;
        }
    }

    // Foreign-thread stop: flags + lowest-layer cancel only (Beast not
    // thread-safe for concurrent protocol close).
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
                    // Reply with client ping (Gate accepts either side).
                    send_text(build_ping_json());
                    continue;
                }

                return true;
            }
            catch (const beast::system_error& se)
            {
                const bool clean_close =
                    (se.code() == websocket::error::closed);
                std::cerr << "GateTransport: websocket "
                          << (clean_close ? "closed by server" : "read error")
                          << " (" << se.code().message() << ")\n";

                open_ = false;
                if (stopped_.load())
                    return false;

                if (fatal_cb_)
                {
                    char buf[160];
                    std::snprintf(buf, sizeof(buf),
                                  "gate market-data WS lost: %s",
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
                std::cerr << "GateTransport: read error: " << e.what()
                          << "\n";
                open_ = false;
                if (stopped_.load())
                    return false;

                if (fatal_cb_)
                {
                    char buf[160];
                    std::snprintf(buf, sizeof(buf),
                                  "gate market-data WS lost: %s", e.what());
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
    std::string stream_type_;
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
    static constexpr unsigned MAX_RECONNECTS = 30;

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
            std::cerr << "GateTransport: ping failed: " << e.what() << "\n";
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
        retry_config cfg;
        cfg.max_attempts = MAX_RECONNECTS;
        cfg.initial_delay = std::chrono::milliseconds(1000);
        cfg.max_delay = std::chrono::milliseconds(30000);
        cfg.on_retry = [this](unsigned attempt, std::exception_ptr) {
            if (stopped_.load())
                return;
            std::cerr << "GateTransport: reconnecting (attempt " << attempt
                      << "/" << MAX_RECONNECTS << ")\n";
        };

        return retry_with_backoff(
            [this]() {
                if (stopped_.load())
                    return true;
                ioc_.restart();
                ws_.reset();
                return open();
            },
            cfg);
    }
};

} // namespace gate

using GateTransport = gate::GateTransport;

#endif // HAS_GATE
