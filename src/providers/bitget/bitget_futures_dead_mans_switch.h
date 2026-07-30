#pragma once
#ifdef HAS_BITGET

// Bitget UTA v3 dead-man's switch (Phase 3 safety).
// Wraps POST /api/v3/trade/countdown-cancel-all with body {"countdown":"N"}
// where N is seconds in [5, 60] (or "0" to disarm).
//
// CRITICAL CAVEAT — account-wide cancel:
// Unlike Binance futures countdownCancelAll (symbol-scoped), Bitget UTA
// countdown-cancel-all cancels ALL open UTA orders for the account when
// the timer expires — not only the provider symbol. v1 assumes a single-
// symbol process; multi-strategy operators must treat this as process-wide.
//
// Rate limit: 1/s/UID → heartbeat_interval_ms clamped to ≥ 1000ms.
// BD enablement: venue may require Bitget BD to unlock this endpoint;
// permission-style failures log a loud enablement note.
//
// Layered with kill-switch:
//   - Orderly shutdown: kill-switch cancel+flatten; DMS disarm(countdown=0).
//   - Catastrophic death: venue cancels orders when countdown expires.
//   - Positions stay open after auto-cancel (orders only) unless
//     dms_attempt_position_close fires close-positions on 2 consecutive
//     heartbeat failures while this thread is still alive.
// Routes through post_fn (tests inject fake; production wraps post_json).

#include "providers/bitget/bitget_rest_client.h"
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

class BitgetFuturesDeadMansSwitch
{
public:
    struct response
    {
        int status = 0;
        std::string body;
    };

    using post_fn = std::function<response(std::string_view endpoint,
                                           std::string_view json_body)>;
    using close_position_fn = std::function<void()>;

    // countdown_ms: operator config in ms. Converted to seconds and clamped
    // to [5, 60]. Values in (0, 5000) WARN+clamp to 5s; > 60000 WARN+clamp
    // to 60s. heartbeat_interval_ms: ≥ 1000 (1/s rate limit) and must stay
    // strictly below clamped countdown so heartbeats refresh before the
    // venue timer expires. Prefer countdown/3 when operator leaves HB
    // unset (0); oversized HB (e.g. raw Binance-style ms/3 after clamp)
    // is WARN-reduced to max(1000, countdown_ms_/3).
    BitgetFuturesDeadMansSwitch(post_fn post,
                                int64_t countdown_ms,
                                int64_t heartbeat_interval_ms,
                                bool attempt_close = false,
                                close_position_fn closer = nullptr)
        : post_(std::move(post))
        , attempt_position_close_on_failure_(attempt_close)
        , close_position_fn_(std::move(closer))
    {
        countdown_sec_ = clamp_countdown_sec(countdown_ms);
        countdown_ms_ = countdown_sec_ * 1000;

        int64_t hb = heartbeat_interval_ms;
        if (hb <= 0)
            hb = countdown_ms_ / 3;
        if (hb < 1000)
            hb = 1000;
        // After countdown clamp, HB must not meet/exceed the venue timer
        // (e.g. countdown_ms=200000 → 60s, raw hb=66666 would never refresh).
        if (hb >= countdown_ms_)
        {
            const int64_t fixed = std::max<int64_t>(1000, countdown_ms_ / 3);
            std::cerr << "BitgetFuturesDeadMansSwitch: WARNING — "
                         "heartbeat_interval_ms=" << hb
                      << " >= clamped countdown " << countdown_ms_
                      << "ms; reducing to " << fixed << "ms\n";
            hb = fixed;
        }
        heartbeat_interval_ms_ = hb;

        // Loud one-shot caveat for operators (account-wide cancel).
        std::cerr << "BitgetFuturesDeadMansSwitch: CAUTION — UTA "
                     "countdown-cancel-all is ACCOUNT-WIDE (all open UTA "
                     "orders), not symbol-scoped. countdown="
                  << countdown_sec_ << "s heartbeat="
                  << heartbeat_interval_ms_ << "ms\n";
    }

    ~BitgetFuturesDeadMansSwitch() { stop(); }

    BitgetFuturesDeadMansSwitch(const BitgetFuturesDeadMansSwitch&) = delete;
    BitgetFuturesDeadMansSwitch& operator=(const BitgetFuturesDeadMansSwitch&) = delete;
    BitgetFuturesDeadMansSwitch(BitgetFuturesDeadMansSwitch&&) = delete;
    BitgetFuturesDeadMansSwitch& operator=(BitgetFuturesDeadMansSwitch&&) = delete;

