#pragma once
#ifdef HAS_BITGET

#include "execution/fill_transport.h"
#include "providers/bitget/bitget_auth.h"
#include "providers/bitget/bitget_endpoints.h"
#include "providers/bitget/bitget_futures_user_data_parser.h"
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
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <iostream>
#include <mutex>
#include <string>
#include <stdexcept>
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

    // Setup controls are consumed before a message callback exists.  Reuse
    // the exact post-ready control schema so a data-bearing pseudo-login
    // cannot be silently discarded during the readiness handshake.
    return BitgetFuturesUserDataParser{}.is_harmless_private_control(msg);
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

    // This frame is consumed before callbacks are active.  Its schema must
    // be identical to the post-ready harmless-control schema: in
    // particular, data/action/op or any unknown field makes it a terminal
    // setup failure instead of an ACK that could erase private truth.
    if (!BitgetFuturesUserDataParser{}.is_harmless_private_control(msg))
        return subscription_ack::rejected;

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

enum class subscription_progress { waiting, ready, rejected, unexpected };

class private_subscription_tracker
{
public:
    subscription_progress consume(std::string_view frame)
    {
        std::string_view topic;
        const auto ack = classify_private_subscription_ack(frame, topic);
        if (ack == subscription_ack::rejected)
            return subscription_progress::rejected;
        if (ack == subscription_ack::unrelated)
            return subscription_progress::unexpected;
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
// Delivery and fatal callbacks are mandatory before open(): facts must reach
// the execution bridge, and disconnect → halt with no reconnect.  A
// callback-less private stream would otherwise become fill-blind.

class BitgetPrivateWsTransport : public IFillTransport
{
public:
    enum class run_result
    {
        stopped,
        network_error,
        handshake_error,
        login_error,
    };

    // Deterministic lifecycle seam.  Production uses the endpoint-backed
    // constructors; tests can model a reader that waits for close() without
    // opening a real TLS socket.
    using run_once_fn = std::function<run_result(std::atomic<bool>& stop)>;

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

    BitgetPrivateWsTransport(std::string api_key,
                             std::string api_secret,
                             std::string passphrase,
                             run_once_fn run_once,
                             bool override_reports_ready = false)
        : BitgetPrivateWsTransport(std::move(api_key),
                                   std::move(api_secret),
                                   std::move(passphrase),
                                   "localhost", "1", "/v3/ws/private")
    {
        run_once_override_ = std::move(run_once);
        override_reports_ready_ = override_reports_ready;
    }

    ~BitgetPrivateWsTransport() override
    {
        // A close-status callback must not reopen while member teardown is
        // about to destroy the socket/context it would otherwise reuse.
        destroying_.store(true, std::memory_order_release);
        close();
    }

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
        // A concurrent open must not queue behind a pending ready gate and
        // later become an implicit reopen after close().  It either observes
        // an already-ready session below or fails closed.
        if (open_in_progress_.load(std::memory_order_acquire))
            return false;
        if (destroying_.load(std::memory_order_acquire))
            return false;

        // Serialize session ownership (threads and io_context reset), while
        // allowing close() to request stop before it waits for this mutex.
        std::unique_lock<std::mutex> control(control_mu_);
        status_notifications deferred_status;
        bool owns_open_call = false;
        const auto finish_open = [&](bool result) {
            if (control.owns_lock()) control.unlock();
            publish_statuses(deferred_status);
            if (owns_open_call)
                open_in_progress_.store(false, std::memory_order_release);
            return result;
        };

        if (open_in_progress_.load(std::memory_order_acquire))
            return finish_open(false);
        if (destroying_.load(std::memory_order_acquire))
            return finish_open(false);

        // A close that has begun owns the terminal transition.  A queued
        // open must not clear stop_flag_ before close has joined the reader.
        if (close_requests_.load(std::memory_order_acquire) != 0)
            return finish_open(false);
        if (session_is_ready_locked()) return finish_open(true);

        // Remain exclusive through deferred status delivery. A status
        // callback must not recursively turn an error/close into a new
        // private stream while this invocation is still unwinding.
        open_in_progress_.store(true, std::memory_order_release);
        owns_open_call = true;

        if (api_key_.empty() || api_secret_.empty() || passphrase_.empty())
        {
            set_state(lifecycle::error, "missing api credentials",
                      &deferred_status);
            return finish_open(false);
        }

        // Private execution must not begin unless its authoritative frames
        // have a delivery sink and a stream-loss has an immediate terminal
        // route.  Check before reader/session setup or any network work.
        if (!message_callback_ready())
        {
            set_state(lifecycle::error, "private message callback required",
                      &deferred_status);
            return finish_open(false);
        }
        if (!fatal_callback_ready())
        {
            set_state(lifecycle::error, "fatal disconnect callback required",
                      &deferred_status);
            return finish_open(false);
        }

        // Error/fatal paths can leave a joinable reader.  It is unsafe to
        // overwrite it or reuse ioc_ until the join establishes that the old
        // reader cannot publish another ready/error transition.
        if ((reader_started_.load(std::memory_order_acquire)
             && !reader_joined_.load(std::memory_order_acquire))
            || reader_.joinable()
            || active_session_.load(std::memory_order_acquire) != 0)
            teardown_locked(&deferred_status);

        if (close_requests_.load(std::memory_order_acquire) != 0)
            return finish_open(false);

        ever_open_.store(false, std::memory_order_release);
        const auto session = next_session_++;
        begin_session_locked(session, "connecting private WS", &deferred_status);
        reader_joined_.store(false, std::memory_order_release);
        try
        {
            reader_ = std::thread([this, session] { run(session); });
            reader_started_.store(true, std::memory_order_release);
        }
        catch (...)
        {
            (void)set_state_for_session(session, lifecycle::error,
                                        "private reader start failed",
                                        &deferred_status);
            stop_session(session);
            teardown_locked(&deferred_status);
            return finish_open(false);
        }

        // Fail-closed ready gate: login + subscribe must complete before
        // ExecutionBridge treats fills_tx as live. Returning true on thread
        // spawn alone allows place-orders with no user-data stream.
        //
        // Release control_mu_ here: a callback running on the reader can
        // synchronously enter a halt/close path, and close() must be able to
        // join rather than blocking behind an open() that is waiting for it.
        control.unlock();
        publish_statuses(deferred_status);
        bool ready = false;
        {
            std::unique_lock<std::mutex> lk(state_mu_);
            const bool signaled = open_cv_.wait_for(
                lk, kOpenReadyTimeout, [this] {
                    return (state_ == lifecycle::open
                            && !ready_status_callback_pending_)
                        || state_ == lifecycle::error
                        || stop_flag_.load(std::memory_order_acquire);
                });
            ready = state_ == lifecycle::open
                && !ready_status_callback_pending_
                && session_running_locked(session);

            if (!ready)
            {
                std::cerr << "BitgetPrivateWsTransport: open ready-gate failed"
                          << (signaled ? " (error state)" : " (timeout)")
                          << "\n";
            }
        }

        control.lock();
        if (ready
            && close_requests_.load(std::memory_order_acquire) == 0
            && session_is_ready_locked())
        {
            return finish_open(true);
        }

        // Tear down partial connect so a subsequent open() starts clean.
        teardown_locked(&deferred_status);
        return finish_open(false);
    }

    void close() override
    {
        // A lifecycle status callback can originate on the reader while a
        // different owner already holds control_mu_ and joins that reader.
        // Re-entering close() there must only assert stop; taking control_mu_
        // would form reader -> control_mu_ -> join(reader).  The outer
        // open/close owner observes stop_flag_ and completes teardown.
        if (status_callback_context() == this)
        {
            request_stop();
            return;
        }

        // Linearize close against reader ready publication before waiting for
        // the session owner.  An open() in its ready gate therefore returns
        // false rather than reporting a stream that is concurrently closing.
        close_requests_.fetch_add(1, std::memory_order_acq_rel);
        request_stop();
        std::unique_lock<std::mutex> control(control_mu_);
        status_notifications deferred_status;
        const bool caller_is_reader = called_from_reader_locked();
        teardown_locked(&deferred_status);
        // Do not reacquire control_mu_ from a reader-originated status
        // callback.  An opener may already own it and be joining this reader;
        // waiting here would create a reader -> control -> join cycle.  The
        // request counter is still consumed, while a later non-reader owner
        // performs the actual join and final closed transition.
        if (caller_is_reader)
        {
            finish_close_request_locked();
            control.unlock();
            publish_statuses(deferred_status);
            return;
        }
        control.unlock();
        publish_statuses(deferred_status);
        control.lock();
        finish_close_request_locked();
    }

    lifecycle state() const override
    {
        std::lock_guard<std::mutex> lk(state_mu_);
        return state_;
    }

    void set_on_message(message_cb cb) override
    {
        std::lock_guard<std::mutex> control(control_mu_);
        std::lock_guard<std::mutex> lock(callback_mu_);
        // A running private stream may not lose its only delivery sink. The
        // bridge may detach it after close() has joined the reader, but a
        // clear while any generation remains live or unjoined is refused.
        if (!cb && !callbacks_detachable_locked())
            return;
        const bool armed = static_cast<bool>(cb);
        message_callback_armed_.store(false, std::memory_order_release);
        message_cb_.store(std::move(cb));
        message_callback_armed_.store(armed, std::memory_order_release);
    }
    void set_on_status(status_cb cb) override
    {
        std::lock_guard<std::mutex> control(control_mu_);
        if (!cb && !callbacks_detachable_locked())
            return;
        status_cb_.store(std::move(cb));
    }

    void set_fatal_disconnect_callback(
        std::function<void(std::string_view reason)> cb) override
    {
        // Installing/replacing a non-empty callback can synchronously replay
        // a previously latched failure. Keep control_mu_ out of that call so
        // a replayed callback may safely enter close().
        if (cb)
        {
            std::lock_guard<std::mutex> lock(callback_mu_);
            fatal_callback_armed_.store(false, std::memory_order_release);
            fatal_cb_.store(std::move(cb));
            fatal_callback_armed_.store(true, std::memory_order_release);
            return;
        }

        std::lock_guard<std::mutex> control(control_mu_);
        std::lock_guard<std::mutex> lock(callback_mu_);
        // Clearing a running private stream's only halt path is refused.
        // Keeping the last registered callback is safer than permitting an
        // asynchronous loss to continue fill-blind.
        if (!callbacks_detachable_locked())
            return;
        fatal_callback_armed_.store(false, std::memory_order_release);
        fatal_cb_.store({});
    }

    // Providers can forward this as private_execution_producer_joined().
    // False before the first reader launch and after every launch; true only
    // after the launch's reader thread has been joined by teardown.
    bool private_execution_producer_joined() const noexcept
    {
        return reader_started_.load(std::memory_order_acquire)
            && reader_joined_.load(std::memory_order_acquire)
            && active_session_.load(std::memory_order_acquire) == 0;
    }

private:
    struct status_notification
    {
        lifecycle state;
        std::string note;
    };
    struct status_notifications
    {
        // A lifecycle path has a bounded transition count. Avoid a vector
        // allocation in close()/destruction merely to defer status callbacks
        // until after control_mu_ is released.
        std::array<status_notification, 8> entries{};
        std::size_t size = 0;

        void push(lifecycle state, std::string note) noexcept
        {
            if (size == entries.size()) return;
            entries[size++] = {state, std::move(note)};
        }

        void clear() noexcept
        {
            for (std::size_t i = 0; i < size; ++i)
                entries[i].note.clear();
            size = 0;
        }
    };

    bool message_callback_ready()
    {
        std::lock_guard<std::mutex> lock(callback_mu_);
        return message_callback_armed_.load(std::memory_order_acquire)
            && static_cast<bool>(message_cb_.load());
    }

    bool fatal_callback_ready()
    {
        std::lock_guard<std::mutex> lock(callback_mu_);
        return fatal_callback_armed_.load(std::memory_order_acquire);
    }

    // `control_mu_` is held by callers. `active_session_` becomes zero before
    // join, so both the joined proof and std::thread ownership check are
    // necessary before a bridge callback may be detached.
    bool callbacks_detachable_locked() const noexcept
    {
        return active_session_.load(std::memory_order_acquire) == 0
            && (!reader_started_.load(std::memory_order_acquire)
                || reader_joined_.load(std::memory_order_acquire))
            && !reader_.joinable();
    }

    void publish_statuses(status_notifications& notifications) noexcept
    {
        for (std::size_t i = 0; i < notifications.size; ++i)
            publish_status(notifications.entries[i].state,
                           notifications.entries[i].note);
        notifications.clear();
    }

    void record_status(status_notifications* deferred, lifecycle s,
                       std::string note) noexcept
    {
        if (deferred)
        {
            deferred->push(s, std::move(note));
            return;
        }
        publish_status(s, note);
    }

    void publish_status(lifecycle s, std::string_view note) noexcept
    {
        auto& callback_context = status_callback_context();
        struct restore_context
        {
            BitgetPrivateWsTransport*& slot;
            BitgetPrivateWsTransport* previous;
            ~restore_context() { slot = previous; }
        } restore{callback_context, callback_context};
        callback_context = this;
        if (auto callback = status_cb_.load())
        {
            try { (*callback)(s, note); }
            catch (...) {}
        }
    }

    static BitgetPrivateWsTransport*& status_callback_context() noexcept
    {
        static thread_local BitgetPrivateWsTransport* current = nullptr;
        return current;
    }

    void set_state(lifecycle s, std::string note,
                   status_notifications* deferred = nullptr)
    {
        {
            std::lock_guard<std::mutex> lk(state_mu_);
            state_ = s;
            if (s == lifecycle::open || s == lifecycle::error)
                open_cv_.notify_all();
        }
        record_status(deferred, s, std::move(note));
    }

    bool set_state_for_session(std::uint64_t session, lifecycle s,
                               std::string note,
                               status_notifications* deferred = nullptr)
    {
        // A reader status callback can call close().  The readiness wait must
        // not succeed until that callback returns, otherwise open() can
        // admit a stream which the callback has already stopped.
        const bool gate_ready_on_callback = s == lifecycle::open
            && deferred == nullptr;
        {
            std::lock_guard<std::mutex> lk(state_mu_);
            if (active_session_.load(std::memory_order_acquire) != session
                || closing_)
                return false;
            if (s == lifecycle::open
                && stop_flag_.load(std::memory_order_acquire))
                return false;
            state_ = s;
            if (gate_ready_on_callback)
                ready_status_callback_pending_ = true;
            else if (s != lifecycle::open)
                ready_status_callback_pending_ = false;
            if (s == lifecycle::error || !gate_ready_on_callback)
                open_cv_.notify_all();
        }
        record_status(deferred, s, std::move(note));
        if (gate_ready_on_callback)
        {
            std::lock_guard<std::mutex> lk(state_mu_);
            ready_status_callback_pending_ = false;
            open_cv_.notify_all();
        }
        return true;
    }

    bool session_running(std::uint64_t session) const noexcept
    {
        return active_session_.load(std::memory_order_acquire) == session
            && !stop_flag_.load(std::memory_order_acquire);
    }

    bool session_running_locked(std::uint64_t session) const noexcept
    {
        return !closing_
            && active_session_.load(std::memory_order_acquire) == session
            && !stop_flag_.load(std::memory_order_acquire);
    }

    bool session_is_ready_locked() const noexcept
    {
        std::lock_guard<std::mutex> lk(state_mu_);
        return state_ == lifecycle::open && !ready_status_callback_pending_
            && !closing_
            && active_session_.load(std::memory_order_acquire) != 0
            && !stop_flag_.load(std::memory_order_acquire);
    }

    void begin_session_locked(std::uint64_t session, std::string note,
                              status_notifications* deferred = nullptr)
    {
        stop_flag_.store(false, std::memory_order_release);
        active_session_.store(session, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lk(state_mu_);
            closing_ = false;
            ready_status_callback_pending_ = false;
            state_ = lifecycle::connecting;
            open_cv_.notify_all();
        }
        record_status(deferred, lifecycle::connecting, std::move(note));
    }

    void request_stop() noexcept
    {
        stop_flag_.store(true, std::memory_order_release);
        stop_flag_.notify_all();
        {
            std::lock_guard<std::mutex> lk(state_mu_);
            closing_ = true;
            open_cv_.notify_all();
        }
        cv_.notify_all();
        interrupt_websocket();
    }

    void stop_session(std::uint64_t session) noexcept
    {
        if (active_session_.load(std::memory_order_acquire) == session)
            request_stop();
    }

    void interrupt_websocket() noexcept
    {
        try
        {
            std::lock_guard<std::mutex> lk(ws_mu_);
            if (ws_)
            {
                beast::error_code ec;
                auto& lowest = beast::get_lowest_layer(*ws_);
                lowest.cancel(ec);
                lowest.close(ec);
            }
            ioc_.stop();
        }
        catch (...)
        {
        }
    }

    bool called_from_reader_locked() const noexcept
    {
        return reader_.joinable()
            && reader_.get_id() == std::this_thread::get_id();
    }

    // `control_mu_` is held.  Joining is the proof that no reader still owns
    // the websocket, frame buffer, callbacks, or lifecycle generation.
    void teardown_locked(status_notifications* deferred = nullptr)
    {
        request_stop();
        if (called_from_reader_locked()) return;

        const bool had_active_session =
            active_session_.load(std::memory_order_acquire) != 0;
        const bool had_worker = reader_.joinable();
        active_session_.store(0, std::memory_order_release);
        if (reader_.joinable()) reader_.join();
        if (reader_started_.load(std::memory_order_acquire))
            reader_joined_.store(true, std::memory_order_release);

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

        bool notify_closed = had_active_session || had_worker;
        {
            std::lock_guard<std::mutex> lk(state_mu_);
            notify_closed = notify_closed || state_ != lifecycle::closed;
            state_ = lifecycle::closed;
            ready_status_callback_pending_ = false;
            closing_ = close_requests_.load(std::memory_order_acquire) != 0;
            open_cv_.notify_all();
        }
        if (notify_closed)
            record_status(deferred, lifecycle::closed, "closed");
    }

    // `control_mu_` is held.  Keep close asserted until the final concurrent
    // close caller has completed; this closes the reopen-between-closes race.
    void finish_close_request_locked() noexcept
    {
        const auto prior = close_requests_.fetch_sub(
            1, std::memory_order_acq_rel);
        if (prior != 1) return;
        if ((reader_started_.load(std::memory_order_acquire)
             && !reader_joined_.load(std::memory_order_acquire))
            || active_session_.load(std::memory_order_acquire) != 0)
            return;
        std::lock_guard<std::mutex> lk(state_mu_);
        closing_ = false;
        open_cv_.notify_all();
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

            if (frame_buffer_.size() > kMaxInboundFrameBytes)
                throw std::runtime_error("oversized private WS frame");
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

            if (frame_buffer_.size() > kMaxInboundFrameBytes)
                throw std::runtime_error("oversized private WS frame");
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
            // A private application fact received before the exact login
            // acknowledgement cannot be silently discarded or reordered.
            // There is no safe delivery route until the subscription gate has
            // completed, so make the entire stream terminal instead.
            std::cerr << "BitgetPrivateWsTransport: unexpected private frame "
                         "before login acknowledgement\n";
            return false;
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

            if (frame_buffer_.size() > kMaxInboundFrameBytes)
                throw std::runtime_error("oversized private WS frame");
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
            if (progress == subscription_progress::unexpected)
            {
                // Do not swallow an execution/account/position frame while
                // still pre-ready. It cannot be replayed into source order
                // after the gate, so fail closed and reconcile.
                std::cerr << "BitgetPrivateWsTransport: unexpected private "
                             "frame before subscription acknowledgement\n";
                return false;
            }

            if (progress == subscription_progress::ready)
                return true;
        }
        return false;
    }

