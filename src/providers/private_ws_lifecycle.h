#pragma once

#include "execution/fill_transport.h"
#include "providers/thread_safe_callback.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

// Shared ownership model for private websocket transports.  Venue transports
// retain their authentication, socket and worker implementation; this class
// owns the lifecycle facts which must not drift between venues.
namespace provider_ws
{

class private_ws_lifecycle
{
public:
    using lifecycle_type = IFillTransport::lifecycle;
    using message_callback_type = IFillTransport::message_cb;
    using status_callback_type = IFillTransport::status_cb;

    struct status_notification
    {
        lifecycle_type state;
        std::string note;
    };

    struct status_notifications
    {
        std::array<status_notification, 8> entries{};
        std::size_t size = 0;

        void push(lifecycle_type state, std::string note) noexcept
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

    explicit private_ws_lifecycle(const void* callback_owner) noexcept
        : callback_owner_(callback_owner)
    {
    }

    private_ws_lifecycle(const private_ws_lifecycle&) = delete;
    private_ws_lifecycle& operator=(const private_ws_lifecycle&) = delete;

    std::mutex& control_mutex() noexcept { return control_mu_; }

    bool try_claim_open() noexcept
    {
        if (destroying_.load(std::memory_order_acquire)) return false;
        bool expected = false;
        return open_in_progress_.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel);
    }

    void release_open_claim() noexcept
    {
        open_in_progress_.store(false, std::memory_order_release);
    }

    bool destroying() const noexcept
    {
        return destroying_.load(std::memory_order_acquire);
    }

    void mark_destroying() noexcept
    {
        destroying_.store(true, std::memory_order_release);
    }

    bool close_requested() const noexcept
    {
        return close_requests_.load(std::memory_order_acquire) != 0;
    }

    void begin_close_request() noexcept
    {
        close_requests_.fetch_add(1, std::memory_order_acq_rel);
    }

    bool callbacks_detachable_locked(bool worker_handles_joinable) const noexcept
    {
        return active_session_.load(std::memory_order_acquire) == 0
            && (!workers_started_.load(std::memory_order_acquire)
                || workers_joined_.load(std::memory_order_acquire))
            && !worker_handles_joinable;
    }

    void set_on_message(message_callback_type cb, bool callbacks_detachable)
    {
        std::lock_guard<std::mutex> lock(callback_mu_);
        if (!cb && !callbacks_detachable) return;
        const bool armed = static_cast<bool>(cb);
        message_callback_armed_.store(false, std::memory_order_release);
        message_cb_.store(std::move(cb));
        message_callback_armed_.store(armed, std::memory_order_release);
    }

    void set_on_status(status_callback_type cb, bool callbacks_detachable)
    {
        if (!cb && !callbacks_detachable) return;
        status_cb_.store(std::move(cb));
    }

    void set_fatal_disconnect_callback(
        std::function<void(std::string_view)> cb,
        bool callbacks_detachable)
    {
        if (cb)
        {
            std::lock_guard<std::mutex> lock(callback_mu_);
            fatal_callback_armed_.store(false, std::memory_order_release);
            fatal_cb_.store(std::move(cb));
            fatal_callback_armed_.store(true, std::memory_order_release);
            return;
        }

        std::lock_guard<std::mutex> lock(callback_mu_);
        if (!callbacks_detachable) return;
        fatal_callback_armed_.store(false, std::memory_order_release);
        fatal_cb_.store({});
    }

    bool message_callback_ready() const
    {
        std::lock_guard<std::mutex> lock(callback_mu_);
        return message_callback_armed_.load(std::memory_order_acquire)
            && static_cast<bool>(message_cb_.load());
    }

    bool fatal_callback_ready() const
    {
        std::lock_guard<std::mutex> lock(callback_mu_);
        return fatal_callback_armed_.load(std::memory_order_acquire);
    }

    // Delivery exceptions are transport failures.  A private frame cannot be
    // discarded merely because its consumer rejected it: callers execute in
    // their reader's guarded loop, which transitions the session to the
    // terminal failure path.
    void publish_message(std::string_view message)
    {
        if (auto callback = message_cb_.load())
            (*callback)(message);
    }

    void publish_failure(std::string_view reason) noexcept
    {
        fatal_cb_.publish(reason);
    }

    lifecycle_type state() const noexcept
    {
        std::lock_guard<std::mutex> lock(state_mu_);
        return state_;
    }

