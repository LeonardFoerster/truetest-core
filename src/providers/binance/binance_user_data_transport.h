#pragma once
#ifdef HAS_BINANCE

#include "../../execution/fill_transport.h"
#include "binance_parser.h"
#include "binance_rest_client.h"
#include "providers/bounded_ws_open.h"
#include "providers/recovery_payload.h"
#include "providers/thread_safe_callback.h"

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>

#include <openssl/ssl.h>

#include <atomic>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

struct binance_keepalive_policy
{
    std::chrono::seconds interval     = std::chrono::seconds(30 * 60);
    std::chrono::seconds retry_delay  = std::chrono::seconds(15);
    int                  max_retries  = 3;
};

namespace binance_keepalive_detail {

inline std::string authoritative_listen_key(std::string_view body)
{
    if (!provider_recovery::is_authoritative_object(body)) return {};
    std::string_view key;
    if (!provider_recovery::top_level_plain_string(body, "listenKey", key)
        || key.empty())
        return {};
    return std::string(key);
}

struct ka_response
{
    int status = 0;
};

using put_fn  = std::function<ka_response(const std::string& listen_key)>;
// POST must write the new listenKey into out_key on success.
using post_fn = std::function<ka_response(std::string& out_key)>;

struct tick_result
{
    enum class kind { ok, rotated, error, stopped };
    kind k = kind::ok;
    std::string new_key;  // set when k == rotated
    std::string note;
};

// PUT up to max_retries; on total failure, rotate via POST. wait_fn sleeps
// up to `delay`, returning true if stop was observed during the wait.
template <typename WaitFn>
inline tick_result keepalive_tick(const binance_keepalive_policy& pol,
                                  const std::string& current_key,
                                  const put_fn&  put_call,
                                  const post_fn& post_call,
                                  std::atomic<bool>& stop,
                                  WaitFn wait_fn)
{
    tick_result r;
    for (int attempt = 1; attempt <= pol.max_retries; ++attempt)
    {
        if (stop.load()) { r.k = tick_result::kind::stopped; return r; }
        auto resp = put_call ? put_call(current_key) : ka_response{500};
        if (resp.status >= 200 && resp.status < 300)
        {
            r.k = tick_result::kind::ok;
            r.note = "listenKey refreshed";
            return r;
        }
        if (attempt < pol.max_retries)
        {
            if (wait_fn(pol.retry_delay, stop))
            {
                r.k = tick_result::kind::stopped;
                return r;
            }
        }
    }

    if (stop.load()) { r.k = tick_result::kind::stopped; return r; }

    std::string rotated_key;
    auto resp = post_call ? post_call(rotated_key) : ka_response{500};
    if (resp.status >= 200 && resp.status < 300 && !rotated_key.empty())
    {
        r.k = tick_result::kind::rotated;
        r.new_key = std::move(rotated_key);
        r.note = "listenKey rotated after repeated keepalive failures";
        return r;
    }

    r.k = tick_result::kind::error;
    r.note = "listenKey keepalive failed and rotation failed";
    return r;
}

}

class BinanceUserDataTransport : public IFillTransport
{
public:
    enum class run_result { stopped, network_error, handshake_error };
    enum class next_step  { retry_after, give_up, stop };
    using create_listen_key_fn =
        std::function<BinanceRestClient::response()>;
    using delete_listen_key_fn =
        std::function<void(const std::string&)>;
    using run_once_fn = std::function<run_result(std::atomic<bool>& stop)>;
    using keepalive_once_fn =
        std::function<binance_keepalive_detail::tick_result()>;

