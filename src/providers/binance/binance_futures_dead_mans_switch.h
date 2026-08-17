#pragma once
#ifdef HAS_BINANCE

// ============================================================
// LIVE-SAFETY SURFACE — Phase 1 freeze (see prod.md)
// Any edit requires explicit two-person CCB review + 4 h
// mainnet shadow run on engine_shadow before merge.
// Authoritative path list: scripts/check-live-safety-freeze.sh
// ============================================================

#include "providers/binance/binance_rest_client.h"
#include "providers/recovery_payload.h"
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
// The first heartbeat failure is terminal for the process. It is latched and
// delivered to the engine halt callback even if the callback is registered
// after the failure. Flattening remains owned by the exact-once kill session.
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
    using failure_fn = std::function<void(std::string_view reason)>;

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
        const auto restart_refused = [this] {
            if (!failure_latched_.load(std::memory_order_acquire)) return false;
            std::cerr << "BinanceFuturesDeadMansSwitch: terminal heartbeat "
                         "failure is latched; refusing to restart.\n";
            return true;
        };
        if (restart_refused()) return false;
        if (running_.load(std::memory_order_acquire)) return true;
        if (restart_refused()) return false;

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
        request_stop();
        if (thread_.joinable()) thread_.join();
    }

    void request_stop() noexcept
    {
        running_.store(false, std::memory_order_release);
        cv_.notify_all();
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

    void set_failure_callback(failure_fn fn)
    {
        failure_fn deliver;
        {
            std::lock_guard<std::mutex> lk(failure_mu_);
            failure_cb_ = std::move(fn);
            if (failure_latched_.load(std::memory_order_acquire)
                && !failure_delivered_ && failure_cb_)
            {
                failure_delivered_ = true;
                deliver = failure_cb_;
            }
        }
        if (deliver) deliver("binance futures DMS heartbeat failed");
    }

    bool failure_latched() const
    {
        return failure_latched_.load(std::memory_order_acquire);
    }

private:
    bool post_countdown(int64_t ms)
    {
        std::string params;
        params.reserve(symbol_.size() + 32);
        binance::append_param(params, "symbol", symbol_);
        binance::append_param(params, "countdownTime", std::to_string(ms));

        const auto resp = post_("/fapi/v1/countdownCancelAll", params);
        if (resp.status < 200 || resp.status >= 300
            || !provider_recovery::is_authoritative_object(resp.body)
            || !provider_recovery::top_level_exact_string(
                resp.body, "symbol", symbol_))
            return false;
        std::string_view returned_countdown;
        return provider_recovery::top_level_scalar_text(
                   resp.body, "countdownTime", returned_countdown)
            && returned_countdown == std::to_string(ms);
    }

    void latch_failure() noexcept
    {
        if (failure_latched_.exchange(true, std::memory_order_acq_rel)) return;
        failure_fn cb;
        try
        {
            std::lock_guard<std::mutex> lk(failure_mu_);
            if (!failure_delivered_ && failure_cb_)
            {
                cb = failure_cb_;
                failure_delivered_ = true;
            }
        }
        catch (...) { return; }
        try { if (cb) cb("binance futures DMS heartbeat failed"); }
        catch (...) {}
    }

    void run()
    {
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

            bool heartbeat_ok = false;
            try { heartbeat_ok = post_countdown(countdown_ms_); }
            catch (...) { heartbeat_ok = false; }
            if (heartbeat_ok)
            {
                last_beat_ms_.store(WorkerWatchdog::now_monotonic_ms(),
                                    std::memory_order_release);
                continue;
            }

            std::cerr << "BinanceFuturesDeadMansSwitch: heartbeat failed "
                         "once for symbol=" << symbol_
                      << "; latching terminal halt.\n";

            latch_failure();

            // Don't update last_beat_ms_ — watchdog reads it, sees the
            // stale value, halts when deadline elapses.
            running_.store(false, std::memory_order_release);
        }
    }

    post_fn post_;
    std::string symbol_;
    int64_t countdown_ms_;
    int64_t heartbeat_interval_ms_;

    std::atomic<bool> failure_latched_{false};
    std::mutex failure_mu_;
    failure_fn failure_cb_{};
    bool failure_delivered_ = false;

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
        auto r = client->safety_post(
            std::string(ep), std::string(p), std::chrono::milliseconds(1500));
        return {r.status, r.body};
    };
    return std::make_shared<BinanceFuturesDeadMansSwitch>(
        std::move(post), std::move(symbol),
        countdown_ms, heartbeat_interval_ms);
}

#endif // HAS_BINANCE