    bool in_status_callback() const noexcept
    {
        return status_callback_context() == callback_owner_;
    }

    void publish_statuses(status_notifications& notifications) noexcept
    {
        for (std::size_t i = 0; i < notifications.size; ++i)
            publish_status(notifications.entries[i].state,
                           notifications.entries[i].note);
        notifications.clear();
    }

    void set_state(lifecycle_type state, std::string note,
                   status_notifications* deferred = nullptr)
    {
        {
            std::lock_guard<std::mutex> lock(state_mu_);
            state_ = state;
            if (state == lifecycle_type::open || state == lifecycle_type::error)
                open_cv_.notify_all();
        }
        record_status(deferred, state, std::move(note));
    }

    bool set_state_for_session(std::uint64_t session, lifecycle_type state,
                               std::string note,
                               status_notifications* deferred = nullptr)
    {
        const bool gate_ready_on_callback = state == lifecycle_type::open
            && deferred == nullptr;
        {
            std::lock_guard<std::mutex> lock(state_mu_);
            if (active_session_.load(std::memory_order_acquire) != session
                || closing_)
                return false;
            if (state == lifecycle_type::open
                && stop_flag_.load(std::memory_order_acquire))
                return false;
            state_ = state;
            if (gate_ready_on_callback)
                ready_status_callback_pending_ = true;
            else if (state != lifecycle_type::open)
                ready_status_callback_pending_ = false;
            if (state == lifecycle_type::error || !gate_ready_on_callback)
                open_cv_.notify_all();
        }
        record_status(deferred, state, std::move(note));
        if (gate_ready_on_callback)
        {
            std::lock_guard<std::mutex> lock(state_mu_);
            ready_status_callback_pending_ = false;
            open_cv_.notify_all();
        }
        return true;
    }

    std::uint64_t begin_session_locked(
        std::string note, status_notifications* deferred = nullptr)
    {
        const auto session = next_session_++;
        stop_flag_.store(false, std::memory_order_release);
        active_session_.store(session, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(state_mu_);
            closing_ = false;
            ready_status_callback_pending_ = false;
            state_ = lifecycle_type::connecting;
            open_cv_.notify_all();
        }
        record_status(deferred, lifecycle_type::connecting, std::move(note));
        return session;
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
        std::lock_guard<std::mutex> lock(state_mu_);
        return state_ == lifecycle_type::open && !ready_status_callback_pending_
            && !closing_
            && active_session_.load(std::memory_order_acquire) != 0
            && !stop_flag_.load(std::memory_order_acquire);
    }

    template <typename Rep, typename Period>
    bool wait_until_ready(std::uint64_t session,
                          std::chrono::duration<Rep, Period> timeout)
    {
        std::unique_lock<std::mutex> lock(state_mu_);
        open_cv_.wait_for(lock, timeout, [this] {
            return (state_ == lifecycle_type::open && !ready_status_callback_pending_)
                || state_ == lifecycle_type::error
                || stop_flag_.load(std::memory_order_acquire);
        });
        return state_ == lifecycle_type::open && !ready_status_callback_pending_
            && session_running_locked(session);
    }

    template <typename StopFn>
    void request_stop(StopFn&& stop_transport) noexcept
    {
        stop_flag_.store(true, std::memory_order_release);
        stop_flag_.notify_all();
        {
            std::lock_guard<std::mutex> lock(state_mu_);
            closing_ = true;
            open_cv_.notify_all();
        }
        try { std::forward<StopFn>(stop_transport)(); }
        catch (...) {}
    }

    template <typename StopFn>
    void stop_session(std::uint64_t session, StopFn&& stop_transport) noexcept
    {
        if (active_session_.load(std::memory_order_acquire) == session)
            request_stop(std::forward<StopFn>(stop_transport));
    }

    void mark_workers_started() noexcept
    {
        workers_joined_.store(false, std::memory_order_release);
        workers_started_.store(true, std::memory_order_release);
    }

    void mark_workers_joined() noexcept
    {
        if (workers_started_.load(std::memory_order_acquire))
            workers_joined_.store(true, std::memory_order_release);
    }

    bool workers_started() const noexcept
    {
        return workers_started_.load(std::memory_order_acquire);
    }

    bool workers_joined() const noexcept
    {
        return workers_joined_.load(std::memory_order_acquire);
    }

