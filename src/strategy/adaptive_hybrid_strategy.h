#pragma once

/*
 * AdaptiveHybridStrategy — Core Plugin for TrueTest HFT Engine
 * =============================================================
 *
 * Primary Mode     : Predictive Order-Book Imbalance Market Making
 * Trigger Mode     : Micro-Momentum Scalping on confirmed On-Chain Volume Spike
 * Defensive Mode   : Automatic pause / reduced quoting on low liquidity, manipulation,
 *                    or E2E latency > 20 ms (Frankfurt colocation target)
 *
 * Inventory Limit  : ±4 % (or tighter 2.5 % in small_cap_mode) per coin notional/equity
 * Position Sizing  : 0.35 % (maker) – 0.65 % (taker on spike) of current equity
 * Dynamic Spread   : 0.12 % – 0.45 % mapped from |imbalance_ewma|
 *
 * EXACT EVENT FLOW (must be 100 % deterministic & race-condition-free):
 *  1. Tick / L2-Update arrives via WebSocket (engine hot path, single writer thread)
 *  2. Atomic update of LocalL2Book (or ImbalanceEngine internal vectors) — engine thread only
 *  3. LatencyMonitor samples E2E (recv_ns → now) ; if > 20 ms → immediate Defensive flag (release-store)
 *  4. ImbalanceEngine recomputes ImbalanceScore (top-K volume) + MicroPrice (BBO weighted)
 *  5. OnChainMonitor (parallel thread) pushes SpikeEvent via SPSC RingBuffer<OnChainSignal>
 *     (main thread drains with try_pop in same on_l2_update / decision window)
 *  6. RiskValidator runs the 5 checks **in this exact order** (all must pass):
 *        a. Global Risk Limit (total exposure, drawdown, open orders)
 *        b. Per-Coin Inventory Limit (current + proposed <= ±4 %)
 *        c. Slippage Estimation (L2 depth walk for proposed notional → impact_bps)
 *        d. Manipulation Flags (cancel-to-trade window, spoof/layering detection via L2 churn)
 *        e. Latency Check (monitor->is_defensive())
 *  7. TradeValidation green-light ONLY if (6) == RejectionReason::None
 *  8. Execution: construct order_event (limit, dynamic spread around micro, sized) and return it
 *     (engine will route_order → risk_manager_ → adapter->submit)
 *  9. Post-execution: on_fill updates atomic inventory + reconciliation; lock-free log/metric push
 *
 * THREAD-SAFETY GUARANTEES (HFT 15y standard):
 *  - All hot-path state (L2 book, imbalance EWMA, inventory, mode) written **only** by the
 *    engine event-loop thread (single writer). on_l2_update / on_tick / on_market / on_fill
 *    are serialized by construction.
 *  - Cross-thread paths (OnChain producer thread → strategy) use the project's proven
 *    SPSC RingBuffer (src/threading/ring_buffer.h) with explicit acquire/release.
 *    OnChainSignal is POD; push uses release, pop uses acquire.
 *  - All monitoring stats (LatencyHistogram, RejectionCounter) are std::atomic<uint64_t>
 *    with relaxed stores (counters) or release/acquire for flags.
 *  - Defensive flag: std::atomic<bool> with release on set, acquire on test (prevents
 *    reordering that could allow an order after the 20 ms trigger).
 *  - No mutex, no lock_guard, no allocation, no shared_ptr in the 20 ms decision path.
 *  - Inventory is updated exclusively in on_fill (exact match to engine::portfolio_).
 *  - Memory orders documented at every atomic site.
 *
 * INTEGRATION NOTES:
 *  - Derives IStrategy; auto-registers as "adaptive-hybrid".
 *  - Uses on_l2_update (primary signal path). Requires engine dispatch wiring (see
 *    separate patch below) or direct calls in test harness / simulated OB replay.
 *  - Config via JSON (isolated loader) + --param overrides (existing machinery).
 *  - Exposes rich indicators (imbalance, mode, p99_latency, reject counts) for TUI/analytics.
 *  - Composes with (does not replace) engine RiskManager / halt_flag_ / venue IRiskCheck.
 *
 * COMPILATION: Add both .cpp to ENGINE_CORE_SOURCES. Only adaptive_hybrid_config.cpp
 * contains nlohmann (ctor time only — hot-path checker whitelisted).
 */

#include "strategy_interface.h"
#include "adaptive_hybrid_config.h"
#include "../threading/ring_buffer.h"
#include "../core/event.h"

#include <atomic>
#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include <algorithm>
#include <cmath>
#include <thread>
#include <mutex>          // only for OnChain thread lifecycle (not hot path)
#include <random>         // for mt19937_64 rng_

// Classes live in global namespace to match all other strategies in the tree
// (breakout_strategy, mean_reversion_strategy, etc.). IStrategy is also global.

// ============================================================================
// Supporting PODs & Enums (lock-free friendly)
// ============================================================================

