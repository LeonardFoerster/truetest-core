#pragma once

#include "../core/event.h"
#include "ring_buffer.h"
#include "spin_policy.h"

#ifdef HAS_DEBUG
#include "../debug/thread_stats.h"
#endif

#ifdef __x86_64__
#include <immintrin.h>
#endif

#include <atomic>
#include <exception>
#include <iostream>
#include <thread>

class Worker
{
public:
    virtual ~Worker() = default;

    virtual void on_event(const event_pointer& ev) = 0;

    void set_spin_policy(spin_policy p) { spin_policy_ = p; }

    template <std::size_t N, typename Policy>
    void run(RingBuffer<event_pointer, N, Policy>& inbound)
    {
        running_.store(true, std::memory_order_release);
        try
        {
            event_pointer ev;
            unsigned idle_count = 0;
            while (running_.load(std::memory_order_acquire))
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
                    on_event(ev);
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

            // Drain remaining events after shutdown signal
            while (inbound.try_pop(ev))
                on_event(ev);
        }
        catch (...)
        {
            exception_ = std::current_exception();
            if (failure_flag_)
                failure_flag_->store(true, std::memory_order_release);
            running_.store(false, std::memory_order_release);
        }
    }

    void stop() { running_.store(false, std::memory_order_release); }
    bool is_running() const { return running_.load(std::memory_order_acquire); }

    bool has_failed() const { return exception_ != nullptr; }
    std::exception_ptr get_exception() const { return exception_; }

    // Set shared failure flag (engine checks this alongside halt_flag)
    void set_failure_flag(std::atomic<bool>& flag) { failure_flag_ = &flag; }

#ifdef HAS_DEBUG
    debug::thread_utilization utilization_;
public:
    const debug::thread_utilization& debug_utilization() const { return utilization_; }
#endif

private:
    std::atomic<bool> running_{false};
    std::exception_ptr exception_;
    std::atomic<bool>* failure_flag_ = nullptr;
    spin_policy spin_policy_ = spin_policy::adaptive;

    void backoff(unsigned idle_count)
    {
        switch (spin_policy_)
        {
        case spin_policy::spin:
            // Pure busy-wait — do nothing
            break;

        case spin_policy::yield:
            std::this_thread::yield();
            break;

        case spin_policy::adaptive:
            if (idle_count < 64)
            {
                // Phase 1: spin (do nothing, ~64 iterations)
            }
            else if (idle_count < 320)
            {
                // Phase 2: pause hint to reduce power/pipeline pressure
#ifdef __x86_64__
                _mm_pause();
#else
                // ARM or other: compiler fence as lightweight pause
                std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
            }
            else
            {
                // Phase 3: yield to OS scheduler
                std::this_thread::yield();
            }
            break;
        }
    }
};