    BinanceUserDataTransport(std::shared_ptr<BinanceRestClient> rest,
                             std::string ws_host = "stream.binance.com",
                             std::string ws_port = "9443",
                             binance_keepalive_policy policy = {},
                             std::string listen_key_path
                                 = "/api/v3/userDataStream")
        : rest_(std::move(rest))
        , ws_host_(std::move(ws_host))
        , ws_port_(std::move(ws_port))
        , listen_key_path_(std::move(listen_key_path))
        , keepalive_policy_(policy)
        , ws_ctx_(boost::asio::ssl::context::tlsv12_client)
    {
        ws_ctx_.set_default_verify_paths();
        ws_ctx_.set_verify_mode(boost::asio::ssl::verify_peer);

        // TLS session resumption across reconnects: 24h disconnect cap +
        // monthly testnet wipes mean this socket genuinely flaps. Cache one
        // most-recent ticket; OpenSSL falls back to a full handshake if the
        // server rejects it.
        SSL_CTX* raw = ws_ctx_.native_handle();
        SSL_CTX_set_session_cache_mode(
            raw, SSL_SESS_CACHE_CLIENT | SSL_SESS_CACHE_NO_INTERNAL_STORE);
        SSL_CTX_set_ex_data(raw, ex_data_index(), this);
        SSL_CTX_sess_set_new_cb(raw, &BinanceUserDataTransport::on_new_session);
        if (rest_)
        {
            const auto path = listen_key_path_;
            create_listen_key_ = [client = rest_, path] {
                return client->post_unsigned(path);
            };
            delete_listen_key_ = [client = rest_, path](const std::string& key) {
                (void)client->safety_del(
                    path, "listenKey=" + binance::url_encode(key),
                    std::chrono::seconds(1));
            };
        }
    }

    // Deterministic seam for readiness/teardown tests. Production always uses
    // the REST-backed constructor above.
    BinanceUserDataTransport(create_listen_key_fn create_key,
                             delete_listen_key_fn delete_key,
                             run_once_fn run_once,
                             keepalive_once_fn keepalive_once = {},
                             binance_keepalive_policy policy = {},
                             bool override_reports_ready = false)
        : BinanceUserDataTransport(nullptr, "localhost", "1")
    {
        create_listen_key_ = std::move(create_key);
        delete_listen_key_ = std::move(delete_key);
        run_once_override_ = std::move(run_once);
        keepalive_once_override_ = std::move(keepalive_once);
        keepalive_policy_ = policy;
        override_reports_ready_ = override_reports_ready;
    }

    ~BinanceUserDataTransport() override
    {
        // A status callback emitted by close() must not resurrect a private
        // reader while this destructor is about to release TLS/session state.
        destroying_.store(true, std::memory_order_release);
        close();
        SSL_SESSION* sess = cached_session_.exchange(
            nullptr, std::memory_order_acq_rel);
        if (sess) SSL_SESSION_free(sess);
    }

    BinanceUserDataTransport(const BinanceUserDataTransport&) = delete;
    BinanceUserDataTransport& operator=(const BinanceUserDataTransport&) = delete;
    BinanceUserDataTransport(BinanceUserDataTransport&&) = delete;
    BinanceUserDataTransport& operator=(BinanceUserDataTransport&&) = delete;

