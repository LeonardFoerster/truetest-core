#pragma once
#ifdef HAS_GATE

// Gate.io USDT-M futures dead-man's switch (Phase 3 safety).
// Wraps POST /api/v4/futures/{settle}/countdown_cancel_all with body
//   {"timeout":N,"contract":"BTC_USDT"}
// where N is seconds ≥ 5 (or 0 to disarm). Contract is always set in v1.
//
// Gate timeout unit is SECONDS (not ms). Operator config stays in ms for
// CLI parity with Binance: timeout_s = max(5, countdown_ms / 1000).
// NEVER send raw ms as timeout — 30000 would be 30000 seconds.
//
// Layered with kill-switch:
//   - Orderly shutdown: kill-switch cancel+flatten; DMS disarm(timeout=0).
//   - Catastrophic death: venue cancels open orders when countdown expires.
//   - Positions stay open after auto-cancel (orders only) unless
//     dms_attempt_position_close fires close-position on 2 consecutive
//     heartbeat failures while this thread is still alive.
//
// Fixed conservative timeout/heartbeat — no adaptive lengthening under load.
// Routes through post_fn (tests inject fake; production wraps post_json).

#include "providers/gate/gate_endpoints.h"
#include "providers/gate/gate_rest_client.h"
#include "threading/worker_watchdog.h"

#include <algorithm>
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

class GateFuturesDeadMansSwitch
{
public:
    struct response
    {
        int status = 0;
        std::string body;
    };

    using post_fn = std::function<response(std::string_view endpoint,
                                           std::string_view json_body)>;
    using close_position_fn = std::function<void(const std::string& symbol)>;

    // countdown_ms: operator config in ms. Converted to seconds via
    // max(5, ms/1000). Values in (0, 5000) WARN+clamp to 5s. No venue-stated
    // upper cap (unlike Bitget 60s) — keep operator value after /1000.
    // heartbeat_interval_ms: ≥ 1000 and must stay strictly below clamped
    // countdown so heartbeats refresh before the venue timer expires.
    // Prefer countdown/3 when operator leaves HB unset (0).
    GateFuturesDeadMansSwitch(post_fn post,
                              std::string symbol,
                              int64_t countdown_ms,
                              int64_t heartbeat_interval_ms,
                              bool attempt_close = false,
                              close_position_fn closer = nullptr,
                              std::string settle = "usdt")
        : post_(std::move(post))
        , symbol_(std::move(symbol))
        , settle_(std::move(settle))
        , attempt_position_close_on_failure_(attempt_close)
        , close_position_fn_(std::move(closer))
    {
        countdown_sec_ = clamp_countdown_sec(countdown_ms);
        countdown_ms_ = countdown_sec_ * 1000;

        // Full signed path including /api/v4 (HMAC signs full path).
        endpoint_path_ = "/api/v4/futures/";
        endpoint_path_.append(settle_);
        endpoint_path_.append("/countdown_cancel_all");

        int64_t hb = heartbeat_interval_ms;
        if (hb <= 0)
            hb = countdown_ms_ / 3;
        if (hb < 1000)
            hb = 1000;
        // HB must not meet/exceed the venue timer or refreshes never win.
        if (hb >= countdown_ms_)
        {
            const int64_t fixed = std::max<int64_t>(1000, countdown_ms_ / 3);
            std::cerr << "GateFuturesDeadMansSwitch: WARNING — "
                         "heartbeat_interval_ms=" << hb
                      << " >= clamped countdown " << countdown_ms_
                      << "ms; reducing to " << fixed << "ms\n";
            hb = fixed;
        }
        heartbeat_interval_ms_ = hb;
    }

    ~GateFuturesDeadMansSwitch() { stop(); }

    GateFuturesDeadMansSwitch(const GateFuturesDeadMansSwitch&) = delete;
    GateFuturesDeadMansSwitch& operator=(const GateFuturesDeadMansSwitch&) = delete;
    GateFuturesDeadMansSwitch(GateFuturesDeadMansSwitch&&) = delete;
    GateFuturesDeadMansSwitch& operator=(GateFuturesDeadMansSwitch&&) = delete;

    // Arms server-side countdown and spawns heartbeat. Returns false if
    // the initial arm fails — refuse to go live without protection.
    bool start()
    {
        if (running_.load(std::memory_order_acquire)) return true;

        if (!post_countdown(countdown_sec_))
        {
            std::cerr << "GateFuturesDeadMansSwitch: initial arm "
                         "(POST " << endpoint_path_
                      << ", contract=" << symbol_
                      << " timeout=" << countdown_sec_
                      << "s) failed — refusing to start.\n";
            return false;
        }

        last_beat_ms_.store(WorkerWatchdog::now_monotonic_ms(),
                            std::memory_order_release);
        consecutive_fails_ = 0;
        running_.store(true, std::memory_order_release);
        thread_ = std::thread([this] { run(); });
        return true;
    }

    // Stops heartbeat thread. Does NOT disarm — caller must call disarm()
    // for orderly cleanup. Uncontrolled death leaves the timer running.
    void stop()
    {
        if (!running_.exchange(false, std::memory_order_acq_rel)) return;
        cv_.notify_all();
        if (thread_.joinable()) thread_.join();
    }

    // POST timeout=0. Idempotent on success.
    bool disarm()
    {
        return post_countdown(/*sec=*/0);
    }