    // Arms server-side countdown and spawns heartbeat. Returns false if
    // the initial arm fails — refuse to go live without protection.
    bool start()
    {
        if (running_.load(std::memory_order_acquire)) return true;

        if (!post_countdown(countdown_sec_))
        {
            std::cerr << "BitgetFuturesDeadMansSwitch: initial arm "
                         "(POST /api/v3/trade/countdown-cancel-all, "
                         "countdown=" << countdown_sec_
                      << "s) failed — refusing to start.\n";
            log_bd_enablement_hint_if_needed(last_body_);
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

    // POST countdown=0. Idempotent on success.
    bool disarm()
    {
        return post_countdown(/*sec=*/0);
    }

    std::atomic<int64_t>& liveness_ts() { return last_beat_ms_; }

    int64_t heartbeat_interval_ms() const { return heartbeat_interval_ms_; }
    int64_t countdown_ms() const { return countdown_ms_; }
    int64_t countdown_sec() const { return countdown_sec_; }

    void set_attempt_position_close(bool v)
    {
        attempt_position_close_on_failure_ = v;
    }
    void set_close_position_fn(close_position_fn fn)
    {
        close_position_fn_ = std::move(fn);
    }

    // Pure helper for tests / provider: convert operator ms → seconds [5,60].
    // Logs WARN when bumping sub-5s or capping above 60s.
    static int64_t clamp_countdown_sec(int64_t countdown_ms)
    {
        if (countdown_ms <= 0)
            return 5; // defensive; provider only constructs when > 0

        if (countdown_ms > 0 && countdown_ms < 5000)
        {
            std::cerr << "BitgetFuturesDeadMansSwitch: WARNING — "
                         "dead_man_countdown_ms=" << countdown_ms
                      << " is below venue minimum 5000ms; clamping to 5s\n";
            return 5;
        }

        int64_t sec = countdown_ms / 1000;
        if (sec < 5) sec = 5;
        if (sec > 60)
        {
            std::cerr << "BitgetFuturesDeadMansSwitch: WARNING — "
                         "dead_man_countdown_ms=" << countdown_ms
                      << " exceeds venue maximum 60000ms; clamping to 60s\n";
            return 60;
        }
        return sec;
    }

private:
    bool post_countdown(int64_t sec)
    {
        // Body is exact JSON string; countdown is a string per Bitget docs.
        std::string body = "{\"countdown\":\"";
        body.append(std::to_string(sec));
        body.append("\"}");

        const auto resp = post_("/api/v3/trade/countdown-cancel-all", body);
        last_body_ = resp.body;

        const bool ok = resp.status >= 200 && resp.status < 300
                     && bitget::is_business_success(resp.status, resp.body);
        if (!ok)
            log_bd_enablement_hint_if_needed(resp.body);
        return ok;
    }

    static void log_bd_enablement_hint_if_needed(std::string_view body)
    {
        // Permission / enablement failures: surface BD unlock note once-ish.
        // Match common permission strings and auth-ish business codes.
        const bool looks_perm =
            body.find("permission") != std::string_view::npos
            || body.find("Permission") != std::string_view::npos
            || body.find("not enabled") != std::string_view::npos
            || body.find("not support") != std::string_view::npos
            || body.find("not allowed") != std::string_view::npos
            || body.find("40014") != std::string_view::npos
            || body.find("40018") != std::string_view::npos
            || body.find("403") != std::string_view::npos;
        if (!looks_perm) return;
        std::cerr << "BitgetFuturesDeadMansSwitch: DMS may require Bitget BD "
                     "enablement if venue returns a permission error. Contact "
                     "Bitget to unlock POST /api/v3/trade/countdown-cancel-all.\n";
    }

    void run()
    {
        // No mid-cycle retry: rate limit is 1/s. Count consecutive cycle
        // failures; on the second, optional close-positions then go silent
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

            if (post_countdown(countdown_sec_))
            {
                last_beat_ms_.store(WorkerWatchdog::now_monotonic_ms(),
                                    std::memory_order_release);
                consecutive_fails_ = 0;
                continue;
            }

            ++consecutive_fails_;
            std::cerr << "BitgetFuturesDeadMansSwitch: heartbeat failed "
                         "(consecutive=" << consecutive_fails_
                      << "); watchdog will halt engine if persistent.\n";

            if (consecutive_fails_ >= 2
                && attempt_position_close_on_failure_
                && close_position_fn_
                && !close_attempted_)
            {
                close_attempted_ = true;
                std::cerr << "BitgetFuturesDeadMansSwitch: 2 consecutive HB "
                             "fails — invoking close-positions last resort\n";
                close_position_fn_();
            }
            // Don't update last_beat_ms_ on failure.
        }
    }

    post_fn post_;
    int64_t countdown_sec_ = 5;
    int64_t countdown_ms_ = 5000;
    int64_t heartbeat_interval_ms_ = 1000;

    bool attempt_position_close_on_failure_ = false;
    close_position_fn close_position_fn_{};
    bool close_attempted_ = false;
    int consecutive_fails_ = 0;
    std::string last_body_;

    std::atomic<int64_t> last_beat_ms_{0};
    std::atomic<bool>    running_{false};
    std::thread          thread_;
    std::mutex           cv_mu_;
    std::condition_variable cv_;
};

inline std::shared_ptr<BitgetFuturesDeadMansSwitch>
make_bitget_futures_dead_mans_switch(
    std::shared_ptr<BitgetRestClient> client,
    int64_t countdown_ms,
    int64_t heartbeat_interval_ms,
    bool attempt_close = false,
    BitgetFuturesDeadMansSwitch::close_position_fn closer = nullptr)
{
    BitgetFuturesDeadMansSwitch::post_fn post;
    if (client)
    {
        post = [client](std::string_view ep, std::string_view body)
            -> BitgetFuturesDeadMansSwitch::response
        {
            auto r = client->post_json(std::string(ep), std::string(body));
            return {r.status, std::move(r.body)};
        };
    }
    return std::make_shared<BitgetFuturesDeadMansSwitch>(
        std::move(post), countdown_ms, heartbeat_interval_ms,
        attempt_close, std::move(closer));
}

#endif // HAS_BITGET
