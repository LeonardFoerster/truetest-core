#pragma once

#include "../core/event.h"
#include "../utils/log/logger.h"
#include "ring_buffer.h"
#include "spin_policy.h"

#ifdef HAS_DEBUG
#include "../debug/thread_stats.h"
#endif

#ifdef __x86_64__
#include <immintrin.h>
#endif

#include <atomic>
#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <string_view>
#include <thread>

class Worker
{
public:
    virtual ~Worker() = default;

    virtual void on_event(const event_pointer& ev) = 0;

    virtual const char* worker_name() const { return "worker"; }

    void set_spin_policy(spin_policy p) { spin_policy_ = p; }
    void set_max_consecutive_errors(unsigned n) { max_consecutive_errors_ = n; }

    template <std::size_t N, typename Policy>
    void run(RingBuffer<event_pointer, N, Policy>& inbound)
    {
        // A single atomic state makes stop() linearizable with startup.  If a
        // pre-start stop won, this CAS fails and we proceed directly to the
        // bounded queue-drain phase without ever publishing `running`.
        auto expected = lifecycle_state::idle;
        if (!state_.compare_exchange_strong(
                expected, lifecycle_state::running,
                std::memory_order_acq_rel,
                std::memory_order_acquire)
            && expected != lifecycle_state::stop_requested)
            return;
        event_pointer ev;
        unsigned idle_count = 0;
        unsigned consecutive_errors = 0;

        while (state_.load(std::memory_order_acquire)
               == lifecycle_state::running)
        {
#ifdef HAS_DEBUG
            auto t0 = std::chrono::high_resolution_clock::now();
#endif
            bool got = inbound.try_pop(ev);
#ifdef HAS_DEBUG
            auto t1 = std::chrono::high_resolution_clock::now();
            utilization_.poll_attempts++;
#endif

            if (got)
            {
                idle_count = 0;
#ifdef HAS_DEBUG
                utilization_.poll_hits++;
                utilization_.idle_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
                auto t2 = std::chrono::high_resolution_clock::now();
#endif
                try
                {
                    on_event(ev);
                    consecutive_errors = 0;
                }
                catch (const std::exception& e)
                {
                    ++consecutive_errors;
                    error_count_.fetch_add(1, std::memory_order_relaxed);
                    LOG_WARN(worker_name(),
                             "on_event exception (%u/%u consecutive): %s",
                             consecutive_errors, max_consecutive_errors_,
                             e.what());

                    if (consecutive_errors >= max_consecutive_errors_)
                    {
                        LOG_ERROR(worker_name(),
                                  "max consecutive errors reached (%u), halting",
                                  max_consecutive_errors_);
                        exception_ = std::current_exception();
                        if (failure_flag_)
                            failure_flag_->store(true, std::memory_order_release);
                        state_.store(
                            lifecycle_state::stopped,
                            std::memory_order_release);
                        notify_failure();
                        return;
                    }
                }
                catch (...)
                {
                    ++consecutive_errors;
                    error_count_.fetch_add(1, std::memory_order_relaxed);
                    LOG_WARN(worker_name(),
                             "on_event unknown exception (%u/%u consecutive)",
                             consecutive_errors, max_consecutive_errors_);

                    if (consecutive_errors >= max_consecutive_errors_)
                    {
                        LOG_ERROR(worker_name(),
                                  "max consecutive errors reached (%u), halting",
                                  max_consecutive_errors_);
                        exception_ = std::current_exception();
                        if (failure_flag_)
                            failure_flag_->store(true, std::memory_order_release);
                        state_.store(
                            lifecycle_state::stopped,
                            std::memory_order_release);
                        notify_failure();
                        return;
                    }
                }
#ifdef HAS_DEBUG
                utilization_.busy_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::high_resolution_clock::now() - t2).count();
                utilization_.events_processed++;
#endif
            }
            else
            {
#ifdef HAS_DEBUG
                utilization_.idle_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
#endif
                backoff(idle_count);
                ++idle_count;
            }
        }

        while (inbound.try_pop(ev))
        {
            try
            {
                on_event(ev);
            }
            catch (const std::exception& e)
            {
                error_count_.fetch_add(1, std::memory_order_relaxed);
                LOG_ERROR(worker_name(),
                          "on_event exception while draining: %s", e.what());
                exception_ = std::current_exception();
                if (failure_flag_)
                    failure_flag_->store(true, std::memory_order_release);
                state_.store(
                    lifecycle_state::stopped,
                    std::memory_order_release);
                notify_failure();
                return;
            }
            catch (...)
            {
                error_count_.fetch_add(1, std::memory_order_relaxed);
                LOG_ERROR(worker_name(),
                          "unknown on_event exception while draining");
                exception_ = std::current_exception();
                if (failure_flag_)
                    failure_flag_->store(true, std::memory_order_release);
                state_.store(
                    lifecycle_state::stopped,
                    std::memory_order_release);
                notify_failure();
                return;
            }
        }
        state_.store(lifecycle_state::stopped, std::memory_order_release);
    }

    void stop()
    {
        auto current = state_.load(std::memory_order_acquire);
        while (current == lifecycle_state::idle
               || current == lifecycle_state::running)
        {
            if (state_.compare_exchange_weak(
                    current, lifecycle_state::stop_requested,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire))
                break;
        }
    }
    bool is_running() const
    {
        return state_.load(std::memory_order_acquire)
            == lifecycle_state::running;
    }

    bool has_failed() const { return exception_ != nullptr; }
    std::exception_ptr get_exception() const { return exception_; }
    unsigned error_count() const { return error_count_.load(std::memory_order_relaxed); }

    void set_failure_flag(std::atomic<bool>& flag) { failure_flag_ = &flag; }
    void set_failure_callback(std::function<void(std::string_view)> cb)
    {
        failure_cb_ = std::move(cb);
    }

#ifdef HAS_DEBUG
    debug::thread_utilization utilization_;
public:
    const debug::thread_utilization& debug_utilization() const { return utilization_; }
#endif

private:
    enum class lifecycle_state : std::uint8_t
    {
        idle,
        running,
        stop_requested,
        stopped,
    };

    std::atomic<lifecycle_state> state_{lifecycle_state::idle};
    std::exception_ptr exception_;
    std::atomic<bool>* failure_flag_ = nullptr;
    std::function<void(std::string_view)> failure_cb_;
    spin_policy spin_policy_ = spin_policy::adaptive;
    unsigned max_consecutive_errors_ = 5;
    std::atomic<unsigned> error_count_{0};

    void notify_failure() noexcept
    {
        if (!failure_cb_) return;
        try { failure_cb_("worker exceeded consecutive-error budget"); }
        catch (...) {}
    }

    void backoff(unsigned idle_count)
    {
        switch (spin_policy_)
        {
        case spin_policy::spin:
            break;

        case spin_policy::yield:
            std::this_thread::yield();
            break;

        case spin_policy::adaptive:
            if (idle_count < 64)
            {
            }
            else if (idle_count < 320)
            {
#ifdef __x86_64__
                _mm_pause();
#else
                std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
            }
            else
            {
                std::this_thread::yield();
            }
            break;
        }
    }
};
