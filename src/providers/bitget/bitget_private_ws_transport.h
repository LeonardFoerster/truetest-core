#pragma once
#ifdef HAS_BITGET

#include "execution/fill_transport.h"
#include "providers/bitget/bitget_auth.h"
#include "providers/bitget/bitget_endpoints.h"
#include "providers/bitget/bitget_parser.h"
#include "providers/bitget/bitget_transport.h"
#include "providers/bounded_ws_open.h"
#include "providers/recovery_payload.h"
#include "providers/thread_safe_callback.h"
#include "utils/retry.h"

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>

#include <array>
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

namespace bitget {

// ---------------------------------------------------------------------------
// Pure helpers (unit-testable without network)
// ---------------------------------------------------------------------------

// Minimal JSON string escape for login fields (RFC 8259 subset).
// Passphrases may contain `"`, `\`, or control chars — raw append breaks the
// login frame and fails auth silently.
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

// Login frame: op=login with apiKey / passphrase / timestamp / sign.
// Sign is Base64(HMAC-SHA256(secret, ts + "GET" + "/user/verify")).
// String fields are JSON-escaped; prehash still uses the raw secret/ts.
inline std::string build_login_json(std::string_view api_key,
                                    std::string_view passphrase,
                                    std::string_view timestamp,
                                    std::string_view sign)
{
    std::string j;
    j.reserve(96 + api_key.size() + passphrase.size() + timestamp.size()
              + sign.size() + 16);
    j += "{\"op\":\"login\",\"args\":[{\"apiKey\":\"";
    append_json_escaped(j, api_key);
    j += "\",\"passphrase\":\"";
    append_json_escaped(j, passphrase);
    j += "\",\"timestamp\":\"";
    append_json_escaped(j, timestamp);
    j += "\",\"sign\":\"";
    append_json_escaped(j, sign);
    j += "\"}]}";
    return j;
}

// Private UTA subscribe: order + fill + position + account (Phase 4 funding).
// instType is "UTA" (not public-stream "usdt-futures").
inline std::string build_private_subscribe_json()
{
    return R"({"op":"subscribe","args":[{"instType":"UTA","topic":"order"},{"instType":"UTA","topic":"fill"},{"instType":"UTA","topic":"position"},{"instType":"UTA","topic":"account"}]})";
}

// Login ack: event=="login" (or op=="login") and code 0 / "0" / "00000".
inline bool is_login_success(std::string_view msg)
{
    if (!provider_recovery::is_authoritative_object(msg)) return false;
    std::string_view event;
    std::string_view op;
    const bool has_event = provider_recovery::top_level_plain_string(
        msg, "event", event);
    const bool has_op = provider_recovery::top_level_plain_string(msg, "op", op);
    if (has_event == has_op || (has_event ? event : op) != "login")
        return false;

    std::string_view code;
    if (!provider_recovery::top_level_scalar_text(msg, "code", code))
        return false;
    return code == "0" || code == "00000";
}

// Login rejected (event/op login with non-zero code).
inline bool is_login_failure(std::string_view msg)
{
    if (!provider_recovery::is_authoritative_object(msg)) return false;
    std::string_view event;
    std::string_view op;
    const bool has_event = provider_recovery::top_level_plain_string(
        msg, "event", event);
    const bool has_op = provider_recovery::top_level_plain_string(msg, "op", op);
    if (has_event == has_op || (has_event ? event : op) != "login")
        return false;

    std::string_view code;
    if (!provider_recovery::top_level_scalar_text(msg, "code", code))
        return false;
    return code != "0" && code != "00000";
}

enum class subscription_ack { unrelated, accepted, rejected };

inline subscription_ack classify_private_subscription_ack(
    std::string_view msg, std::string_view& topic_out)
{
    topic_out = {};
    if (!provider_recovery::is_authoritative_object(msg))
        return subscription_ack::unrelated;

    std::string_view event;
    if (!provider_recovery::top_level_plain_string(msg, "event", event))
        return subscription_ack::unrelated;
    if (event == "error") return subscription_ack::rejected;
    if (event != "subscribe") return subscription_ack::unrelated;

    std::string_view code_raw;
    const auto code_state = provider_recovery::payload_parser(msg)
        .inspect_top_level_member("code", code_raw);
    if (code_state == provider_recovery::payload_parser::member_result::invalid_or_duplicate)
        return subscription_ack::rejected;
    if (code_state == provider_recovery::payload_parser::member_result::unique)
    {
        std::string_view code;
        if (!provider_recovery::top_level_scalar_text(msg, "code", code)
            || (!code.empty() && code != "0" && code != "00000"))
            return subscription_ack::rejected;
    }

    std::string_view arg;
    if (!provider_recovery::top_level_member(msg, "arg", arg)
        || !provider_recovery::is_authoritative_object(arg)
        || !provider_recovery::top_level_exact_string(arg, "instType", "UTA")
        || !provider_recovery::top_level_plain_string(arg, "topic", topic_out))
        return subscription_ack::rejected;

    if (topic_out != "order" && topic_out != "fill"
        && topic_out != "position" && topic_out != "account")
        return subscription_ack::rejected;
    return subscription_ack::accepted;
}

enum class subscription_progress { waiting, ready, rejected };

class private_subscription_tracker
{
public:
    subscription_progress consume(std::string_view frame)
    {
        std::string_view topic;
        const auto ack = classify_private_subscription_ack(frame, topic);
        if (ack == subscription_ack::rejected)
            return subscription_progress::rejected;
        if (ack == subscription_ack::accepted)
        {
            if (topic == "order") seen_[0] = true;
            else if (topic == "fill") seen_[1] = true;
            else if (topic == "position") seen_[2] = true;
            else if (topic == "account") seen_[3] = true;
        }
        return seen_[0] && seen_[1] && seen_[2] && seen_[3]
            ? subscription_progress::ready : subscription_progress::waiting;
    }

private:
    std::array<bool, 4> seen_{};
};

// ---------------------------------------------------------------------------
// BitgetPrivateWsTransport — IFillTransport for UTA private WS
// ---------------------------------------------------------------------------
//
// Connect wss://{ws_private_host}/v3/ws/private → login → subscribe
// order/fill/position. Raw text ping/pong with poll-before-read (same as
// public BitgetTransport). WS is source of truth for fills.
//
// fatal_cb set (live): disconnect → halt, no reconnect.
// fatal_cb unset (tests): reconnect with backoff.

class BitgetPrivateWsTransport : public IFillTransport
{
public:
    BitgetPrivateWsTransport(std::string api_key,
                             std::string api_secret,
                             std::string passphrase,
                             std::string host = "ws.bitget.com",
                             std::string port = "443",
                             std::string path = "/v3/ws/private")
        : api_key_(std::move(api_key))
        , api_secret_(std::move(api_secret))
        , passphrase_(std::move(passphrase))
        , host_(std::move(host))
        , port_(std::move(port))
        , path_(std::move(path))
        , ctx_(ssl::context::tlsv12_client)
        , signer_(api_secret_)
    {
        ctx_.set_default_verify_paths();
        ctx_.set_verify_mode(ssl::verify_peer);
    }

