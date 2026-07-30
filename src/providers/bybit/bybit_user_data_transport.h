#pragma once
#ifdef HAS_BYBIT

#include "execution/fill_transport.h"
#include "providers/bybit/bybit_auth.h"
#include "providers/bybit/bybit_endpoints.h"
#include "providers/bybit/bybit_parser.h"
#include "providers/bybit/bybit_transport.h"

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <functional>
#include <iostream>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <netinet/in.h>
#include <netinet/tcp.h>
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

// Minimal JSON string escape for auth fields (RFC 8259 subset).
inline void append_json_escaped(std::string& out, std::string_view s)
{
    out.reserve(out.size() + s.size() + 8);
    for (unsigned char c : s)
    {
        switch (c)
        {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b";  break;
        case '\f': out += "\\f";  break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:
            if (c < 0x20)
            {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x",
                              static_cast<unsigned>(c));
                out += buf;
            }
            else
            {
                out.push_back(static_cast<char>(c));
            }
            break;
        }
    }
}

// Private WS auth frame:
//   {"op":"auth","args":["<api_key>", <expires_ms>, "<sig>"]}
// sig = HMAC_SHA256(secret, "GET/realtime" + expires)
// expires is a JSON number (ms).
inline std::string build_auth_json(std::string_view api_key,
                                   std::int64_t expires_ms,
                                   std::string_view sign)
{
    std::string j;
    j.reserve(64 + api_key.size() + sign.size() + 24);
    j += "{\"op\":\"auth\",\"args\":[\"";
    append_json_escaped(j, api_key);
    j += "\",";
    j += std::to_string(expires_ms);
    j += ",\"";
    append_json_escaped(j, sign);
    j += "\"]}";
    return j;
}

// Categorised private topics for linear USDT-M (do not mix with all-in-one).
inline std::string build_private_subscribe_json()
{
    return R"({"op":"subscribe","args":["order.linear","execution.linear","position.linear","wallet"]})";
}

// Auth ack: op=="auth" and success true (or ret_msg empty / "OK").
inline bool is_auth_success(std::string_view msg)
{
    if (msg.empty()) return false;

    auto op = extract_sv_string(msg, "op");
    if (op != "auth")
        return false;

    // success:true (bool) or "success":true
    if (auto b = extract_sv_optional_bool(msg, "success"))
        return *b;

    // Some gateways echo retCode 0.
    auto code = extract_sv_number(msg, "retCode");
    if (code.empty())
        code = extract_sv_string(msg, "retCode");
    if (!code.empty() && code == "0")
        return true;

    return false;
}

// Auth rejected (op auth with success false or non-zero code).
inline bool is_auth_failure(std::string_view msg)
{
    if (msg.empty()) return false;

    auto op = extract_sv_string(msg, "op");
    if (op != "auth")
        return false;

    if (auto b = extract_sv_optional_bool(msg, "success"))
        return !*b;

    auto code = extract_sv_number(msg, "retCode");
    if (code.empty())
        code = extract_sv_string(msg, "retCode");
    if (!code.empty() && code != "0")
        return true;

    return false;
}

// ---------------------------------------------------------------------------
// BybitUserDataTransport — IFillTransport for V5 private WS
// ---------------------------------------------------------------------------
//
// Connect wss://{ws_private_host}/v5/private → auth → subscribe
// order.linear / execution.linear / position.linear / wallet.
// App-level {"op":"ping"} every ~20s (no listenKey).
//
// fatal_cb set (live): disconnect → halt, no reconnect.
// fatal_cb unset (tests): reconnect with backoff.

class BybitUserDataTransport : public IFillTransport
{
public:
    BybitUserDataTransport(std::string api_key,
                           std::string api_secret,
                           std::string host = "stream.bybit.com",
                           std::string port = "443",
                           std::string path = "/v5/private")
        : api_key_(std::move(api_key))
        , api_secret_(std::move(api_secret))
        , host_(std::move(host))
        , port_(std::move(port))
        , path_(std::move(path))
        , ctx_(ssl::context::tlsv12_client)
        , signer_(api_secret_)
    {
        ctx_.set_default_verify_paths();
        ctx_.set_verify_mode(ssl::verify_peer);
    }

    BybitUserDataTransport(std::string api_key,
                           std::string api_secret,
                           const endpoints& ep)
        : BybitUserDataTransport(std::move(api_key),
                                 std::move(api_secret),
                                 ep.ws_private_host,
                                 ep.ws_port,
                                 ep.ws_private_path)
    {
    }

