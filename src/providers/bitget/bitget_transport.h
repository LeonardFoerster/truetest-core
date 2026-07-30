#pragma once
#ifdef HAS_BITGET

#include "providers/bitget/bitget_endpoints.h"
#include "providers/transport.h"
#include "utils/retry.h"

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>

#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
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

namespace bitget {

// ---------------------------------------------------------------------------
// Pure helpers (unit-testable without network)
// ---------------------------------------------------------------------------

// CLI / config stream name → UTA v3 public topic (+ optional kline interval).
struct mapped_topic
{
    std::string topic;
    std::string interval; // empty unless topic == "kline"
};

// Map CLI stream names (plan §7.5) onto Bitget UTA public topics.
//   trade              → publicTrade
//   ticker             → ticker
//   kline1m / candle1m → kline + interval 1m
//   books1|books5|…    → identity
// Unknown names pass through as the topic (already-mapped publicTrade etc.).
inline mapped_topic map_stream_to_topic(std::string_view stream)
{
    if (stream == "trade")
        return {"publicTrade", {}};
    if (stream == "ticker")
        return {"ticker", {}};
    if (stream == "books1" || stream == "books5" || stream == "books50" ||
        stream == "books")
        return {std::string(stream), {}};

    auto strip_prefix = [](std::string_view s,
                           std::string_view prefix) -> std::optional<std::string_view> {
        if (s.size() > prefix.size() && s.substr(0, prefix.size()) == prefix)
            return s.substr(prefix.size());
        return std::nullopt;
    };

    if (auto iv = strip_prefix(stream, "kline"))
        return {"kline", std::string(*iv)};
    if (auto iv = strip_prefix(stream, "candle"))
        return {"kline", std::string(*iv)};

    return {std::string(stream), {}};
}

// One args[] element. Compact JSON, no spaces (deterministic for tests).
inline std::string build_subscribe_arg(std::string_view symbol,
                                       std::string_view topic,
                                       std::string_view interval = {},
                                       std::string_view inst_type = "usdt-futures")
{
    std::string arg;
    arg.reserve(96 + symbol.size() + topic.size() + interval.size());
    arg += "{\"instType\":\"";
    arg += inst_type;
    arg += "\",\"topic\":\"";
    arg += topic;
    arg += "\",\"symbol\":\"";
    arg += symbol;
    arg += '"';
    if (!interval.empty())
    {
        arg += ",\"interval\":\"";
        arg += interval;
        arg += '"';
    }
    arg += '}';
    return arg;
}

// Single-topic subscribe frame:
// {"op":"subscribe","args":[{...}]}
inline std::string build_subscribe_json(std::string_view symbol,
                                        std::string_view topic,
                                        std::string_view interval = {},
                                        std::string_view inst_type = "usdt-futures")
{
    std::string j;
    j.reserve(48 + symbol.size() + topic.size() + interval.size());
    j += "{\"op\":\"subscribe\",\"args\":[";
    j += build_subscribe_arg(symbol, topic, interval, inst_type);
    j += "]}";
    return j;
}

// Multi-topic subscribe (combined transport).
inline std::string build_subscribe_json(std::string_view symbol,
                                        const std::vector<mapped_topic>& topics,
                                        std::string_view inst_type = "usdt-futures")
{
    std::string j = "{\"op\":\"subscribe\",\"args\":[";
    for (std::size_t i = 0; i < topics.size(); ++i)
    {
        if (i > 0)
            j += ',';
        j += build_subscribe_arg(symbol, topics[i].topic, topics[i].interval,
                                 inst_type);
    }
    j += "]}";
    return j;
}

// Map CLI streams then build multi-subscribe JSON.
inline std::string build_subscribe_json_for_streams(
    std::string_view symbol,
    const std::vector<std::string>& streams,
    std::string_view inst_type = "usdt-futures")
{
    std::vector<mapped_topic> topics;
    topics.reserve(streams.size());
    for (const auto& s : streams)
        topics.push_back(map_stream_to_topic(s));
    return build_subscribe_json(symbol, topics, inst_type);
}

inline std::string to_upper_ascii(std::string s)
{
    for (char& c : s)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

inline bool is_ping_text(std::string_view msg)
{
    return msg == "ping";
}

inline bool is_pong_text(std::string_view msg)
{
    return msg == "pong";
}

// ---------------------------------------------------------------------------
// Pure-sync WS wake helpers (poll-before-read)
// ---------------------------------------------------------------------------
//
// Why not Beast timeouts / SO_RCVTIMEO?
// - Beast `stream_base::timeout` is **async-only** (stream_base.hpp).
// - SO_RCVTIMEO does **not** bound Asio synchronous I/O: Asio drives the
//   socket non-blocking and, on EAGAIN, polls with an *infinite* timeout
//   (see binance_rest_client.h). Kernel socket timeouts never surface.
//
// Real silent-market wake: `poll(fd, POLLIN, ~25s)` *before* `ws_->read()`.
// If poll times out → send raw text `"ping"` and loop. If readable (or TLS
// already has app data via SSL_pending) → `ws_->read()`.
//
// Caveats (honest):
// - TCP connect + TLS + WS handshake on this pure-sync path have **no**
//   wall-clock bound (only kernel TCP retransmit / peer close).
// - Once inside `ws_->read()`, a partial WebSocket frame can still block
//   until more bytes arrive; poll only guards the *start* of each message.
// - Single consumer thread owns read + write (no concurrent Beast writes).

enum class poll_wait_result
{
    ready,   // POLLIN / POLLHUP — caller should read
    timeout, // no socket activity within timeout
    error,   // poll failure or POLLERR/POLLNVAL
};

// Unit-testable poll wrapper (pass any fd; transports use the WS socket).
inline poll_wait_result poll_fd_readable(int fd, std::chrono::milliseconds timeout)
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

        // Error conditions on the fd.
        if (pfd.revents & (POLLERR | POLLNVAL))
            return poll_wait_result::error;
        // Readable data or peer hangup (read will surface clean close).
        if (pfd.revents & (POLLIN | POLLHUP))
            return poll_wait_result::ready;
        return poll_wait_result::error;
    }
}