enum class Mode : uint8_t {
    MAKER_IMBALANCE = 0,   // primary predictive MM
    SCALPER_MOMENTUM = 1,  // on-chain spike + micro-momentum
    DEFENSIVE = 2          // paused / micro size / wide spread
};

enum class RejectionReason : uint8_t {
    NONE = 0,
    GLOBAL_RISK,
    PER_COIN_INVENTORY,
    EXCESSIVE_SLIPPAGE,
    MANIPULATION_DETECTED,
    LATENCY_VIOLATION,
    COUNT = 6
};

struct OnChainSignal {
    uint64_t seq{0};
    uint64_t ts_ns{0};
    double   z_score{0.0};
    double   volume_delta{0.0};
    char     symbol[16]{};
    bool     is_spike{false};
};

// SPSC ring for OnChain thread → engine thread (DropOldest safe for signals)
using OnChainRing = RingBuffer<OnChainSignal, 65536, DropOldest>;

// Lightweight L2 view (top-K) maintained by ImbalanceEngine
struct L2Level { double price; int64_t qty; };

struct L2Snapshot {
    std::vector<L2Level> bids; // desc
    std::vector<L2Level> asks; // asc
    uint64_t             seq{0};
};

// ============================================================================
// ImbalanceEngine — Order-Book Microstructure Core (single-writer, engine thread)
// ============================================================================
class ImbalanceEngine {
public:
    ImbalanceEngine() : ImbalanceEngine(AdaptiveHybridConfig{}) {}
    explicit ImbalanceEngine(const AdaptiveHybridConfig& cfg);

    void on_l2_snapshot(const std::vector<l2_level>& bids,
                        const std::vector<l2_level>& asks,
                        std::chrono::system_clock::time_point ts = {});

    void on_l2_update(tick_side side, double price, int64_t new_qty,
                      std::chrono::system_clock::time_point ts = {});

    // Fast getters (called after update in hot path)
    double imbalance_score() const;      // raw or EWMA
    double micro_price() const;
    double dynamic_spread_pct() const;   // [spread_min, spread_max] mapped from |imb|
    double liquidity_depth_bps(double bps_from_mid) const; // notional within X bps
    bool   is_thin_book() const;
    double imb_momentum() const;         // short-term delta for scalping trigger
    Mode   recommended_mode(bool onchain_spike_active) const;

    void reset();

    // For indicators / debug
    std::vector<std::pair<std::string, double>> get_values() const;

private:
    void rebuild(const std::vector<L2Level>& bids, const std::vector<L2Level>& asks);
    void apply_delta(tick_side side, double price, int64_t qty);
    void recompute();

    AdaptiveHybridConfig cfg_;
    std::vector<L2Level> bids_;   // descending price
    std::vector<L2Level> asks_;   // ascending price
    double imb_ewma_ = 0.0;
    double last_raw_imb_ = 0.0;
    double micro_ = 0.0;
    double last_mid_ = 0.0;
    uint64_t update_seq_ = 0;
    std::chrono::system_clock::time_point last_ts_{};
};

// ============================================================================
// RiskValidator — Exact 5-Check Sequential Gate (post-signal, pre-order_event)
// ============================================================================
class RiskValidator {
public:
    explicit RiskValidator(const AdaptiveHybridConfig& cfg);

    // Returns NONE only if ALL 5 checks pass in order.
    // Writes estimated_impact_bps for logging.
    RejectionReason check_order(
        const std::string& symbol,
        double proposed_notional,
        double current_inventory_pct,   // already signed (+ long / - short)
        const L2Snapshot& l2,           // current top-K view
        double p99_latency_ms,
        double recent_cancel_to_trade,
        bool   onchain_spike_ok,
        double& out_estimated_impact_bps
    ) const;

    // Called by strategy on its own emits / observed L2 churn (for manip detection)
    void record_cancel(const std::string& sym);
    void record_fill(const std::string& sym);
    void on_l2_level_churn(const std::string& sym, double price, int64_t prev_qty, int64_t new_qty);

private:
    bool check_global(double proposed_notional, double current_inventory_pct) const;
    bool check_per_coin(double current_pct, double proposed_notional) const;
    bool check_slippage(const L2Snapshot& l2, double notional, double& impact_bps) const;
    bool check_manipulation(const std::string& sym) const;
    bool check_latency(double p99_ms) const;

    AdaptiveHybridConfig cfg_;
    // Simple per-symbol windows (engine-thread only, deques pruned)
    std::unordered_map<std::string, std::deque<std::chrono::steady_clock::time_point>> cancel_windows_;
    std::unordered_map<std::string, std::deque<std::chrono::steady_clock::time_point>> fill_windows_;
    std::unordered_map<std::string, int> level_churn_count_;
};