    std::atomic<int64_t>& liveness_ts() { return last_beat_ms_; }

    int64_t heartbeat_interval_ms() const { return heartbeat_interval_ms_; }
    int64_t countdown_ms() const { return countdown_ms_; }
    int64_t countdown_sec() const { return countdown_sec_; }
    const std::string& symbol() const { return symbol_; }
    const std::string& endpoint_path() const { return endpoint_path_; }

    void set_attempt_position_close(bool v)
    {
        attempt_position_close_on_failure_ = v;
    }
    void set_close_position_fn(close_position_fn fn)
    {
        close_position_fn_ = std::move(fn);
    }

    // Pure helper for tests / provider: convert operator ms → seconds ≥ 5.
    // Logs WARN when bumping sub-5s. No upper clamp (Gate docs: min 5 only).
    static int64_t clamp_countdown_sec(int64_t countdown_ms)
    {
        if (countdown_ms <= 0)
            return 5; // defensive; provider only constructs when > 0

        if (countdown_ms > 0 && countdown_ms < 5000)
        {
            std::cerr << "GateFuturesDeadMansSwitch: WARNING — "
                         "dead_man_countdown_ms=" << countdown_ms
                      << " is below venue minimum 5000ms; clamping to 5s\n";
            return 5;
        }

        int64_t sec = countdown_ms / 1000;
        if (sec < 5)
        {
            // e.g. 5000..5999 ms → integer division yields 5; 0 handled above.
            // Keep max(5, ms/1000) semantics explicit for partial seconds.
            std::cerr << "GateFuturesDeadMansSwitch: WARNING — "
                         "dead_man_countdown_ms=" << countdown_ms
                      << " floors below 5s; clamping to 5s\n";
            sec = 5;
        }
        return sec;
    }

private:
    bool post_countdown(int64_t sec)
    {
        // Body is exact JSON; timeout is a number (seconds), contract always set.
        // {"timeout":30,"contract":"BTC_USDT"}
        std::string body;
        body.reserve(48 + symbol_.size());
        body.append("{\"timeout\":");
        body.append(std::to_string(sec));
        body.append(",\"contract\":\"");
        body.append(symbol_);
        body.append("\"}");

        const auto resp = post_(endpoint_path_, body);
        // Gate success is HTTP 2xx (no Bitget-style business code envelope).
        return resp.status >= 200 && resp.status < 300;
    }

    void run()
    {
        // Count consecutive cycle failures; on the second, optional
        // close-position then go silent (liveness atomic stops advancing
        // → WorkerWatchdog can halt). No adaptive timeout lengthening.
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

            if (post_countdown(countdown_sec_))
            {
                last_beat_ms_.store(WorkerWatchdog::now_monotonic_ms(),
                                    std::memory_order_release);
                consecutive_fails_ = 0;
                // Recovered after a close episode — allow another last-resort
                // close if a later dual-failure streak occurs.
                close_attempted_ = false;
                continue;
            }

            ++consecutive_fails_;
            std::cerr << "GateFuturesDeadMansSwitch: heartbeat failed "
                         "(consecutive=" << consecutive_fails_
                      << ", contract=" << symbol_
                      << "); watchdog will halt engine if persistent.\n";

            if (consecutive_fails_ >= 2
                && attempt_position_close_on_failure_
                && close_position_fn_
                && !close_attempted_)
            {
                close_attempted_ = true;
                std::cerr << "GateFuturesDeadMansSwitch: 2 consecutive HB "
                             "fails — invoking close-position last resort "
                             "for contract=" << symbol_ << "\n";
                close_position_fn_(symbol_);
            }
            // Don't update last_beat_ms_ on failure.
        }
    }

    post_fn post_;
    std::string symbol_;
    std::string settle_;
    std::string endpoint_path_;
    int64_t countdown_sec_ = 5;
    int64_t countdown_ms_ = 5000;
    int64_t heartbeat_interval_ms_ = 1000;

    bool attempt_position_close_on_failure_ = false;
    close_position_fn close_position_fn_{};
    bool close_attempted_ = false;
    int consecutive_fails_ = 0;

    std::atomic<int64_t> last_beat_ms_{0};
    std::atomic<bool>    running_{false};
    std::thread          thread_;
    std::mutex           cv_mu_;
    std::condition_variable cv_;
};

inline std::shared_ptr<GateFuturesDeadMansSwitch>
make_gate_futures_dead_mans_switch(
    std::shared_ptr<GateRestClient> client,
    std::string symbol,
    int64_t countdown_ms,
    int64_t heartbeat_interval_ms,
    bool attempt_close = false,
    GateFuturesDeadMansSwitch::close_position_fn closer = nullptr,
    const gate::endpoints& ep = gate::usdt_mainnet())
{
    const std::string settle = gate::settle_str(ep);
    GateFuturesDeadMansSwitch::post_fn post;
    if (client)
    {
        post = [client](std::string_view ep_path, std::string_view body)
            -> GateFuturesDeadMansSwitch::response
        {
            auto r = client->post_json(std::string(ep_path),
                                       std::string(body));
            return {r.status, std::move(r.body)};
        };
    }
    return std::make_shared<GateFuturesDeadMansSwitch>(
        std::move(post), std::move(symbol), countdown_ms,
        heartbeat_interval_ms, attempt_close, std::move(closer),
        settle);
}

#endif // HAS_GATE