    BitgetPrivateWsTransport(std::string api_key,
                             std::string api_secret,
                             std::string passphrase,
                             const endpoints& ep)
        : BitgetPrivateWsTransport(std::move(api_key),
                                   std::move(api_secret),
                                   std::move(passphrase),
                                   ep.ws_private_host,
                                   ep.ws_port,
                                   ep.ws_private_path)
    {
    }

    ~BitgetPrivateWsTransport() override { close(); }

    BitgetPrivateWsTransport(const BitgetPrivateWsTransport&) = delete;
    BitgetPrivateWsTransport& operator=(const BitgetPrivateWsTransport&) = delete;
    BitgetPrivateWsTransport(BitgetPrivateWsTransport&&) = delete;
    BitgetPrivateWsTransport& operator=(BitgetPrivateWsTransport&&) = delete;

    // Optional clock offset applied to login timestamp (ms). Wire after
    // REST time-sync so login stays within the ~30s validity window.
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

        if (api_key_.empty() || api_secret_.empty() || passphrase_.empty())
        {
            set_state(lifecycle::error, "missing api credentials");
            return false;
        }

        // Ensure previous reader is fully reaped (error/fatal paths leave
        // joinable threads). Assigning to a joinable std::thread terminates.
        if (reader_.joinable())
            reader_.join();

        stop_flag_.store(false);
        ever_open_.store(false, std::memory_order_release);
        set_state(lifecycle::connecting, "connecting private WS");
        reader_ = std::thread([this] { run(); });

        // Fail-closed ready gate: login + subscribe must complete before
        // ExecutionBridge treats fills_tx as live. Returning true on thread
        // spawn alone allows place-orders with no user-data stream.
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