// ============================================================================
// OnChainMonitor — Parallel Volume-Spike Detector (own thread + lock-free ring)
// ============================================================================
class OnChainMonitor {
public:
    explicit OnChainMonitor(const AdaptiveHybridConfig& cfg, OnChainRing& ring);
    ~OnChainMonitor();

    // Production: start real TRON/Helius listener thread here.
    // v1: deterministic mock injector for paper / unit tests.
    void start();
    void stop();

    // Test / harness hook — injects a spike that will be visible on next drain
    void inject_spike(const std::string& symbol, double z_score, double vol_delta);

    // Called from hot path (engine thread) — non-blocking drain
    bool has_recent_spike(const std::string& symbol, uint64_t max_age_ns = 30'000'000'000ULL) const;

private:
    void thread_main();                 // producer loop (mock or real)

    AdaptiveHybridConfig cfg_;
    OnChainRing& ring_;
    std::atomic<bool> running_{false};
    std::thread worker_;
    std::mutex state_mu_;               // only for mock state (not hot path)
    uint64_t seq_{0};

    // Per-symbol EWMA state (producer side)
    struct SpikeState {
        double ewma = 0.0;
        double var_sum = 0.0;
        int count = 0;
        uint64_t last_ts = 0;
    };
    std::unordered_map<std::string, SpikeState> states_;
};

// ============================================================================
// Lightweight lock-free monitoring (strategy owned)
// ============================================================================
class LatencyHistogram {
public:
    void record_ns(int64_t ns);
    struct Snapshot {
        int64_t p50, p99, p999, max_ns;
        uint64_t count;
    };
    Snapshot snapshot() const;

private:
    // 24 bins: 0-50µs, 50-200µs, ..., >100 ms (log spaced around 20 ms)
    static constexpr int kBins = 24;
    std::array<std::atomic<uint64_t>, kBins> bins_{};
    std::atomic<int64_t> max_ns_{0};
    std::atomic<uint64_t> count_{0};
};

class RejectionCounter {
public:
    void inc(RejectionReason r);
    uint64_t get(RejectionReason r) const;
    uint64_t total() const;

private:
    std::array<std::atomic<uint64_t>, static_cast<size_t>(RejectionReason::COUNT)> cnt_{};
};

// ============================================================================
// AdaptiveHybridStrategy — The Plugin (IStrategy implementation)
// ============================================================================
class AdaptiveHybridStrategy : public IStrategy {
public:
    explicit AdaptiveHybridStrategy(AdaptiveHybridConfig cfg = {});
    ~AdaptiveHybridStrategy() override;

    // IStrategy overrides
    std::optional<order_event> on_market(const market_event& mkt) override;
    std::optional<order_event> on_tick(const tick_event& te) override;
    std::optional<order_event> on_l2_update(const l2_update_event& ev) override;

    void on_fill(const fill_event& fill, std::uint64_t opener_order_id) override;
    void set_position_open(const std::string& symbol, bool open) override;

    std::vector<param_def> get_param_schema() const override;
    void set_param(const std::string& key, double value) override;

    std::vector<std::pair<std::string, double>> get_indicator_values(
        const std::string& symbol) const override;

    // Test / harness visibility (not part of IStrategy)
    Mode current_mode(const std::string& sym) const;
    double current_inventory_pct(const std::string& sym) const;
    uint64_t rejection_count(RejectionReason r) const;
    void inject_onchain_spike(const std::string& sym, double z, double vol);

private:
    // Core 9-step flow entry (called from on_l2_update primarily)
    std::optional<order_event> process_l2_update(const l2_update_event& ev);

    // Helpers (all engine-thread)
    double compute_equity_proxy() const;   // last_mid * inventory + cash proxy
    double compute_proposed_notional(const std::string& sym, double price, bool is_taker) const;
    bool   decide_and_validate(const std::string& sym,
                               double imb,
                               bool onchain_spike,
                               double price,
                               order_side side,
                               double& out_qty,
                               double& out_limit_price,
                               RejectionReason& out_rej);

    // State (single-writer — engine thread only)
    AdaptiveHybridConfig cfg_;
    std::unordered_map<std::string, ImbalanceEngine> imb_engines_;
    std::unordered_map<std::string, double> inventory_pct_;   // signed, updated only on_fill
    std::unordered_map<std::string, Mode> modes_;
    std::unordered_map<std::string, uint64_t> last_update_seq_;

    // Cross-thread OnChain
    OnChainRing onchain_ring_;
    std::unique_ptr<OnChainMonitor> onchain_monitor_;

    // Monitoring (lock-free)
    LatencyHistogram latency_hist_;
    RejectionCounter rejection_cnt_;

    // Simple last-known mids for sizing
    std::unordered_map<std::string, double> last_mids_;

    // RNG for jitter (seeded)
    mutable std::mt19937_64 rng_;

    // Defensive flag (visible across any future threads)
    std::atomic<bool> defensive_mode_{false};
};

// end of global namespace (no wrapper to match project style)
