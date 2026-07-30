#pragma once
#ifdef HAS_BYBIT

#include "providers/bybit/bybit_endpoints.h"
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
#include <charconv>
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

namespace bybit {

// ---------------------------------------------------------------------------
// Pure helpers (unit-testable without network)
// ---------------------------------------------------------------------------

// CLI / config stream → Bybit V5 public topic arg (without symbol suffix yet).
//   trade              → publicTrade
//   publicTrade        → publicTrade
//   orderbook.50       → orderbook.50
//   orderbook50        → orderbook.50
//   kline.1 / kline1   → kline.1
//   kline1m            → kline.1  (minutes strip trailing m)
//   kline1h / kline60  → kline.60
//   kline1d / klineD   → kline.D
struct mapped_topic
{
    std::string topic; // full arg except trailing .SYMBOL (e.g. publicTrade, orderbook.50)
};

inline std::string to_upper_ascii(std::string s)
{
    for (char& c : s)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

// Map CLI kline suffixes onto Bybit interval tokens (1,3,5,15,30,60,D,W,M).
inline std::string normalize_kline_interval(std::string_view interval)
{
    if (interval.empty()) return {};
    std::string iv(interval);
    // Strip leading dots.
    while (!iv.empty() && iv.front() == '.')
        iv.erase(iv.begin());
    if (iv.empty()) return {};

    // Common CLI forms: 1m, 5m, 1h, 4h, 1d, 1w, 1M
    if (iv.size() >= 2)
    {
        const char last = iv.back();
        std::string num = iv.substr(0, iv.size() - 1);
        bool digits = !num.empty();
        for (char c : num)
        {
            if (c < '0' || c > '9') { digits = false; break; }
        }
        if (digits)
        {
            if (last == 'm' || last == 'M')
            {
                // Minutes: Bybit uses bare number; "M" alone is month.
                if (last == 'M' && num == "1")
                    return "M";
                return num;
            }
            if (last == 'h' || last == 'H')
            {
                // Hours → minutes on Bybit linear kline.
                int h = 0;
                auto [p, ec] = std::from_chars(num.data(), num.data() + num.size(), h);
                if (ec == std::errc() && h > 0)
                    return std::to_string(h * 60);
            }
            if (last == 'd' || last == 'D')
                return "D";
            if (last == 'w' || last == 'W')
                return "W";
        }
    }
    // Already Bybit form: 1, 60, D, W, M
    if (iv == "d" || iv == "D") return "D";
    if (iv == "w" || iv == "W") return "W";
    if (iv == "M") return "M";
    return iv;
}

inline mapped_topic map_stream_to_topic(std::string_view stream)
{
    if (stream.empty() || stream == "trade" || stream == "publicTrade")
        return {"publicTrade"};

    // orderbook.{depth} or orderbook{depth}
    if (stream.size() >= 9 && stream.substr(0, 9) == "orderbook")
    {
        if (stream.size() == 9)
            return {"orderbook.50"}; // default depth
        if (stream[9] == '.')
            return {std::string(stream)};
        // orderbook50 → orderbook.50
        return {std::string("orderbook.") + std::string(stream.substr(9))};
    }
    // books* aliases (Bitget-style CLI comfort)
    if (stream == "books1")  return {"orderbook.1"};
    if (stream == "books50") return {"orderbook.50"};
    if (stream == "books")   return {"orderbook.50"};

    auto strip_prefix = [](std::string_view s,
                           std::string_view prefix) -> std::optional<std::string_view> {
        if (s.size() > prefix.size() && s.substr(0, prefix.size()) == prefix)
            return s.substr(prefix.size());
        return std::nullopt;
    };

    if (auto iv = strip_prefix(stream, "kline"))
    {
        // kline.1 already dotted, or kline1 / kline1m
        std::string_view raw = *iv;
        if (!raw.empty() && raw.front() == '.')
            raw.remove_prefix(1);
        auto norm = normalize_kline_interval(raw);
        if (norm.empty()) norm = "1";
        return {std::string("kline.") + norm};
    }
    if (auto iv = strip_prefix(stream, "candle"))
    {
        std::string_view raw = *iv;
        if (!raw.empty() && raw.front() == '.')
            raw.remove_prefix(1);
        auto norm = normalize_kline_interval(raw);
        if (norm.empty()) norm = "1";
        return {std::string("kline.") + norm};
    }

    // Pass-through already-mapped topics (e.g. "publicTrade", "orderbook.200").
    return {std::string(stream)};
}

// Build one args[] topic string: "{topic}.{SYMBOL}"
inline std::string build_topic_arg(std::string_view symbol, std::string_view topic)
{
    std::string arg;
    arg.reserve(topic.size() + 1 + symbol.size());
    arg.append(topic);
    arg.push_back('.');
    arg.append(symbol);
    return arg;
}

// {"op":"subscribe","args":["publicTrade.BTCUSDT"]}
inline std::string build_subscribe_json(std::string_view symbol,
                                        std::string_view topic)
{
    const auto arg = build_topic_arg(symbol, topic);
    std::string j;
    j.reserve(40 + arg.size());
    j += "{\"op\":\"subscribe\",\"args\":[\"";
    j += arg;
    j += "\"]}";
    return j;
}

inline std::string build_subscribe_json(std::string_view symbol,
                                        const std::vector<mapped_topic>& topics)
{
    std::string j = "{\"op\":\"subscribe\",\"args\":[";
    for (std::size_t i = 0; i < topics.size(); ++i)
    {
        if (i > 0) j += ',';
        j += '"';
        j += build_topic_arg(symbol, topics[i].topic);
        j += '"';
    }
    j += "]}";
    return j;
}

inline std::string build_subscribe_json_for_streams(
    std::string_view symbol,
    const std::vector<std::string>& streams)
{
    std::vector<mapped_topic> topics;
    topics.reserve(streams.size());
    for (const auto& s : streams)
        topics.push_back(map_stream_to_topic(s));
    return build_subscribe_json(symbol, topics);
}

inline std::string build_ping_json()
{
    return R"({"op":"ping"})";
}

// Bybit pong frames: {"op":"pong",...} or {"success":true,"ret_msg":"pong",...}
inline bool is_pong_frame(std::string_view msg)
{
    if (msg.find("\"op\":\"pong\"") != std::string_view::npos)
        return true;
    if (msg.find("\"ret_msg\":\"pong\"") != std::string_view::npos)
        return true;
    if (msg.find("\"ret_msg\": \"pong\"") != std::string_view::npos)
        return true;
    return false;
}

// Subscribe/auth acks — not market data.
inline bool is_control_ack(std::string_view msg)
{
    // {"success":true,"ret_msg":"subscribe",...} or op subscribe responses
    if (msg.find("\"op\":\"subscribe\"") != std::string_view::npos)
        return true;
    if (msg.find("\"ret_msg\":\"subscribe\"") != std::string_view::npos)
        return true;
    if (msg.find("\"ret_msg\": \"subscribe\"") != std::string_view::npos)
        return true;
    // Auth acks (private WS; harmless if seen on public)
    if (msg.find("\"op\":\"auth\"") != std::string_view::npos)
        return true;
    return false;
}

// ---------------------------------------------------------------------------
// Pure-sync WS wake helpers (poll-before-read) — same rationale as Bitget
// ---------------------------------------------------------------------------

enum class poll_wait_result
{
    ready,
    timeout,
    error,
};

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
// BybitTransport — single-topic public WS (V5 linear)
// ---------------------------------------------------------------------------
//
// Connect: wss://{host}:{port}/v5/public/linear
// Subscribe after handshake: {"op":"subscribe","args":["publicTrade.BTCUSDT"]}
// Heartbeat: app-level {"op":"ping"} every ~20s
// fatal_cb_ set → fail loud, no reconnect (live path). Unset → reconnect.

class BybitTransport : public IDataTransport
{
public:
    BybitTransport(const std::string& symbol,
                   const std::string& stream_type,
                   const std::string& host = "stream.bybit.com",
                   const std::string& port = "443",
                   const std::string& path = "/v5/public/linear")
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

    BybitTransport(const std::string& symbol,
                   const std::string& stream_type,
                   const endpoints& ep)
        : BybitTransport(symbol,
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
            net::connect(lowest, results);

            const int fd = lowest.native_handle();
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
                std::cerr << "BybitTransport: SNI setup failed\n";
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
                    req.set(boost::beast::http::field::user_agent, "TrueTest/1.0");
                }));