            std::cerr << "BitgetPrivateWsTransport: open ready-gate failed"
                      << (signaled ? " (error state)" : " (timeout)")
                      << "\n";
        }

        // Tear down partial connect so a subsequent open() starts clean.
        close();
        return false;
    }

    void close() override
    {
        // 1. Signal stop so the reader exits poll/wait loops.
        stop_flag_.store(true);
        cv_.notify_all();

        // 2. Foreign-thread interrupt: Beast websocket::stream is NOT
        // thread-safe. Do NOT call ws_->close() while the reader may still
        // be in read/write. Cancel/close the lowest-layer socket only — that
        // unblocks a blocking read without concurrent protocol ops on *ws_.
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

        // 3. Reap reader — sole stream user after open until join returns.
        if (reader_.joinable())
            reader_.join();

        // 4. Now safe: optional graceful WS close, then drop the stream.
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

        // 5. Drop bridge callbacks so a reused fill_tx cannot fire into a
        // destroyed owner (UAF hardening). Keep fatal_cb_ (wired by engine).
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
        fatal_cb_.store(std::move(cb));
    }

private:
    enum class run_result
    {
        stopped,
        network_error,
        handshake_error,
        login_error,
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
        {
            try { status_cb_(s, note); }
            catch (...) {}
        }
    }

    int64_t login_timestamp_ms() const
    {
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
        return now + time_offset_ms_.load(std::memory_order_relaxed);
    }

    void send_text(std::string_view text)
    {
        if (!ws_)
            return;
        ws_->write(net::buffer(text.data(), text.size()));
    }

    bool send_text_bounded(std::string text,
                           std::chrono::milliseconds timeout)
    {
        if (!ws_) return false;
        auto payload = std::make_shared<std::string>(std::move(text));
        return provider_ws::run_bounded(
            ioc_, timeout,
            [this, payload](auto done) {
                ws_->async_write(
                    net::buffer(*payload),
                    [payload, done](beast::error_code ec,
                                    std::size_t) mutable { done(ec); });
            },
            [this] {
                beast::error_code ignored;
                auto& lowest = beast::get_lowest_layer(*ws_);
                lowest.cancel(ignored);
                lowest.close(ignored);
            });
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
            send_text("ping");
            last_ping_ = now;
        }
        catch (const std::exception& e)
        {
            std::cerr << "BitgetPrivateWsTransport: ping failed: " << e.what()
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

    // Read one application frame (handles text ping/pong). Returns false
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

            const bool read = provider_ws::run_bounded(
                ioc_, kPollWake,
                [this](auto done) {
                    ws_->async_read(
                        frame_buffer_,
                        [done](beast::error_code ec,
                               std::size_t) mutable { done(ec); });
                },
                [this] {
                    beast::error_code ignored;
                    auto& lowest = beast::get_lowest_layer(*ws_);
                    lowest.cancel(ignored);
                    lowest.close(ignored);
                });
            if (!read) return false;

            if (stop_flag_.load())
                return false;

            auto const_buf = frame_buffer_.data();
            out = std::string_view(static_cast<const char*>(const_buf.data()),
                                   const_buf.size());

            if (is_pong_text(out))
                continue;
            if (is_ping_text(out))
            {
                send_text("pong");
                continue;
            }
            return true;
        }
        return false;
    }

    // Wait for login ack (or fail). Uses poll wake so we can stop cleanly.
    bool await_login()
    {
        const auto deadline =
            std::chrono::steady_clock::now() + kLoginTimeout;

        while (!stop_flag_.load())
        {
            if (std::chrono::steady_clock::now() >= deadline)
            {
                std::cerr << "BitgetPrivateWsTransport: login timeout\n";
                return false;
            }

            // Shorter poll while waiting for login.
            if (ws_ && !ssl_has_pending_app_data(
                            ws_->next_layer().native_handle()))
            {
                const int fd = beast::get_lowest_layer(*ws_).native_handle();
                const auto remaining =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        deadline - std::chrono::steady_clock::now());
                auto wait = remaining < kLoginPoll ? remaining : kLoginPoll;
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

            const auto remaining =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - std::chrono::steady_clock::now());
            if (remaining.count() <= 0)
                return false;
            const bool read = provider_ws::run_bounded(
                ioc_, remaining,
                [this](auto done) {
                    ws_->async_read(
                        frame_buffer_,
                        [done](beast::error_code ec,
                               std::size_t) mutable { done(ec); });
                },
                [this] {
                    beast::error_code ignored;
                    auto& lowest = beast::get_lowest_layer(*ws_);
                    lowest.cancel(ignored);
                    lowest.close(ignored);
                });
            if (!read) return false;
            if (stop_flag_.load())
                return false;

            auto const_buf = frame_buffer_.data();
            std::string_view view(
                static_cast<const char*>(const_buf.data()), const_buf.size());

            if (is_pong_text(view))
                continue;
            if (is_ping_text(view))
            {
                send_text("pong");
                continue;
            }

            if (is_login_success(view))
                return true;
            if (is_login_failure(view))
            {
                std::cerr << "BitgetPrivateWsTransport: login rejected: "
                          << view.substr(0, 200) << "\n";
                return false;
            }
            // Unrelated frame during login (rare) — keep waiting.
        }
        return false;
    }

    bool await_private_subscriptions()
    {
        const auto deadline =
            std::chrono::steady_clock::now() + kLoginTimeout;
        private_subscription_tracker tracker;

        while (!stop_flag_.load(std::memory_order_acquire))
        {
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline)
            {
                std::cerr << "BitgetPrivateWsTransport: subscribe timeout\n";
                return false;
            }

            if (ws_ && !ssl_has_pending_app_data(
                            ws_->next_layer().native_handle()))
            {
                const int fd = beast::get_lowest_layer(*ws_).native_handle();
                const auto remaining =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        deadline - now);
                const auto wait = remaining < kLoginPoll
                    ? remaining : kLoginPoll;
                if (wait.count() <= 0) return false;
                const auto pr = poll_fd_readable(fd, wait);
                if (pr == poll_wait_result::timeout) continue;
                if (pr == poll_wait_result::error) return false;
            }

            if (frame_buffer_.size() > 0)
                frame_buffer_.consume(frame_buffer_.size());
            const auto remaining =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - std::chrono::steady_clock::now());
            if (remaining.count() <= 0) return false;
            const bool read = provider_ws::run_bounded(
                ioc_, remaining,
                [this](auto done) {
                    ws_->async_read(
                        frame_buffer_,
                        [done](beast::error_code ec,
                               std::size_t) mutable { done(ec); });
                },
                [this] {
                    beast::error_code ignored;
                    auto& lowest = beast::get_lowest_layer(*ws_);
                    lowest.cancel(ignored);
                    lowest.close(ignored);
                });
            if (!read || stop_flag_.load(std::memory_order_acquire))
                return false;

            const auto const_buf = frame_buffer_.data();
            const std::string_view view(
                static_cast<const char*>(const_buf.data()), const_buf.size());
            if (is_pong_text(view)) continue;
            if (is_ping_text(view))
            {
                send_text("pong");
                continue;
            }

            const auto progress = tracker.consume(view);
            if (progress == subscription_progress::rejected)
            {
                std::cerr << "BitgetPrivateWsTransport: subscribe rejected: "
                          << view.substr(0, 200) << "\n";
                return false;
            }
            if (progress == subscription_progress::ready)
                return true;
            // Ignore unrelated frames until all four authoritative acks arrive.
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

            constexpr auto open_stage_timeout = std::chrono::seconds(3);
            const bool opened = provider_ws::open_tls_websocket(
                ioc_, *ws_, host_, port_, path_, open_stage_timeout,
                [](auto& socket) {
                    auto& lowest = beast::get_lowest_layer(socket);
                    const int yes = 1;
                    const int idle = 1, intvl = 1, cnt = 2;
                    const int fd = lowest.native_handle();
                    ::setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &yes,
                                 sizeof(yes));
                    ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle,
                                 sizeof(idle));
                    ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl,
                                 sizeof(intvl));
                    ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt,
                                 sizeof(cnt));
                },
                [](auto& socket) {
                    websocket::stream_base::timeout opt;
                    opt.handshake_timeout = websocket::stream_base::none();
                    opt.idle_timeout = websocket::stream_base::none();
                    opt.keep_alive_pings = false;
                    socket.set_option(opt);
                    socket.set_option(websocket::stream_base::decorator(
                        [](websocket::request_type& req) {
                            req.set(boost::beast::http::field::user_agent,
                                    "TrueTest/1.0");
                        }));
                    socket.control_callback(
                        [](websocket::frame_type kind, beast::string_view) {
                            (void)kind;
                        });
                });
            if (!opened)
                return stop_flag_.load(std::memory_order_acquire)
                    ? run_result::stopped : run_result::handshake_error;

            // Login.
            const std::string ts = std::to_string(login_timestamp_ms());
            const std::string sign = signer_.sign(
                build_prehash(ts, "GET", "/user/verify", "", ""));
            if (sign.empty())
            {
                std::cerr << "BitgetPrivateWsTransport: sign_ws_login failed\n";
                return run_result::login_error;
            }
            const std::string login =
                build_login_json(api_key_, passphrase_, ts, sign);
            if (!send_text_bounded(login, open_stage_timeout))
                return stop_flag_.load(std::memory_order_acquire)
                    ? run_result::stopped : run_result::login_error;

            if (!await_login())
            {
                if (stop_flag_.load())
                    return run_result::stopped;
                return run_result::login_error;
            }

            // Subscribe order / fill / position.
            if (!send_text_bounded(build_private_subscribe_json(),
                                   open_stage_timeout))
                return stop_flag_.load(std::memory_order_acquire)
                    ? run_result::stopped : run_result::login_error;
            if (!await_private_subscriptions())
                return stop_flag_.load(std::memory_order_acquire)
                    ? run_result::stopped : run_result::login_error;
            last_ping_ = std::chrono::steady_clock::now();

            reached_open = true;
            ever_open_.store(true, std::memory_order_release);
            set_state(lifecycle::open, "private WS open (order/fill/position)");

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
            std::cerr << "BitgetPrivateWsTransport: " << e.what() << "\n";
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
                // Dropped after open without stop_flag → treat as network loss
                // so fatal_cb / reconnect policy can run (unless close() set stop).
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
            else if (r == run_result::login_error)
                what = "login error";

            // Initial connect failed before ever open: fail open() wait
            // immediately — do not burn reconnect budget during ready-gate.
            if (!ever_open_.load(std::memory_order_acquire))
            {
                char buf[192];
                std::snprintf(buf, sizeof(buf),
                              "bitget private WS initial connect failed: %s",
                              what);
                std::cerr << "BitgetPrivateWsTransport: " << buf << "\n";
                stop_flag_.store(true, std::memory_order_release);
                set_state(lifecycle::error, buf);
                return;
            }

            // Any post-ready loss is terminal. If the engine has not yet
            // registered its callback, latch and synchronously deliver it
            // from set_fatal_disconnect_callback.
            char buf[192];
            std::snprintf(buf, sizeof(buf),
                          "bitget private WS lost: %s", what);
            stop_flag_.store(true);
            set_state(lifecycle::error, buf);
            fatal_cb_.publish(buf);
            return;
        }
    }

    std::string api_key_;
    std::string api_secret_;
    std::string passphrase_;
    std::string host_;
    std::string port_;
    std::string path_;

    ssl::context ctx_;
    HmacSha256Base64Signer signer_;
    std::atomic<long long> time_offset_ms_{0};

    net::io_context ioc_;
    std::mutex ws_mu_;
    std::unique_ptr<websocket::stream<beast::ssl_stream<tcp::socket>>> ws_;
    beast::flat_buffer frame_buffer_;

    message_cb message_cb_;
    status_cb status_cb_;
    LatchedFailureCallback fatal_cb_;

    std::thread reader_;
    std::atomic<bool> stop_flag_{false};
    // Set once login+subscribe succeed; open() ready-gate and reconnect policy.
    std::atomic<bool> ever_open_{false};

    std::mutex cv_mu_;
    std::condition_variable cv_;

    mutable std::mutex state_mu_;
    std::condition_variable open_cv_;
    lifecycle state_ = lifecycle::closed;

    std::chrono::steady_clock::time_point last_ping_{};

    static constexpr auto kPingInterval = std::chrono::seconds(30);
    static constexpr auto kPollWake = std::chrono::seconds(25);
    static constexpr auto kLoginTimeout = std::chrono::seconds(10);
    static constexpr auto kLoginPoll = std::chrono::milliseconds(500);
    // Bound open() wait: login timeout (10s) + TLS + slack.
    static constexpr auto kOpenReadyTimeout = std::chrono::seconds(15);
};

} // namespace bitget

using BitgetPrivateWsTransport = bitget::BitgetPrivateWsTransport;

#endif // HAS_BITGET