    bool needs_teardown(bool worker_handles_joinable) const noexcept
    {
        return (workers_started_.load(std::memory_order_acquire)
                && !workers_joined_.load(std::memory_order_acquire))
            || worker_handles_joinable
            || active_session_.load(std::memory_order_acquire) != 0;
    }

    void mark_closed(bool had_active_session, bool had_worker,
                     status_notifications* deferred = nullptr)
    {
        bool notify_closed = had_active_session || had_worker;
        {
            std::lock_guard<std::mutex> lock(state_mu_);
            notify_closed = notify_closed || state_ != lifecycle_type::closed;
            state_ = lifecycle_type::closed;
            ready_status_callback_pending_ = false;
            closing_ = close_requested();
            open_cv_.notify_all();
        }
        if (notify_closed)
            record_status(deferred, lifecycle_type::closed, "closed");
    }

    void clear_active_session() noexcept
    {
        active_session_.store(0, std::memory_order_release);
    }

    std::uint64_t active_session() const noexcept
    {
        return active_session_.load(std::memory_order_acquire);
    }

    bool has_active_session() const noexcept
    {
        return active_session_.load(std::memory_order_acquire) != 0;
    }

    std::atomic<bool>& stop_token() noexcept { return stop_flag_; }
    const std::atomic<bool>& stop_token() const noexcept { return stop_flag_; }

    bool workers_unjoined() const noexcept
    {
        return workers_started_.load(std::memory_order_acquire)
            && !workers_joined_.load(std::memory_order_acquire);
    }

    void mark_ever_open() noexcept
    {
        ever_open_.store(true, std::memory_order_release);
    }

    void clear_ever_open() noexcept
    {
        ever_open_.store(false, std::memory_order_release);
    }

    bool ever_open() const noexcept
    {
        return ever_open_.load(std::memory_order_acquire);
    }

    bool producer_joined() const noexcept
    {
        return workers_started_.load(std::memory_order_acquire)
            && workers_joined_.load(std::memory_order_acquire)
            && active_session_.load(std::memory_order_acquire) == 0;
    }

    void finish_close_request(bool workers_unjoined) noexcept
    {
        const auto prior = close_requests_.fetch_sub(
            1, std::memory_order_acq_rel);
        if (prior != 1) return;
        if (workers_unjoined
            || active_session_.load(std::memory_order_acquire) != 0)
            return;
        std::lock_guard<std::mutex> lock(state_mu_);
        closing_ = false;
        open_cv_.notify_all();
    }

private:
    void record_status(status_notifications* deferred, lifecycle_type state,
                       std::string note) noexcept
    {
        if (deferred)
        {
            deferred->push(state, std::move(note));
            return;
        }
        publish_status(state, note);
    }

    void publish_status(lifecycle_type state, std::string_view note) noexcept
    {
        auto& context = status_callback_context();
        struct restore_context
        {
            const void*& slot;
            const void* previous;
            ~restore_context() { slot = previous; }
        } restore{context, context};
        context = callback_owner_;
        if (auto callback = status_cb_.load())
        {
            try { (*callback)(state, note); }
            catch (...) {}
        }
    }

    static const void*& status_callback_context() noexcept
    {
        static thread_local const void* current = nullptr;
        return current;
    }

private:
    const void* callback_owner_ = nullptr;
    std::mutex control_mu_;
    mutable std::mutex callback_mu_;
    mutable std::mutex state_mu_;
    std::condition_variable open_cv_;
    ThreadSafeCallback<void(std::string_view)> message_cb_;
    ThreadSafeCallback<void(lifecycle_type, std::string_view)> status_cb_;
    LatchedFailureCallback fatal_cb_;
    std::atomic<bool> message_callback_armed_{false};
    std::atomic<bool> fatal_callback_armed_{false};
    std::atomic<bool> stop_flag_{false};
    std::atomic<bool> ever_open_{false};
    std::atomic<bool> workers_started_{false};
    std::atomic<bool> workers_joined_{false};
    std::atomic<bool> open_in_progress_{false};
    std::atomic<bool> destroying_{false};
    std::atomic<std::uint64_t> active_session_{0};
    std::atomic<unsigned> close_requests_{0};
    std::uint64_t next_session_ = 1;
    lifecycle_type state_ = lifecycle_type::closed;
    bool closing_ = false;
    bool ready_status_callback_pending_ = false;
};

} // namespace provider_ws
