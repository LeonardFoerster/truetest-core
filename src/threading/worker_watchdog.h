#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <iostream>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

// Liveness watchdog for long-lived threads that don't fit Worker's
// event-loop pattern (e.g. the futures dead-man's-switch heartbeat).
// Sources own a monotonic-ms atomic, update it inside their loop, and
// register it here with a deadline. A polling thread checks every
// registered source; if any goes longer than its deadline without a
// beat, the watchdog sets the engine halt flag and stops monitoring
// (single-shot — no stderr spam, no double-set).
// The "I'm alive" channel is a pull, not a push: the watchdog reads,
// the source writes. No callbacks back to the source. Sources that
// can't even update their atomic (deadlocked, OS-paused) are exactly
// the ones we want to detect, so we don't trust them to call us.
// Sources must outlive the watchdog. There's no unregister() because
// the only realistic lifecycle is "engine startup → run → shutdown",
// where everything goes down together.
class WorkerWatchdog
{
public:
    explicit WorkerWatchdog(std::chrono::milliseconds poll_interval =
                                std::chrono::milliseconds(500))
        : poll_interval_(poll_interval) {}

    ~WorkerWatchdog() { stop(); }

    WorkerWatchdog(const WorkerWatchdog&) = delete;
    WorkerWatchdog& operator=(const WorkerWatchdog&) = delete;
    WorkerWatchdog(WorkerWatchdog&&) = delete;
    WorkerWatchdog& operator=(WorkerWatchdog&&) = delete;

    // `last_alive_ms` must outlive this watchdog. `deadline_ms` is the
    // grace period before "missed beat" → halt. Callers should pick
    // deadline >= 2 × source's natural beat interval so a single-cycle
    // jitter (network flap, scheduler hiccup) doesn't trigger.
    void register_source(std::string name,
                         std::atomic<int64_t>* last_alive_ms,
                         int64_t deadline_ms)
    {
        std::lock_guard<std::mutex> lk(mu_);
        sources_.push_back({std::move(name), last_alive_ms, deadline_ms});
    }

    // The watchdog sets this when a registered source misses its
    // deadline. Caller wires it to engine.halt_flag_.
    void set_halt_flag(std::atomic<bool>& flag) { halt_flag_ = &flag; }

    // Optional callback fired when the first source goes hung. Receives
    // the source's name and the observed age in ms. Engine wires this to
    // trigger_halt() so the dashboard banner / event ring stay in sync.
    // Called BEFORE halt_flag_ is set so trigger_halt's exchange-gate
    // sees halt_flag_ still false on entry.
    void set_halt_callback(
        std::function<void(std::string_view name, std::int64_t age_ms)> cb)
    {
        halt_callback_ = std::move(cb);
    }

    void start()
    {
        if (thread_.joinable()) return;
        running_.store(true, std::memory_order_release);
        triggered_.store(false, std::memory_order_release);
        thread_ = std::thread([this] { run(); });
    }

    void stop()
    {
        if (!thread_.joinable()) return;
        running_.store(false, std::memory_order_release);
        cv_.notify_all();
        thread_.join();
    }

    bool triggered() const
    {
        return triggered_.load(std::memory_order_acquire);
    }

    static int64_t now_monotonic_ms()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    // Pure: is this source overdue?
    //  - last_alive_ms <= 0 → never beaten yet, treat as alive (the
    //    source might still be spinning up; the deadline starts on
    //    its first beat).
    //  - deadline_ms <= 0 → no deadline configured, treat as alive.
    static bool is_hung(int64_t now_ms, int64_t last_alive_ms,
                        int64_t deadline_ms)
    {
        if (last_alive_ms <= 0) return false;
        if (deadline_ms <= 0) return false;
        return (now_ms - last_alive_ms) > deadline_ms;
    }

private:
    struct registration
    {
        std::string name;
        std::atomic<int64_t>* last_alive_ms;
        int64_t deadline_ms;
    };

    void run()
    {
        while (running_.load(std::memory_order_acquire))
        {
            {
                std::unique_lock<std::mutex> lk(cv_mu_);
                cv_.wait_for(lk, poll_interval_,
                             [this] { return !running_.load(std::memory_order_acquire); });
            }
            if (!running_.load(std::memory_order_acquire)) break;

            const int64_t now = now_monotonic_ms();
            bool any_hung = false;
            std::string  first_hung_name;
            std::int64_t first_hung_age_ms = 0;

            {
                std::lock_guard<std::mutex> lk(mu_);
                for (const auto& s : sources_)
                {
                    const int64_t last =
                        s.last_alive_ms->load(std::memory_order_acquire);
                    if (is_hung(now, last, s.deadline_ms))
                    {
                        std::cerr << "WorkerWatchdog: source '" << s.name
                                  << "' missed liveness deadline ("
                                  << (now - last) << "ms since last beat, "
                                  << "deadline " << s.deadline_ms
                                  << "ms) — halting engine.\n";
                        if (!any_hung)
                        {
                            first_hung_name = s.name;
                            first_hung_age_ms = now - last;
                        }
                        any_hung = true;
                    }
                }
            }

            if (any_hung)
            {
                triggered_.store(true, std::memory_order_release);
                // Callback first: trigger_halt's halt_flag_.exchange gate
                // must observe halt_flag_ still false to publish the
                // reason / push the dashboard event.
                if (halt_callback_)
                    halt_callback_(first_hung_name, first_hung_age_ms);
                if (halt_flag_)
                    halt_flag_->store(true, std::memory_order_release);
                // Single-shot: stop polling so subsequent ticks don't
                // re-log. Engine teardown will call stop() when it
                // observes halt_flag_; we just exit the loop early.
                running_.store(false, std::memory_order_release);
                return;
            }
        }
    }

    std::chrono::milliseconds poll_interval_;
    std::mutex mu_;
    std::vector<registration> sources_;
    std::atomic<bool>* halt_flag_ = nullptr;
    std::function<void(std::string_view, std::int64_t)> halt_callback_;

    std::atomic<bool> running_{false};
    std::atomic<bool> triggered_{false};
    std::thread thread_;
    std::mutex cv_mu_;
    std::condition_variable cv_;
};