    run_result run_once(std::uint64_t session)
    {
        bool reached_open = false;
        try
        {
            if (!session_running(session)) return run_result::stopped;
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
                    // Enforce the cap before login/subscription reads as well
                    // as normal private frames. A huge control-looking frame
                    // must not force an unbounded Beast buffer allocation.
                    socket.read_message_max(kMaxInboundFrameBytes);
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
            if (!opened || !session_running(session))
                return !session_running(session)
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
                return !session_running(session)
                    ? run_result::stopped : run_result::login_error;

            if (!await_login())
            {
                if (!session_running(session))
                    return run_result::stopped;
                return run_result::login_error;
            }

            // Subscribe order / fill / position.
            if (!send_text_bounded(build_private_subscribe_json(),
                                   open_stage_timeout))
                return !session_running(session)
                    ? run_result::stopped : run_result::login_error;
            if (!await_private_subscriptions())
                return !session_running(session)
                    ? run_result::stopped : run_result::login_error;
            last_ping_ = std::chrono::steady_clock::now();

            reached_open = true;
            ever_open_.store(true, std::memory_order_release);
            if (!set_state_for_session(
                    session, lifecycle::open,
                    "private WS open (order/fill/position)"))
                return run_result::stopped;

            std::string_view view;
            while (session_running(session))
            {
                if (!read_app_frame(view))
                    break;
                if (!session_running(session)) break;
                if (auto callback = message_cb_.load())
                    (*callback)(view);
            }
            return run_result::stopped;
        }
        catch (const beast::system_error& se)
        {
            if (!session_running(session))
                return run_result::stopped;
            (void)se;
            return reached_open ? run_result::network_error
                                : run_result::handshake_error;
        }
        catch (const std::exception& e)
        {
            if (!session_running(session))
                return run_result::stopped;
            std::cerr << "BitgetPrivateWsTransport: " << e.what() << "\n";
            return reached_open ? run_result::network_error
                                : run_result::handshake_error;
        }
    }

