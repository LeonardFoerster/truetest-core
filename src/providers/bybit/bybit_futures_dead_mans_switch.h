#pragma once
#ifdef HAS_BYBIT

// Bybit V5 linear dead-man's switch (Phase 3 safety).
//
// Retail / testnet default: **process-local** DMS only.
// Bybit DCP (POST /v5/order/disconnected-cancel-all) is institutional-only
// and is NOT silently pretended. When DCP is unavailable we:
//   - log a loud caveat (orders NOT auto-cancelled on SIGKILL)
//   - run a heartbeat thread that health-checks the venue
//   - expose last_beat_ms_ to WorkerWatchdog
//   - on 2 consecutive HB fails + dms_attempt_position_close: optional
//     reduceOnly MARKET flatten via close_position_fn
//
// Optional DCP path (when dcp_arm_fn is provided and succeeds):
//   - arm once at start with venue time window
//   - disarm at orderly close
//   - still run local heartbeat for liveness
//
// Routes through health_fn / optional dcp_arm_fn so tests inject fakes.
// Production: make_bybit_futures_dead_mans_switch(rest, ...).

#include "providers/bybit/bybit_endpoints.h"
#include "providers/bybit/bybit_rest_client.h"
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

class BybitFuturesDeadMansSwitch
{
public:
    struct response
    {
        int status = 0;
        std::string body;
        bool business_ok = false;
    };

    // Health probe used as heartbeat (unsigned market/time or signed wallet).
    using health_fn = std::function<response()>;
    // Optional institutional DCP arm/disarm. arm(secs) / disarm(0).
    // Return true on success. Nullptr → local-only mode.
    using dcp_fn = std::function<bool(int64_t time_window_sec)>;
    using close_position_fn = std::function<void()>;

    // countdown_ms: operator config in ms — used as:
    //   - DCP timeWindow when dcp_arm is set (clamped to [3, 300] s)
    //   - local "expected protection window" for logging / HB default
    // heartbeat_interval_ms: ≥ 1000; default countdown/3 when 0; must stay
    // strictly below countdown so watchdog can fire before a stale HB is
    // tolerated forever. Prefer countdown/3 when operator leaves HB unset.
    BybitFuturesDeadMansSwitch(health_fn health,
                               int64_t countdown_ms,
                               int64_t heartbeat_interval_ms,
                               bool attempt_close = false,
                               close_position_fn closer = nullptr,
                               dcp_fn dcp_arm = nullptr)
        : health_(std::move(health))
        , attempt_position_close_on_failure_(attempt_close)
        , close_position_fn_(std::move(closer))
        , dcp_arm_(std::move(dcp_arm))
    {
        countdown_sec_ = clamp_countdown_sec(countdown_ms);
        countdown_ms_ = countdown_sec_ * 1000;

        int64_t hb = heartbeat_interval_ms;
        if (hb <= 0)
            hb = countdown_ms_ / 3;
        if (hb < 1000)
            hb = 1000;
        if (hb >= countdown_ms_)
        {
            const int64_t fixed = std::max<int64_t>(1000, countdown_ms_ / 3);
            std::cerr << "BybitFuturesDeadMansSwitch: WARNING — "
                         "heartbeat_interval_ms=" << hb
                      << " >= clamped countdown " << countdown_ms_
                      << "ms; reducing to " << fixed << "ms\n";
            hb = fixed;
        }
        heartbeat_interval_ms_ = hb;

        if (!dcp_arm_)
        {
            std::cerr << "BybitFuturesDeadMansSwitch: CAUTION — DCP "
                         "(disconnected-cancel-all) is institutional-only; "
                         "running LOCAL DMS only. Open orders are NOT "
                         "auto-cancelled on process death (SIGKILL/OOM). "
                         "countdown=" << countdown_sec_
                      << "s heartbeat=" << heartbeat_interval_ms_ << "ms\n";
        }
        else
        {
            std::cerr << "BybitFuturesDeadMansSwitch: DCP arm callback "
                         "configured; countdown=" << countdown_sec_
                      << "s heartbeat=" << heartbeat_interval_ms_ << "ms\n";
        }
    }