            ws_->control_callback(
                [](websocket::frame_type kind, beast::string_view) {
                    (void)kind;
                });

            ws_->handshake(host_ + ":" + port_, path_);

            const auto mapped = map_stream_to_topic(stream_type_);
            const std::string sub = build_subscribe_json(symbol_, mapped.topic);
            ws_->write(net::buffer(sub));
            last_ping_ = std::chrono::steady_clock::now();

            open_ = true;
            stopped_ = false;
            return true;
        }
        catch (const std::exception& e)
        {
            std::cerr << "BybitTransport: open failed: " << e.what() << "\n";
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

                if (is_pong_frame(out) || is_control_ack(out))
                    continue;

                return true;
            }
            catch (const beast::system_error& se)
            {
                const bool clean_close = (se.code() == websocket::error::closed);
                std::cerr << "BybitTransport: websocket "
                          << (clean_close ? "closed by server" : "read error")
                          << " (" << se.code().message() << ")\n";

                open_ = false;

                if (stopped_.load())
                    return false;

                if (fatal_cb_)
                {
                    char buf[160];
                    std::snprintf(buf, sizeof(buf),
                                  "bybit market-data WS lost: %s",
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
                std::cerr << "BybitTransport: read error: " << e.what() << "\n";
                open_ = false;

                if (stopped_.load())
                    return false;

                if (fatal_cb_)
                {
                    char buf[160];
                    std::snprintf(buf, sizeof(buf),
                                  "bybit market-data WS lost: %s", e.what());
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

    // Bybit docs: send ping every ~20s.
    static constexpr auto kPingInterval = std::chrono::seconds(20);
    static constexpr auto kPollWake = std::chrono::seconds(18);
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
            const auto ping = build_ping_json();
            send_text(ping);
            last_ping_ = now;
        }
        catch (const std::exception& e)
        {
            std::cerr << "BybitTransport: ping failed: " << e.what() << "\n";
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
            std::cerr << "BybitTransport: reconnecting (attempt " << attempt
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

} // namespace bybit

using BybitTransport = bybit::BybitTransport;

#endif // HAS_BYBIT