    ~BybitUserDataTransport() override { close(); }

    BybitUserDataTransport(const BybitUserDataTransport&) = delete;
    BybitUserDataTransport& operator=(const BybitUserDataTransport&) = delete;
    BybitUserDataTransport(BybitUserDataTransport&&) = delete;
    BybitUserDataTransport& operator=(BybitUserDataTransport&&) = delete;

    // Optional clock offset applied to auth expires (ms). Wire after
    // REST time-sync so auth stays within the validity window.
    void set_time_offset_ms(long long offset_ms)
    {
        time_offset_ms_.store(offset_ms, std::memory_order_relaxed);
    }

    bool open() override
    {
        {
            std::lock_guard<std::mutex> lk(state_mu_);
            if (state_ == lifecycle::open)
                return true;
        }

        if (api_key_.empty() || api_secret_.empty())
        {
            set_state(lifecycle::error, "missing api credentials");
            return false;
        }

        // Ensure previous reader is fully reaped.
        if (reader_.joinable())
            reader_.join();

        stop_flag_.store(false);
        ever_open_.store(false, std::memory_order_release);
        set_state(lifecycle::connecting, "connecting private WS");
        reader_ = std::thread([this] { run(); });

        // Fail-closed ready gate: auth + subscribe must complete before
        // ExecutionBridge treats fills_tx as live.
        {
            std::unique_lock<std::mutex> lk(state_mu_);
            const bool signaled = open_cv_.wait_for(
                lk, kOpenReadyTimeout, [this] {
                    return state_ == lifecycle::open
                        || state_ == lifecycle::error
                        || stop_flag_.load(std::memory_order_acquire);
                });
            if (state_ == lifecycle::open)
                return true;

            std::cerr << "BybitUserDataTransport: open ready-gate failed"
                      << (signaled ? " (error state)" : " (timeout)")
                      << "\n";
        }

        close();
        return false;
    }

    void close() override
    {
        stop_flag_.store(true);
        cv_.notify_all();

        {
            std::lock_guard<std::mutex> lk(ws_mu_);
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

        if (reader_.joinable())
            reader_.join();

        {
            std::lock_guard<std::mutex> lk(ws_mu_);
            if (ws_)
            {
                try
                {
                    beast::error_code ec;
                    ws_->close(websocket::close_code::normal, ec);
                }
                catch (...)
                {
                }
            }
            ws_.reset();
            ioc_.restart();
        }

        message_cb_ = {};
        status_cb_ = {};

        set_state(lifecycle::closed, "closed");
    }

    lifecycle state() const override
    {
        std::lock_guard<std::mutex> lk(state_mu_);
        return state_;
    }

    void set_on_message(message_cb cb) override { message_cb_ = std::move(cb); }
    void set_on_status(status_cb cb) override { status_cb_ = std::move(cb); }

    void set_fatal_disconnect_callback(
        std::function<void(std::string_view reason)> cb) override
    {
        fatal_cb_ = std::move(cb);
    }

private:
    enum class run_result
    {
        stopped,
        network_error,
        handshake_error,
        auth_error,
    };

    void set_state(lifecycle s, std::string note)
    {
        {
            std::lock_guard<std::mutex> lk(state_mu_);
            state_ = s;
            if (s == lifecycle::open || s == lifecycle::error)
                open_cv_.notify_all();
        }
        if (status_cb_)
            status_cb_(s, note);
    }

    int64_t auth_expires_ms() const
    {
        // Docs sample: now + 10s. Use +20s headroom for clock skew.
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
        return now + time_offset_ms_.load(std::memory_order_relaxed) + 20'000;
    }

    void send_text(std::string_view text)
    {
        if (!ws_)
            return;
        ws_->write(net::buffer(text.data(), text.size()));
    }

    void maybe_send_ping(bool force = false)
    {
        if (!ws_ || stop_flag_.load())
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
            std::cerr << "BybitUserDataTransport: ping failed: " << e.what()
                      << "\n";
        }
    }

    bool wait_ready_for_read()
    {
        if (!ws_ || stop_flag_.load())
            return false;

        if (ssl_has_pending_app_data(ws_->next_layer().native_handle()))
            return true;

        const int fd = beast::get_lowest_layer(*ws_).native_handle();
        const auto pr = poll_fd_readable(fd, kPollWake);
        if (pr == poll_wait_result::timeout)
            return false;
        return true;
    }