    bool open() override
    {
        // Avoid queuing a second opener behind the ready gate.  A concurrent
        // open is not a request to replace a live private stream; it must
        // either observe a ready session or fail closed.
        if (open_in_progress_.load(std::memory_order_acquire))
            return false;
        if (destroying_.load(std::memory_order_acquire))
            return false;

        // One owner at a time may create a listen key, start workers, or reap
        // a prior session.  `close()` first requests stop without this mutex,
        // so it can still interrupt the ready wait below.
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

        // A close already requested owns the next terminal transition.  Do
        // not let a queued concurrent open reset stop_flag_ and resurrect a
        // private stream before that close has obtained its join proof.
        if (close_requests_.load(std::memory_order_acquire) != 0)
            return finish_open(false);
        if (session_is_ready_locked()) return finish_open(true);

        // Keep the whole invocation exclusive, including deferred status
        // delivery: a reentrant status callback must not turn an error/close
        // transition into an implicit nested open.
        open_in_progress_.store(true, std::memory_order_release);
        owns_open_call = true;

        // Private execution must have both sides of its safety contract before
        // any venue resource is created: a delivery sink for authoritative
        // facts and a terminal-disconnect route for losing that source.
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

        // A terminal reader leaves a joinable thread behind until the next
        // owner reaps it.  Never overwrite a std::thread or reuse ioc_ before
        // that proof of reader exit exists.
        if ((workers_started_.load(std::memory_order_acquire)
             && !workers_joined_.load(std::memory_order_acquire))
            || reader_.joinable() || keepalive_.joinable()
            || active_session_.load(std::memory_order_acquire) != 0)
            teardown_locked(&deferred_status);

        if (close_requests_.load(std::memory_order_acquire) != 0)
            return finish_open(false);

        if (!create_listen_key_)
        {
            set_state(lifecycle::error, "no REST client", &deferred_status);
            return finish_open(false);
        }

        const auto session = next_session_++;
        begin_session_locked(session, "creating listenKey", &deferred_status);

        BinanceRestClient::response resp;
        try
        {
            resp = create_listen_key_();
        }
        catch (...)
        {
            (void)set_state_for_session(
                session, lifecycle::error, "listenKey create threw",
                &deferred_status);
            stop_session(session);
            teardown_locked(&deferred_status);
            return finish_open(false);
        }
        const auto created_key =
            binance_keepalive_detail::authoritative_listen_key(resp.body);
        if (!session_running(session))
        {
            // close() can win while the synchronous REST create is in
            // flight.  The response key belongs to this cancelled session and
            // must not be retained or leaked into a later reopen.
            if (delete_listen_key_ && !created_key.empty())
            {
                try { delete_listen_key_(created_key); }
                catch (...) {}
            }
            teardown_locked(&deferred_status);
            return finish_open(false);
        }
        if (resp.status < 200 || resp.status >= 300 || created_key.empty())
        {
            // A non-2xx response can still carry a syntactically valid key;
            // never leave that cancelled server-side session alive merely
            // because its HTTP status was not usable for readiness.
            if (delete_listen_key_ && !created_key.empty())
            {
                try { delete_listen_key_(created_key); }
                catch (...) {}
            }
            set_state_for_session(
                session, lifecycle::error,
                "listenKey create HTTP " + std::to_string(resp.status),
                &deferred_status);
            stop_session(session);
            teardown_locked(&deferred_status);
            return finish_open(false);
        }

        {
            std::lock_guard<std::mutex> lk(listen_key_mu_);
            listen_key_ = created_key;
        }
        if (current_listen_key().empty())
        {
            set_state_for_session(session, lifecycle::error,
                                  "listenKey missing in response",
                                  &deferred_status);
            stop_session(session);
            teardown_locked(&deferred_status);
            return finish_open(false);
        }

        ever_open_.store(false, std::memory_order_release);
        workers_joined_.store(false, std::memory_order_release);
        try
        {
            reader_ = std::thread([this, session] { run(session); });
            keepalive_ = std::thread([this, session] {
                keepalive_loop(session);
            });
            workers_started_.store(true, std::memory_order_release);
        }
        catch (...)
        {
            (void)set_state_for_session(session, lifecycle::error,
                                        "private worker start failed",
                                        &deferred_status);
            stop_session(session);
            teardown_locked(&deferred_status);
            return finish_open(false);
        }

        // Do not retain ownership of control_mu_ while waiting for readiness.
        // A reader callback is allowed to enter the halt path synchronously;
        // that path may close the transport, which must be able to acquire
        // the lifecycle owner and join rather than deadlocking behind open().
        control.unlock();
        publish_statuses(deferred_status);
        bool ready = false;
        {
            std::unique_lock<std::mutex> lk(state_mu_);
            open_cv_.wait_for(lk, std::chrono::seconds(5), [this] {
                return (state_ == lifecycle::open
                        && !ready_status_callback_pending_)
                    || state_ == lifecycle::error
                    || stop_flag_.load(std::memory_order_acquire);
            });
            ready = state_ == lifecycle::open
                && !ready_status_callback_pending_
                && session_running_locked(session);
        }

        control.lock();
        if (ready
            && close_requests_.load(std::memory_order_acquire) == 0
            && session_is_ready_locked())
        {
            return finish_open(true);
        }

        teardown_locked(&deferred_status);
        return finish_open(false);
    }