// True when OpenSSL already holds decrypted app bytes (poll would miss them).
inline bool ssl_has_pending_app_data(SSL* ssl)
{
    return ssl != nullptr && ::SSL_pending(ssl) > 0;
}

// ---------------------------------------------------------------------------
// BitgetTransport — single-topic public WS (UTA v3)
// ---------------------------------------------------------------------------
//
// Connect path: wss://{host}:{port}{path}  (default /v3/ws/public)
// After handshake: JSON subscribe with lowercase instType "usdt-futures".
// Heartbeat: raw text "ping" every ~30s → expect "pong" (not JSON / not WS ping).
// Silent markets: poll-before-read (~25s) → app text "ping" (see helpers above).
// fatal_cb_ set → fail loud, no reconnect (live path). Unset → reconnect.

class BitgetTransport : public IDataTransport
{
public:
    BitgetTransport(const std::string& symbol,
                    const std::string& stream_type,
                    const std::string& host = "ws.bitget.com",
                    const std::string& port = "443",
                    const std::string& path = "/v3/ws/public")
        : symbol_(to_upper_ascii(symbol))
        , stream_type_(stream_type)
        , host_(host)
        , port_(port)
        , path_(path)
        , ctx_(ssl::context::tlsv12_client)
    {
        ctx_.set_default_verify_paths();
        ctx_.set_verify_mode(ssl::verify_peer);
    }

    BitgetTransport(const std::string& symbol,
                    const std::string& stream_type,
                    const endpoints& ep)
        : BitgetTransport(symbol,
                          stream_type,
                          ep.ws_public_host,
                          ep.ws_port,
                          ep.ws_public_path)
    {
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
            // No wall-clock bound on resolve/connect/TLS/WS upgrade on this
            // pure-sync path (Asio sync I/O + SO_*TIMEO are ineffective;
            // see poll helpers comment). Kernel TCP retransmit is the floor.
            net::connect(lowest, results);

            const int fd = lowest.native_handle();

            // TCP keepalive: kernel-side cable-pull detection (~3s) when the
            // app-level text ping path is itself wedged. Best-effort.
            {
                const int yes = 1;
                const int idle = 1, intvl = 1, cnt = 2;
                ::setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &yes, sizeof(yes));
                ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
                ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
                ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
            }

            if (!SSL_set_tlsext_host_name(
                    ws_->next_layer().native_handle(), host_.c_str()))
            {
                std::cerr << "BitgetTransport: SNI setup failed\n";
                return false;
            }

            ws_->next_layer().handshake(ssl::stream_base::client);

