#pragma once
#ifdef HAS_BINANCE

#include "providers/binance/binance_rest_client.h"
#include "threading/worker_watchdog.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

// Binance futures dead-man's switch: wraps POST /fapi/v1/countdownCancelAll
// and a heartbeat thread that refreshes the server-side countdown
// timer. If the engine dies between refreshes, Binance auto-cancels
// every open order on the configured symbol within `countdown_ms` of
// the last successful heartbeat.
// Layered safety with the orderly kill-switch:
//   - Orderly shutdown: kill-switch cancels orders + flattens position;
//     this DMS gets disarm()ed afterward (countdownTime=0).
//   - Catastrophic shutdown (SIGKILL / OOM / kernel panic / network gone):
//     kill-switch never runs. The DMS countdown still expires server-side
//     and Binance cancels orders.
// Critically: this only cancels ORDERS. Open futures positions stay
// open after auto-cancel; operator must close manually. The DMS is
// half a safety net; the kill-switch's flatten step is the other half.
// The class itself doesn't touch a socket — it routes everything
// through a `post_fn` callable, matching the bracket adapter's pattern.
// `make_binance_futures_dead_mans_switch(rest_client, ...)` is the
// production factory; tests inject a fake.
class BinanceFuturesDeadMansSwitch
{
public:
    struct response
    {
        int status = 0;
        std::string body;
    };

    using post_fn = std::function<response(std::string_view endpoint,
                                           std::string_view params)>;

    BinanceFuturesDeadMansSwitch(post_fn post,
                                 std::string symbol,
                                 int64_t countdown_ms,
                                 int64_t heartbeat_interval_ms)
        : post_(std::move(post))
        , symbol_(std::move(symbol))
        , countdown_ms_(countdown_ms)
        , heartbeat_interval_ms_(heartbeat_interval_ms)
    {}

    ~BinanceFuturesDeadMansSwitch() { stop(); }

    BinanceFuturesDeadMansSwitch(const BinanceFuturesDeadMansSwitch&) = delete;
    BinanceFuturesDeadMansSwitch& operator=(const BinanceFuturesDeadMansSwitch&) = delete;
    BinanceFuturesDeadMansSwitch(BinanceFuturesDeadMansSwitch&&) = delete;
    BinanceFuturesDeadMansSwitch& operator=(BinanceFuturesDeadMansSwitch&&) = delete;

    // Arms the server-side countdown and spawns the heartbeat thread.
    // Returns false if the initial arm fails — the operator should treat
    // that as "refuse to go live": the DMS contract is "if I die, the
    // venue cleans up", and we can't promise that without a confirmed
    // arm.
    bool start()
    {
        if (running_.load(std::memory_order_acquire)) return true;

        if (!post_countdown(countdown_ms_))
        {
            std::cerr << "BinanceFuturesDeadMansSwitch: initial arm "
                         "(POST /fapi/v1/countdownCancelAll, symbol="
                      << symbol_ << " countdown=" << countdown_ms_
                      << "ms) failed — refusing to start.\n";
            return false;
        }

        last_beat_ms_.store(WorkerWatchdog::now_monotonic_ms(),
                            std::memory_order_release);
        running_.store(true, std::memory_order_release);
        thread_ = std::thread([this] { run(); });
        return true;
    }

    // Stops the heartbeat thread. Does NOT disarm — caller must
    // explicitly call disarm() if they want the server-side timer
    // cancelled. Splitting these lets orderly shutdown interleave the
    // disarm with other cleanup REST calls; uncontrolled shutdown
    // (engine death) leaves the timer to expire on its own.
    void stop()
    {
        if (!running_.exchange(false, std::memory_order_acq_rel)) return;
        cv_.notify_all();
        if (thread_.joinable()) thread_.join();
    }

    // POST countdownTime=0. Idempotent — returns true on 2xx, including
    // when the server-side timer wasn't armed in the first place.
    bool disarm()
    {
        return post_countdown(/*ms=*/0);
    }

    // Watchdog liveness source. Updated on every successful heartbeat;
    // an external WorkerWatchdog uses this to detect a hung heartbeat
    // thread and halt the engine before the venue countdown fires.
    std::atomic<int64_t>& liveness_ts() { return last_beat_ms_; }

    int64_t heartbeat_interval_ms() const { return heartbeat_interval_ms_; }
    int64_t countdown_ms() const { return countdown_ms_; }

private:
    bool post_countdown(int64_t ms)
    {
        std::string params;
        params.reserve(symbol_.size() + 32);
        params.append("symbol=", 7);
        params.append(symbol_);
        params.append("&countdownTime=", 15);
        params.append(std::to_string(ms));

        const auto resp = post_("/fapi/v1/countdownCancelAll", params);
        return resp.status >= 200 && resp.status < 300;
    }

    void run()
    {
        // Heartbeat retry budget: one extra attempt after a brief pause
        // before declaring the cycle a miss. A single transient network
        // hiccup is forgivable; persistent failure goes silent and the
        // external watchdog catches it.
        constexpr auto retry_pause = std::chrono::milliseconds(500);

        while (running_.load(std::memory_order_acquire))
        {
            {
                std::unique_lock<std::mutex> lk(cv_mu_);
                cv_.wait_for(lk,
                             std::chrono::milliseconds(heartbeat_interval_ms_),
                             [this] {
                                 return !running_.load(std::memory_order_acquire);
                             });
            }
            if (!running_.load(std::memory_order_acquire)) break;

            if (post_countdown(countdown_ms_))
            {
                last_beat_ms_.store(WorkerWatchdog::now_monotonic_ms(),
                                    std::memory_order_release);
                continue;
            }

            // First attempt failed. One retry before falling silent.
            {
                std::unique_lock<std::mutex> lk(cv_mu_);
                if (cv_.wait_for(lk, retry_pause, [this] {
                        return !running_.load(std::memory_order_acquire);
                    }))
                    break;
            }

            if (post_countdown(countdown_ms_))
            {
                last_beat_ms_.store(WorkerWatchdog::now_monotonic_ms(),
                                    std::memory_order_release);
                continue;
            }

            std::cerr << "BinanceFuturesDeadMansSwitch: heartbeat failed "
                         "twice for symbol=" << symbol_
                      << "; watchdog will halt engine if persistent.\n";
            // Don't update last_beat_ms_ — watchdog reads it, sees the
            // stale value, halts when deadline elapses.
        }
    }

    post_fn post_;
    std::string symbol_;
    int64_t countdown_ms_;
    int64_t heartbeat_interval_ms_;

    std::atomic<int64_t> last_beat_ms_{0};
    std::atomic<bool>    running_{false};
    std::thread          thread_;
    std::mutex           cv_mu_;
    std::condition_variable cv_;
};

inline std::shared_ptr<BinanceFuturesDeadMansSwitch>
make_binance_futures_dead_mans_switch(
    std::shared_ptr<BinanceRestClient> client,
    std::string symbol,
    int64_t countdown_ms,
    int64_t heartbeat_interval_ms)
{
    auto post = [client](std::string_view ep, std::string_view p)
        -> BinanceFuturesDeadMansSwitch::response
    {
        // countdownCancelAll is signed (TRADE endpoint).
        auto r = client->post(std::string(ep), std::string(p));
        return {r.status, r.body};
    };
    return std::make_shared<BinanceFuturesDeadMansSwitch>(
        std::move(post), std::move(symbol),
        countdown_ms, heartbeat_interval_ms);
}

#endif // HAS_BINANCE