    void close() override
    {
        // A lifecycle status callback can originate on the reader or
        // keepalive thread while a different owner is already joining that
        // worker under control_mu_.  Re-entering close() in that callback
        // must only request the stop: trying to acquire control_mu_ would
        // create reader -> control_mu_ -> join(reader).  The outer open/close
        // owner observes stop_flag_ and performs the join/closed transition.
        if (status_callback_context() == this)
        {
            request_stop();
            return;
        }

        // Linearize close against a ready publication before waiting for the
        // lifecycle owner.  This is intentionally outside control_mu_: an
        // open() blocked in its ready gate must be interruptible immediately.
        close_requests_.fetch_add(1, std::memory_order_acq_rel);
        request_stop();
        std::unique_lock<std::mutex> control(control_mu_);
        status_notifications deferred_status;
        const bool caller_is_worker = called_from_worker_locked();
        teardown_locked(&deferred_status);
        // A status callback can invoke close() from the reader/keepalive
        // thread.  It must not reacquire control_mu_ after publishing: a
        // concurrent opener can otherwise hold control_mu_ while joining this
        // worker, while this worker waits to finish close().  Consume this
        // close request now; the next non-worker owner reaps the stopped
        // generation and clears closing_.
        if (caller_is_worker)
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
        // Do not let a live/private session lose its delivery sink.  The
        // bridge may detach it after close() has joined both workers, but a
        // clear during any active or unjoined generation is refused.
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

    // A fatal user-data loss must synchronously enter the engine halt path.
    // open() refuses until this callback is present; a private stream that
    // merely notices a disconnect without an owner is fill-blind.
    void set_fatal_disconnect_callback(
        std::function<void(std::string_view reason)> cb) override
    {
        // Installing/replacing a non-empty callback can synchronously replay
        // a previously latched failure. Do that without control_mu_ so a
        // callback which enters close() cannot self-deadlock.
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
        // Do not permit an already-running private stream to become
        // callback-less.  A cleared callback is only safe after close() has
        // joined the reader and keepalive workers.
        if (!callbacks_detachable_locked())
            return;
        fatal_callback_armed_.store(false, std::memory_order_release);
        fatal_cb_.store({});
    }

    std::string listen_key() const
    {
        std::lock_guard<std::mutex> lk(listen_key_mu_);
        return listen_key_;
    }

    // A provider can forward this as private_execution_producer_joined().
    // It remains false before the first worker launch and after every new
    // launch, and becomes true only after close() has joined both workers.
    bool private_execution_producer_joined() const noexcept
    {
        return workers_started_.load(std::memory_order_acquire)
            && workers_joined_.load(std::memory_order_acquire)
            && active_session_.load(std::memory_order_acquire) == 0;
    }

    struct reconnect_state
    {
        int attempt = 0;
        long long last_open_ms = 0;
        bool stop = false;
    };

    static next_step decide_next(reconnect_state s,
                                 run_result last,
                                 int max_attempts,
                                 long long now_ms,
                                 long long reset_threshold_ms)
    {
        if (s.stop || last == run_result::stopped) return next_step::stop;

        if (s.last_open_ms > 0 &&
            (now_ms - s.last_open_ms) > reset_threshold_ms)
            return next_step::retry_after;

        if (s.attempt + 1 >= max_attempts) return next_step::give_up;
        return next_step::retry_after;
    }

private:
    struct status_notification
    {
        lifecycle state;
        std::string note;
    };
    struct status_notifications
    {
        // Lifecycle transitions during one open/close path are bounded. Keep
        // the deferred callback handoff allocation-free, including from the
        // destructor's close() path.
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

    static int ex_data_index()
    {
        static const int idx = SSL_CTX_get_ex_new_index(
            0, const_cast<char*>("BinanceUserDataTransport::this"),
            nullptr, nullptr, nullptr);
        return idx;
    }

    static int on_new_session(SSL* ssl, SSL_SESSION* session)
    {
        SSL_CTX* ctx = SSL_get_SSL_CTX(ssl);
        auto* self = static_cast<BinanceUserDataTransport*>(
            SSL_CTX_get_ex_data(ctx, ex_data_index()));
        if (!self) return 0;
        SSL_SESSION* old = self->cached_session_.exchange(
            session, std::memory_order_acq_rel);
        if (old) SSL_SESSION_free(old);
        return 1;
    }

    std::string current_listen_key() const
    {
        std::lock_guard<std::mutex> lk(listen_key_mu_);
        return listen_key_;
    }

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

    // `control_mu_` is held by callers.  `active_session_` is cleared before
    // join, so the joined flag and thread ownership check are both required:
    // a callback can only be detached once no reader/keepalive can still
    // enter it, and no concurrent open can slip between the check and clear.
    bool callbacks_detachable_locked() const noexcept
    {
        return active_session_.load(std::memory_order_acquire) == 0
            && (!workers_started_.load(std::memory_order_acquire)
                || workers_joined_.load(std::memory_order_acquire))
            && !reader_.joinable() && !keepalive_.joinable();
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
            BinanceUserDataTransport*& slot;
            BinanceUserDataTransport* previous;
            ~restore_context() { slot = previous; }
        } restore{callback_context, callback_context};
        callback_context = this;
        if (auto callback = status_cb_.load())
        {
            try { (*callback)(s, note); }
            catch (...) {}
        }
    }

    static BinanceUserDataTransport*& status_callback_context() noexcept
    {
        static thread_local BinanceUserDataTransport* current = nullptr;
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

    // Only the owner generation can update the public lifecycle.  In
    // particular, a late ready/error from a reader being closed must not make
    // a newer session appear usable.
    bool set_state_for_session(std::uint64_t session, lifecycle s,
                               std::string note,
                               status_notifications* deferred = nullptr)
    {
        // A status callback may synchronously close this transport.  Do not
        // let open() report success until the callback has completed: the
        // callback is part of the ready linearization, not an asynchronous
        // observer of an already-admitted private stream.
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

    bool called_from_worker_locked() const noexcept
    {
        const auto self = std::this_thread::get_id();
        return (reader_.joinable() && reader_.get_id() == self)
            || (keepalive_.joinable() && keepalive_.get_id() == self);
    }

    // `control_mu_` is held.  The join is the ownership proof which permits
    // socket reset, listen-key deletion, and the next session start.
    void teardown_locked(status_notifications* deferred = nullptr)
    {
        request_stop();

        // A callback should request engine halt, not synchronously tear down
        // its own reader.  Returning here leaves a stopped, non-ready session
        // that a later external close/open will reap without self-join.
        if (called_from_worker_locked()) return;

        const bool had_active_session =
            active_session_.load(std::memory_order_acquire) != 0;
        const bool had_worker = reader_.joinable() || keepalive_.joinable();
        active_session_.store(0, std::memory_order_release);
        if (reader_.joinable()) reader_.join();
        if (keepalive_.joinable()) keepalive_.join();
        if (workers_started_.load(std::memory_order_acquire))
            workers_joined_.store(true, std::memory_order_release);

        {
            std::lock_guard<std::mutex> lk(ws_mu_);
            if (ws_)
            {
                try
                {
                    boost::beast::error_code ec;
                    ws_->close(boost::beast::websocket::close_code::normal, ec);
                }
                catch (...) {}
            }
            ws_.reset();
            ioc_.restart();
        }

        std::string key_to_delete;
        {
            std::lock_guard<std::mutex> lk(listen_key_mu_);
            key_to_delete = std::move(listen_key_);
            listen_key_.clear();
        }
        if (delete_listen_key_ && !key_to_delete.empty())
        {
            try { delete_listen_key_(key_to_delete); }
            catch (...) {}
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

    // `control_mu_` is held after this close caller has completed teardown.
    // Keep `closing_` asserted until the last concurrent close caller exits;
    // otherwise a queued open could restart between two close() calls.
    void finish_close_request_locked() noexcept
    {
        const auto prior = close_requests_.fetch_sub(
            1, std::memory_order_acq_rel);
        if (prior != 1) return;
        if ((workers_started_.load(std::memory_order_acquire)
             && !workers_joined_.load(std::memory_order_acquire))
            || active_session_.load(std::memory_order_acquire) != 0)
            return;
        std::lock_guard<std::mutex> lk(state_mu_);
        closing_ = false;
        open_cv_.notify_all();
    }

    run_result run_once(std::uint64_t session)
    {
        namespace beast = boost::beast;
        namespace websocket = beast::websocket;
        namespace net = boost::asio;
        namespace ssl = net::ssl;
        using tcp = net::ip::tcp;

        bool reached_open = false;
        try
        {
            constexpr auto connect_deadline = std::chrono::seconds(3);
            {
                std::lock_guard<std::mutex> lk(ws_mu_);
                ws_ = std::make_unique<
                    websocket::stream<beast::ssl_stream<tcp::socket>>>(ioc_, ws_ctx_);
            }
            std::string target = "/ws/" + current_listen_key();
            const bool ws_ready = provider_ws::open_tls_websocket(
                ioc_, *ws_, ws_host_, ws_port_, target, connect_deadline,
                [&](auto& socket) {
                    auto& lowest = beast::get_lowest_layer(socket);
                    const int yes = 1;
                    const int idle = 1, intvl = 1, cnt = 2;
                    const int fd = lowest.native_handle();
                    ::setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &yes, sizeof(yes));
                    ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
                    ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
                    ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
                    if (SSL_SESSION* sess =
                            cached_session_.load(std::memory_order_acquire))
                        SSL_set_session(socket.next_layer().native_handle(), sess);
                },
                [&](auto& socket) {
                    websocket::stream_base::timeout opt;
                    opt.handshake_timeout = std::chrono::seconds(3);
                    opt.idle_timeout = std::chrono::milliseconds(1500);
                    opt.keep_alive_pings = true;
                    socket.set_option(opt);
                    // Enforce a hard pre-allocation cap before the first
                    // private frame is read. User-data frames are facts, not
                    // a reason to permit unbounded allocation under attack.
                    socket.read_message_max(kMaxInboundFrameBytes);
                    socket.set_option(websocket::stream_base::decorator(
                        [](websocket::request_type& req) {
                            req.set(boost::beast::http::field::user_agent,
                                    "TrueTest/1.0");
                        }));
                });
            if (!ws_ready || !session_running(session))
                return !session_running(session)
                    ? run_result::stopped : run_result::handshake_error;

            reached_open = true;
            ever_open_.store(true, std::memory_order_release);
            if (!set_state_for_session(session, lifecycle::open,
                                       "user-data stream open"))
                return run_result::stopped;

            beast::flat_buffer buf;
            while (session_running(session))
            {
                buf.consume(buf.size());
                beast::error_code read_ec;
                if (ioc_.stopped()) ioc_.restart();
                ws_->async_read(buf, [&](beast::error_code ec, std::size_t) {
                    read_ec = ec;
                });
                ioc_.run();
                if (read_ec)
                    throw beast::system_error(read_ec);

                if (!session_running(session)) break;

                auto data = buf.data();
                std::string_view view(
                    static_cast<const char*>(data.data()), data.size());
                if (view.size() > kMaxInboundFrameBytes)
                    throw std::runtime_error("oversized private WS frame");

                if (auto callback = message_cb_.load())
                    (*callback)(view);
            }
            return run_result::stopped;
        }
        catch (const boost::beast::system_error& se)
        {
            if (!session_running(session)) return run_result::stopped;
            return reached_open ? run_result::network_error
                                : run_result::handshake_error;
        }
        catch (const std::exception&)
        {
            if (!session_running(session)) return run_result::stopped;
            return reached_open ? run_result::network_error
                                : run_result::handshake_error;
        }
    }

    void run(std::uint64_t session)
    {
        if (!session_running(session)) return;
        if (run_once_override_ && override_reports_ready_)
        {
            ever_open_.store(true, std::memory_order_release);
            if (!set_state_for_session(session, lifecycle::open,
                                       "test user-data stream open"))
                return;
        }
        auto r = run_once_override_ ? run_once_override_(stop_flag_)
                                    : run_once(session);
        if (!session_running(session)) return;
        if (r == run_result::stopped)
        {
            // A peer can close cleanly without an I/O error.  It is still a
            // terminal loss of the private source of truth, never a healthy
            // "open" session.
            r = run_result::network_error;
        }
        if (!ever_open_.load(std::memory_order_acquire))
        {
            set_state_for_session(session, lifecycle::error,
                                  "initial user-data stream handshake failed");
            stop_session(session);
            return;
        }

        const char* what =
            (r == run_result::network_error) ? "network error"
                                             : "handshake error";
        char fatal_reason[160];
        std::snprintf(fatal_reason, sizeof(fatal_reason),
                      "binance user-data WS lost: %s", what);
        set_state_for_session(session, lifecycle::error, fatal_reason);
        stop_session(session);
        fatal_cb_.publish(fatal_reason);
    }

    void keepalive_loop(std::uint64_t session)
    {
        while (session_running(session))
        {
            {
                std::unique_lock<std::mutex> lk(cv_mu_);
                cv_.wait_for(lk, keepalive_policy_.interval,
                             [this, session] { return !session_running(session); });
            }
            if (!session_running(session)) break;

            binance_keepalive_detail::tick_result out;
            try
            {
                if (keepalive_once_override_)
                {
                    out = keepalive_once_override_();
                }
                else
                {
                    if (!rest_) continue;
                    std::string current;
                    {
                        std::lock_guard<std::mutex> lk(listen_key_mu_);
                        current = listen_key_;
                    }
                    if (current.empty()) continue;

                    using binance_keepalive_detail::ka_response;
                    auto put_call = [this](const std::string& key) {
                        auto r = rest_->put_unsigned(
                            listen_key_path_,
                            "listenKey=" + binance::url_encode(key));
                        return ka_response{r.status};
                    };
                    auto post_call = [this](std::string& out_key) {
                        auto r = rest_->post_unsigned(listen_key_path_);
                        if (r.status >= 200 && r.status < 300)
                        {
                            out_key = binance_keepalive_detail::
                                authoritative_listen_key(r.body);
                        }
                        return ka_response{r.status};
                    };
                    auto wait_fn = [this](std::chrono::seconds delay,
                                          std::atomic<bool>& stop) -> bool {
                        std::unique_lock<std::mutex> lk(cv_mu_);
                        return cv_.wait_for(
                            lk, delay, [&stop] { return stop.load(); });
                    };
                    out = binance_keepalive_detail::keepalive_tick(
                        keepalive_policy_, current, put_call, post_call,
                        stop_flag_, wait_fn);
                }
            }
            catch (...)
            {
                out.k = binance_keepalive_detail::tick_result::kind::error;
                out.note = "listenKey keepalive threw";
            }

            using K = binance_keepalive_detail::tick_result::kind;
            if (out.k == K::ok)
            {
                (void)set_state_for_session(session, lifecycle::open,
                                            "listenKey refreshed");
            }
            else if (out.k == K::rotated || out.k == K::error)
            {
                // A newly issued listenKey belongs to a different websocket
                // path. Merely replacing listen_key_ leaves the existing
                // reader on an unverified source of truth. Reconnect cannot
                // be assumed here, so rotation is terminal just like a total
                // keepalive failure; halt/reconciliation owns recovery.
                const std::string_view reason = out.note.empty()
                    ? (out.k == K::rotated
                        ? std::string_view{
                              "binance listenKey rotated without verified reconnect"}
                        : std::string_view{
                              "binance listenKey keepalive failed"})
                    : std::string_view{out.note};
                if (out.k == K::rotated && delete_listen_key_
                    && !out.new_key.empty())
                {
                    // The replacement was never bound to a verified reader.
                    // Best-effort retirement cannot change the terminal
                    // outcome if venue cleanup itself fails.
                    try { delete_listen_key_(out.new_key); }
                    catch (...) {}
                }
                (void)set_state_for_session(session, lifecycle::error,
                                            std::string(reason));
                stop_session(session);
                fatal_cb_.publish(reason);
                break;
            }
        }
    }

    void interrupt_websocket() noexcept
    {
        try
        {
            std::lock_guard<std::mutex> lk(ws_mu_);
            if (ws_)
            {
                boost::beast::error_code ec;
                auto& lowest = boost::beast::get_lowest_layer(*ws_);
                lowest.cancel(ec);
                lowest.close(ec);
            }
            ioc_.stop();
        }
        catch (...) {}
    }

    std::shared_ptr<BinanceRestClient> rest_;
    std::string ws_host_;
    std::string ws_port_;
    std::string listen_key_path_;
    mutable std::mutex listen_key_mu_;
    std::string listen_key_;
    binance_keepalive_policy keepalive_policy_;
    boost::asio::io_context ioc_;
    boost::asio::ssl::context ws_ctx_;
    std::atomic<SSL_SESSION*> cached_session_{nullptr};

    std::unique_ptr<boost::beast::websocket::stream<
        boost::beast::ssl_stream<boost::asio::ip::tcp::socket>>> ws_;
    std::mutex ws_mu_;

    ThreadSafeCallback<void(std::string_view)> message_cb_;
    ThreadSafeCallback<void(lifecycle, std::string_view)> status_cb_;
    LatchedFailureCallback fatal_cb_;
    // Serializes the armed bit with replacement of its corresponding
    // callback.  An atomic armed bit alone can otherwise pair `true` with a
    // concurrently-cleared callback and let open() start fill-blind.
    std::mutex callback_mu_;
    std::atomic<bool> message_callback_armed_{false};
    std::atomic<bool> fatal_callback_armed_{false};

    // Serializes ownership of reader_/keepalive_, ioc_ reset, and the venue
    // listen key.  It is deliberately distinct from state_mu_: close() must
    // be able to request stop while open() owns this mutex and awaits ready.
    std::mutex control_mu_;
    std::thread reader_;
    std::thread keepalive_;
    std::atomic<bool> stop_flag_{false};
    std::atomic<bool> ever_open_{false};
    std::atomic<bool> workers_started_{false};
    std::atomic<bool> workers_joined_{false};
    std::atomic<bool> open_in_progress_{false};
    std::atomic<bool> destroying_{false};
    std::atomic<std::uint64_t> active_session_{0};
    std::atomic<unsigned> close_requests_{0};
    std::uint64_t next_session_ = 1;

    create_listen_key_fn create_listen_key_;
    delete_listen_key_fn delete_listen_key_;
    run_once_fn run_once_override_;
    keepalive_once_fn keepalive_once_override_;
    bool override_reports_ready_ = false;

    std::mutex cv_mu_;
    std::condition_variable cv_;

    mutable std::mutex state_mu_;
    std::condition_variable open_cv_;
    lifecycle state_ = lifecycle::closed;
    // Protected by state_mu_.  It supplies the close-vs-ready linearization
    // that lifecycle alone cannot express (IFillTransport has no `closing`).
    bool closing_ = false;
    // Also protected by state_mu_.  A reader-originated `open` status is not
    // ready until its callback returns, so a callback-triggered close wins
    // the open() readiness gate instead of racing a stale successful return.
    bool ready_status_callback_pending_ = false;

    // Private execution payloads are deliberately bounded before Beast grows
    // a frame buffer. Larger input is a terminal transport failure.
    static constexpr std::size_t kMaxInboundFrameBytes = 1024U * 1024U;
};

#endif // HAS_BINANCE