            // Beast stream_base::timeout is async-only — leave none() so it is
            // never mistaken for a real bound on sync tcp::socket ops.
            {
                websocket::stream_base::timeout opt;
                opt.handshake_timeout = websocket::stream_base::none();
                opt.idle_timeout = websocket::stream_base::none();
                opt.keep_alive_pings = false;
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

            // Path-based URL (not Binance /ws/symbol@stream).
            ws_->handshake(host_ + ":" + port_, path_);

            // Subscribe after handshake.
            const auto mapped = map_stream_to_topic(stream_type_);
            const std::string sub =
                build_subscribe_json(symbol_, mapped.topic, mapped.interval);
            ws_->write(net::buffer(sub));
            last_ping_ = std::chrono::steady_clock::now();

            open_ = true;
            stopped_ = false;
            return true;
        }
        catch (const std::exception& e)
        {
            std::cerr << "BitgetTransport: open failed: " << e.what() << "\n";
            return false;
        }
    }

    // Foreign-thread stop: set flags, then interrupt via lowest-layer socket
    // cancel/close only. Beast websocket::stream is NOT thread-safe — do not
    // call ws_->close() while a reader may still be in read_frame_blocking.
    // Stream reset happens on reconnect / open after stop.
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
                // Continuous markets: still need text "ping" every ~30s.
                maybe_send_ping();

                // Silent markets: do not call blocking ws_->read until the
                // socket (or TLS buffer) has data — otherwise we cannot ping.
                if (!wait_ready_for_read())
                {
                    if (stopped_.load())
                        return false;
                    // poll timeout → app ping, re-enter (not a disconnect).
                    maybe_send_ping(/*force=*/true);
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

                // Bitget heartbeat: ignore pong; answer server ping with pong.
                if (is_pong_text(out))
                    continue;
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
                std::cerr << "BitgetTransport: websocket "
                          << (clean_close ? "closed by server" : "read error")
                          << " (" << se.code().message() << ")\n";

                open_ = false;

                if (stopped_.load())
                    return false;

                if (fatal_cb_)
                {
                    char buf[160];
                    std::snprintf(buf, sizeof(buf),
                                  "bitget market-data WS lost: %s",
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
                std::cerr << "BitgetTransport: read error: " << e.what() << "\n";
                open_ = false;

                if (stopped_.load())
                    return false;

                if (fatal_cb_)
                {
                    char buf[160];
                    std::snprintf(buf, sizeof(buf),
                                  "bitget market-data WS lost: %s", e.what());
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

    bool reconnect_stream(const std::string& new_symbol,
                          const std::string& new_stream_type)
    {
        request_stop();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        symbol_ = to_upper_ascii(new_symbol);
        stream_type_ = new_stream_type;
        stopped_ = false;
        ioc_.restart();
        ws_.reset();
        return open();
    }

    // Engine wires this in live mode. When set, a read/handshake error
    // routes here and the transport STOPS — no reconnect loop.
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
    // poll() timeout when waiting for the next frame under silence.
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
            send_text("ping");
            last_ping_ = now;
        }
        catch (const std::exception& e)
        {
            std::cerr << "BitgetTransport: ping failed: " << e.what() << "\n";
        }
    }

    // true  → safe to call ws_->read() (socket readable, TLS pending, or
    //         POLLERR so read can surface the real error).
    // false → poll timed out under silence (caller sends text "ping") or stop.
    bool wait_ready_for_read()
    {
        if (!ws_ || stopped_.load())
            return false;

        // TLS may already hold decrypted bytes that poll cannot see.
        if (ssl_has_pending_app_data(ws_->next_layer().native_handle()))
            return true;

        const int fd = beast::get_lowest_layer(*ws_).native_handle();
        const auto pr = poll_fd_readable(fd, kPollWake);
        if (pr == poll_wait_result::timeout)
            return false;
        // ready or error → let ws_->read report disconnect / deliver data.
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
            std::cerr << "BitgetTransport: reconnecting (attempt " << attempt
                      << "/" << MAX_RECONNECTS << ")\n";
        };

        return retry_with_backoff(
            [this]() {
                if (stopped_.load())
                    return true; // bail out of retry loop
                ioc_.restart();
                ws_.reset();
                return open();
            },
            cfg);
    }
};

} // namespace bitget

// Bring class into global namespace for IDataTransport users that construct
// BitgetTransport without the bitget:: prefix (mirrors BinanceTransport).
using BitgetTransport = bitget::BitgetTransport;

#endif // HAS_BITGET