    // Read one application frame (handles ping/pong). Returns false
    // on stop/error; throws beast::system_error on socket failure.
    // `out` views frame_buffer_ — valid only until the next read.
    bool read_app_frame(std::string_view& out)
    {
        while (!stop_flag_.load())
        {
            maybe_send_ping();

            if (!wait_ready_for_read())
            {
                if (stop_flag_.load())
                    return false;
                maybe_send_ping(/*force=*/true);
                continue;
            }

            if (frame_buffer_.size() > 0)
                frame_buffer_.consume(frame_buffer_.size());

            ws_->read(frame_buffer_);

            if (stop_flag_.load())
                return false;

            auto const_buf = frame_buffer_.data();
            out = std::string_view(static_cast<const char*>(const_buf.data()),
                                   const_buf.size());

            if (is_pong_frame(out))
                continue;
            // Ignore control acks (subscribe/auth) on the hot read path.
            if (is_control_ack(out) && !is_auth_success(out)
                && !is_auth_failure(out))
                continue;
            return true;
        }
        return false;
    }

    bool await_auth()
    {
        const auto deadline =
            std::chrono::steady_clock::now() + kAuthTimeout;

        while (!stop_flag_.load())
        {
            if (std::chrono::steady_clock::now() >= deadline)
            {
                std::cerr << "BybitUserDataTransport: auth timeout\n";
                return false;
            }

            if (ws_ && !ssl_has_pending_app_data(
                            ws_->next_layer().native_handle()))
            {
                const int fd = beast::get_lowest_layer(*ws_).native_handle();
                const auto remaining =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        deadline - std::chrono::steady_clock::now());
                auto wait = remaining < kAuthPoll ? remaining : kAuthPoll;
                if (wait.count() <= 0)
                    return false;
                const auto pr = poll_fd_readable(fd, wait);
                if (pr == poll_wait_result::timeout)
                    continue;
                if (pr == poll_wait_result::error)
                    return false;
            }

            if (frame_buffer_.size() > 0)
                frame_buffer_.consume(frame_buffer_.size());

            ws_->read(frame_buffer_);
            if (stop_flag_.load())
                return false;

            auto const_buf = frame_buffer_.data();
            std::string_view view(
                static_cast<const char*>(const_buf.data()), const_buf.size());

            if (is_pong_frame(view))
                continue;

            if (is_auth_success(view))
                return true;
            if (is_auth_failure(view))
            {
                std::cerr << "BybitUserDataTransport: auth rejected: "
                          << redact_for_log(view, 200) << "\n";
                return false;
            }
            // Unrelated frame during auth — keep waiting.
        }
        return false;
    }

    run_result run_once()
    {
        bool reached_open = false;
        try
        {
            {
                std::lock_guard<std::mutex> lk(ws_mu_);
                ioc_.restart();
                ws_.reset();
                ws_ = std::make_unique<
                    websocket::stream<beast::ssl_stream<tcp::socket>>>(ioc_,
                                                                      ctx_);
            }

            tcp::resolver resolver(ioc_);
            auto results = resolver.resolve(host_, port_);

            auto& lowest = beast::get_lowest_layer(*ws_);
            net::connect(lowest, results);

            const int fd = lowest.native_handle();
            {
                const int yes = 1;
                const int idle = 1, intvl = 1, cnt = 2;
                ::setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &yes, sizeof(yes));
                ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
                ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl,
                             sizeof(intvl));
                ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
            }

            if (!SSL_set_tlsext_host_name(ws_->next_layer().native_handle(),
                                          host_.c_str()))
            {
                return run_result::handshake_error;
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

            // Auth.
            const std::int64_t expires = auth_expires_ms();
            const std::string expires_str = std::to_string(expires);
            const std::string sign = signer_.sign(
                build_ws_auth_prehash(expires_str));
            if (sign.empty())
            {
                std::cerr << "BybitUserDataTransport: sign_ws_auth failed\n";
                return run_result::auth_error;
            }
            send_text(build_auth_json(api_key_, expires, sign));

            if (!await_auth())
            {
                if (stop_flag_.load())
                    return run_result::stopped;
                return run_result::auth_error;
            }

            // Subscribe order / execution / position / wallet.
            send_text(build_private_subscribe_json());
            last_ping_ = std::chrono::steady_clock::now();

            reached_open = true;
            ever_open_.store(true, std::memory_order_release);
            set_state(lifecycle::open,
                      "private WS open (order/execution/position/wallet)");

            std::string_view view;
            while (!stop_flag_.load())
            {
                if (!read_app_frame(view))
                    break;
                if (message_cb_)
                    message_cb_(view);
            }
            return run_result::stopped;
        }
        catch (const beast::system_error& se)
        {
            if (stop_flag_.load())
                return run_result::stopped;
            (void)se;
            return reached_open ? run_result::network_error
                                : run_result::handshake_error;
        }
        catch (const std::exception& e)
        {
            if (stop_flag_.load())
                return run_result::stopped;
            std::cerr << "BybitUserDataTransport: " << e.what() << "\n";
            return reached_open ? run_result::network_error
                                : run_result::handshake_error;
        }
    }

    void run()
    {
        constexpr unsigned k_max_attempts = 10;
        auto delay = std::chrono::seconds(1);
        const auto max_delay = std::chrono::seconds(30);
        unsigned attempt = 0;

        while (!stop_flag_.load())
        {
            if (attempt > 0)
            {
                set_state(lifecycle::connecting,
                          "reconnecting private WS (attempt "
                              + std::to_string(attempt + 1) + "/"
                              + std::to_string(k_max_attempts) + ")");
                std::unique_lock<std::mutex> lk(cv_mu_);
                if (cv_.wait_for(lk, delay,
                                 [this] { return stop_flag_.load(); }))
                    break;
                delay = std::min(delay * 2, max_delay);
            }

            auto r = run_once();
            if (r == run_result::stopped)
            {
                if (stop_flag_.load(std::memory_order_acquire))
                    return;
                if (!ever_open_.load(std::memory_order_acquire))
                {
                    set_state(lifecycle::error,
                              "private WS stopped before ready");
                    return;
                }
                r = run_result::network_error;
            }

            const char* what = "network error";
            if (r == run_result::handshake_error)
                what = "handshake error";
            else if (r == run_result::auth_error)
                what = "auth error";

            if (!ever_open_.load(std::memory_order_acquire))
            {
                char buf[192];
                std::snprintf(buf, sizeof(buf),
                              "bybit private WS initial connect failed: %s",
                              what);
                std::cerr << "BybitUserDataTransport: " << buf << "\n";
                stop_flag_.store(true, std::memory_order_release);
                set_state(lifecycle::error, buf);
                return;
            }

            // Live path: fatal disconnect → halt, no reconnect.
            if (fatal_cb_)
            {
                char buf[192];
                std::snprintf(buf, sizeof(buf),
                              "bybit private WS lost: %s", what);
                fatal_cb_(buf);
                stop_flag_.store(true);
                set_state(lifecycle::error, buf);
                return;
            }

            if (++attempt >= k_max_attempts)
            {
                set_state(lifecycle::error,
                          "private WS: giving up after "
                              + std::to_string(k_max_attempts)
                              + " reconnect attempts");
                return;
            }
        }
    }

    std::string api_key_;
    std::string api_secret_;
    std::string host_;
    std::string port_;
    std::string path_;

    ssl::context ctx_;
    HmacSha256HexSigner signer_;
    std::atomic<long long> time_offset_ms_{0};

    net::io_context ioc_;
    std::mutex ws_mu_;
    std::unique_ptr<websocket::stream<beast::ssl_stream<tcp::socket>>> ws_;
    beast::flat_buffer frame_buffer_;

    message_cb message_cb_;
    status_cb status_cb_;
    std::function<void(std::string_view)> fatal_cb_;

    std::thread reader_;
    std::atomic<bool> stop_flag_{false};
    std::atomic<bool> ever_open_{false};

    std::mutex cv_mu_;
    std::condition_variable cv_;

    mutable std::mutex state_mu_;
    std::condition_variable open_cv_;
    lifecycle state_ = lifecycle::closed;

    std::chrono::steady_clock::time_point last_ping_{};

    static constexpr auto kPingInterval = std::chrono::seconds(20);
    static constexpr auto kPollWake = std::chrono::seconds(15);
    static constexpr auto kAuthTimeout = std::chrono::seconds(10);
    static constexpr auto kAuthPoll = std::chrono::milliseconds(500);
    static constexpr auto kOpenReadyTimeout = std::chrono::seconds(15);
};

} // namespace bybit

using BybitUserDataTransport = bybit::BybitUserDataTransport;

#endif // HAS_BYBIT