    void run(std::uint64_t session)
    {
        constexpr unsigned k_max_attempts = 10;
        auto delay = std::chrono::seconds(1);
        const auto max_delay = std::chrono::seconds(30);
        unsigned attempt = 0;

        if (!session_running(session)) return;
        if (run_once_override_ && override_reports_ready_)
        {
            ever_open_.store(true, std::memory_order_release);
            if (!set_state_for_session(
                    session, lifecycle::open, "test private WS open"))
                return;
        }

        while (session_running(session))
        {
            if (attempt > 0)
            {
                (void)set_state_for_session(
                    session, lifecycle::connecting,
                    "reconnecting private WS (attempt "
                        + std::to_string(attempt + 1) + "/"
                        + std::to_string(k_max_attempts) + ")");
                std::unique_lock<std::mutex> lk(cv_mu_);
                if (cv_.wait_for(lk, delay,
                                 [this, session] {
                                     return !session_running(session);
                                 }))
                    break;
                delay = std::min(delay * 2, max_delay);
            }

            auto r = run_once_override_ ? run_once_override_(stop_flag_)
                                        : run_once(session);
            if (!session_running(session)) return;
            if (r == run_result::stopped)
            {
                // Dropped after open without stop_flag → treat as network loss
                // so fatal_cb / reconnect policy can run (unless close() set stop).
                if (!session_running(session))
                    return;
                if (!ever_open_.load(std::memory_order_acquire))
                {
                    set_state_for_session(session, lifecycle::error,
                                          "private WS stopped before ready");
                    stop_session(session);
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
                set_state_for_session(session, lifecycle::error, buf);
                stop_session(session);
                return;
            }

            // Any post-ready loss is terminal; open() already proved that an
            // engine halt callback was registered for this session.
            char buf[192];
            std::snprintf(buf, sizeof(buf),
                          "bitget private WS lost: %s", what);
            set_state_for_session(session, lifecycle::error, buf);
            stop_session(session);
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

    ThreadSafeCallback<void(std::string_view)> message_cb_;
    ThreadSafeCallback<void(lifecycle, std::string_view)> status_cb_;
    LatchedFailureCallback fatal_cb_;
    // Couple the armed bit to the callback replacement, so a racing setter
    // cannot leave open() with an armed bit for an empty delivery/halt route.
    std::mutex callback_mu_;
    std::atomic<bool> message_callback_armed_{false};
    std::atomic<bool> fatal_callback_armed_{false};

    // `control_mu_` serializes reader ownership and ioc_ reset.  It is not
    // used for stop publication, so close() can wake a concurrent open().
    std::mutex control_mu_;
    std::thread reader_;
    std::atomic<bool> stop_flag_{false};
    // Set once login+subscribe succeed; open() ready-gate and reconnect policy.
    std::atomic<bool> ever_open_{false};
    std::atomic<bool> reader_started_{false};
    std::atomic<bool> reader_joined_{false};
    std::atomic<bool> open_in_progress_{false};
    std::atomic<bool> destroying_{false};
    std::atomic<std::uint64_t> active_session_{0};
    std::atomic<unsigned> close_requests_{0};
    std::uint64_t next_session_ = 1;
    run_once_fn run_once_override_;
    bool override_reports_ready_ = false;

    std::mutex cv_mu_;
    std::condition_variable cv_;

    mutable std::mutex state_mu_;
    std::condition_variable open_cv_;
    lifecycle state_ = lifecycle::closed;
    // Protected by state_mu_.  This is the internal `closing` state missing
    // from IFillTransport::lifecycle and prevents late ready publication.
    bool closing_ = false;
    // Protected by state_mu_. A status callback is part of the ready
    // handshake: it can close the transport, so open() waits for it to return
    // before acknowledging readiness.
    bool ready_status_callback_pending_ = false;

    std::chrono::steady_clock::time_point last_ping_{};

    static constexpr auto kPingInterval = std::chrono::seconds(30);
    static constexpr auto kPollWake = std::chrono::seconds(25);
    static constexpr auto kLoginTimeout = std::chrono::seconds(10);
    static constexpr auto kLoginPoll = std::chrono::milliseconds(500);
    // Bound open() wait: login timeout (10s) + TLS + slack.
    static constexpr auto kOpenReadyTimeout = std::chrono::seconds(15);
    // Applied to the websocket before every private read, including the
    // login/subscription phase. Larger frames terminate the transport.
    static constexpr std::size_t kMaxInboundFrameBytes = 1024U * 1024U;
};

} // namespace bitget

using BitgetPrivateWsTransport = bitget::BitgetPrivateWsTransport;

#endif // HAS_BITGET