    ~BybitFuturesDeadMansSwitch() { stop(); }

    BybitFuturesDeadMansSwitch(const BybitFuturesDeadMansSwitch&) = delete;
    BybitFuturesDeadMansSwitch& operator=(const BybitFuturesDeadMansSwitch&) = delete;
    BybitFuturesDeadMansSwitch(BybitFuturesDeadMansSwitch&&) = delete;
    BybitFuturesDeadMansSwitch& operator=(BybitFuturesDeadMansSwitch&&) = delete;

    // Arms optional DCP, verifies first health probe, spawns heartbeat.
    // Returns false if DCP arm fails (when configured) OR initial health
    // probe fails — refuse to go live without protection signal.
    bool start()
    {
        if (running_.load(std::memory_order_acquire)) return true;

        if (dcp_arm_)
        {
            if (!dcp_arm_(countdown_sec_))
            {
                std::cerr << "BybitFuturesDeadMansSwitch: DCP initial arm "
                             "failed — refusing to start.\n";
                return false;
            }
            dcp_armed_ = true;
        }

        if (!probe_health())
        {
            std::cerr << "BybitFuturesDeadMansSwitch: initial health probe "
                         "failed — refusing to start.\n";
            // Best-effort DCP disarm if we just armed.
            if (dcp_armed_ && dcp_arm_)
                (void)dcp_arm_(/*sec=*/0);
            dcp_armed_ = false;
            return false;
        }

        last_beat_ms_.store(WorkerWatchdog::now_monotonic_ms(),
                            std::memory_order_release);
        consecutive_fails_ = 0;
        running_.store(true, std::memory_order_release);
        thread_ = std::thread([this] { run(); });
        return true;
    }

    // Stops heartbeat thread. Does NOT disarm DCP — caller must call
    // disarm() for orderly cleanup. Uncontrolled death leaves DCP timer
    // running (if armed); local-only mode has no venue timer.
    void stop()
    {
        if (!running_.exchange(false, std::memory_order_acq_rel)) return;
        cv_.notify_all();
        if (thread_.joinable()) thread_.join();
    }

    // DCP disarm (countdown/window 0). Local-only: returns true (no-op).
    bool disarm()
    {
        if (!dcp_arm_)
            return true;
        const bool ok = dcp_arm_(/*sec=*/0);
        if (ok)
            dcp_armed_ = false;
        return ok;
    }

    std::atomic<int64_t>& liveness_ts() { return last_beat_ms_; }

    int64_t heartbeat_interval_ms() const { return heartbeat_interval_ms_; }
    int64_t countdown_ms() const { return countdown_ms_; }
    int64_t countdown_sec() const { return countdown_sec_; }
    bool dcp_armed() const { return dcp_armed_; }

    void set_attempt_position_close(bool v)
    {
        attempt_position_close_on_failure_ = v;
    }
    void set_close_position_fn(close_position_fn fn)
    {
        close_position_fn_ = std::move(fn);
    }

    // Pure helper: convert operator ms → seconds.
    // Local DMS: clamp to [5, 300] (align with DCP max window for logging).
    // DCP venue range is 3–300s; we floor at 5s for conservative local HB.
    static int64_t clamp_countdown_sec(int64_t countdown_ms)
    {
        if (countdown_ms <= 0)
            return 5; // defensive; provider only constructs when > 0

        if (countdown_ms > 0 && countdown_ms < 5000)
        {
            std::cerr << "BybitFuturesDeadMansSwitch: WARNING — "
                         "dead_man_countdown_ms=" << countdown_ms
                      << " is below minimum 5000ms; clamping to 5s\n";
            return 5;
        }

        int64_t sec = countdown_ms / 1000;
        if (sec < 5) sec = 5;
        if (sec > 300)
        {
            std::cerr << "BybitFuturesDeadMansSwitch: WARNING — "
                         "dead_man_countdown_ms=" << countdown_ms
                      << " exceeds maximum 300000ms; clamping to 300s\n";
            return 300;
        }
        return sec;
    }

private:
    bool probe_health()
    {
        if (!health_) return false;
        const auto resp = health_();
        if (resp.status < 200 || resp.status >= 300)
            return false;
        // Unsigned market/time may lack retCode on some paths; accept
        // business_ok OR retCode==0 OR body containing a time field.
        if (resp.business_ok) return true;
        if (bybit::is_business_success(resp.status, resp.body)) return true;
        // Unsigned /v5/market/time often has retCode 0; if missing, accept
        // any 2xx with a non-empty body that looks like time payload.
        if (!resp.body.empty()
            && (resp.body.find("time") != std::string::npos
                || resp.body.find("retCode") != std::string::npos))
        {
            // If retCode present and non-zero, fail.
            auto code = bybit::extract_ret_code(resp.body);
            if (!code.empty() && code != "0")
                return false;
            return true;
        }
        return false;
    }

