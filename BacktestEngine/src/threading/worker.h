#pragma once

#include "../core/event.h"
#include "ring_buffer.h"

#include <atomic>
#include <exception>
#include <iostream>

class Worker
{
public:
    virtual ~Worker() = default;

    virtual void on_event(const event_pointer& ev) = 0;

    template <std::size_t N, typename Policy>
    void run(RingBuffer<event_pointer, N, Policy>& inbound)
    {
        running_.store(true, std::memory_order_release);
        try
        {
            event_pointer ev;
            while (running_.load(std::memory_order_acquire))
            {
                if (inbound.try_pop(ev))
                    on_event(ev);
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

private:
    std::atomic<bool> running_{false};
    std::exception_ptr exception_;
    std::atomic<bool>* failure_flag_ = nullptr;
};