    void run()
    {
        // No mid-cycle retry storm: one probe per HB interval.
        // On second consecutive fail: optional flatten, then go silent
        // (liveness atomic stops advancing → WorkerWatchdog can halt).
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

            if (probe_health())
            {
                last_beat_ms_.store(WorkerWatchdog::now_monotonic_ms(),
                                    std::memory_order_release);
                consecutive_fails_ = 0;
                close_attempted_ = false;
                continue;
            }

            ++consecutive_fails_;
            std::cerr << "BybitFuturesDeadMansSwitch: heartbeat failed "
                         "(consecutive=" << consecutive_fails_
                      << "); watchdog will halt engine if persistent.\n";

            if (consecutive_fails_ >= 2
                && attempt_position_close_on_failure_
                && close_position_fn_
                && !close_attempted_)
            {
                close_attempted_ = true;
                std::cerr << "BybitFuturesDeadMansSwitch: 2 consecutive HB "
                             "fails — invoking reduceOnly flatten last resort\n";
                close_position_fn_();
            }
            // Don't update last_beat_ms_ on failure.
        }
    }

    health_fn health_;
    int64_t countdown_sec_ = 5;
    int64_t countdown_ms_ = 5000;
    int64_t heartbeat_interval_ms_ = 1000;

    bool attempt_position_close_on_failure_ = false;
    close_position_fn close_position_fn_{};
    dcp_fn dcp_arm_;
    bool dcp_armed_ = false;
    bool close_attempted_ = false;
    int consecutive_fails_ = 0;

    std::atomic<int64_t> last_beat_ms_{0};
    std::atomic<bool>    running_{false};
    std::thread          thread_;
    std::mutex           cv_mu_;
    std::condition_variable cv_;
};

inline std::shared_ptr<BybitFuturesDeadMansSwitch>
make_bybit_futures_dead_mans_switch(
    std::shared_ptr<BybitRestClient> client,
    int64_t countdown_ms,
    int64_t heartbeat_interval_ms,
    bool attempt_close = false,
    BybitFuturesDeadMansSwitch::close_position_fn closer = nullptr,
    bool enable_dcp = false)
{
    BybitFuturesDeadMansSwitch::health_fn health;
    BybitFuturesDeadMansSwitch::dcp_fn dcp;
    if (client)
    {
        // Unsigned market time — cheap, no TRADE rate-limit pressure.
        health = [client]() -> BybitFuturesDeadMansSwitch::response {
            auto r = client->get_unsigned(bybit::paths::market_time, "");
            return {r.status, std::move(r.body), r.business_ok};
        };
        if (enable_dcp)
        {
            // Institutional DCP: product DERIVATIVES, timeWindow in seconds.
            dcp = [client](int64_t sec) -> bool {
                std::string body = "{\"product\":\"DERIVATIVES\",\"timeWindow\":";
                body.append(std::to_string(sec));
                body.push_back('}');
                auto r = client->post_json(
                    "/v5/order/disconnected-cancel-all", body);
                return r.status >= 200 && r.status < 300
                    && (r.business_ok
                        || bybit::is_business_success(r.status, r.body));
            };
        }
    }
    return std::make_shared<BybitFuturesDeadMansSwitch>(
        std::move(health), countdown_ms, heartbeat_interval_ms,
        attempt_close, std::move(closer), std::move(dcp));
}

#endif // HAS_BYBIT
