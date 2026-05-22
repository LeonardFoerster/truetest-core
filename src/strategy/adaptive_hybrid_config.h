#pragma once

#include <string>
#include <cstdint>
#include <limits>

// global namespace (project convention for strategies)

// Configuration for Adaptive Hybrid HFT Strategy (20ms Frankfurt colocation window,
// small-cap coins e.g. TRX and lower liquidity names).
// All values have explicit bounds and defaults tuned for ±4% inventory,
// 0.35-0.65% sizing, 0.12-0.45% dynamic spread, 3.5z on-chain spikes.
struct AdaptiveHybridConfig {
    // Imbalance thresholds (predictive OB MM primary mode)
    double imbalance_long_threshold  = 0.22;   // |imb_ewma| > this → lean long bias in quotes
    double imbalance_short_threshold = -0.22;
    double imbalance_defensive_abs   = 0.65;   // |imb| > this → force Defensive mode (thin book risk)

    // Dynamic spread mapping (0.12% - 0.45%)
    double spread_min_pct = 0.0012;
    double spread_max_pct = 0.0045;

    // Position sizing (% of current equity per trade)
    double maker_size_frac = 0.0035; // 0.35%
    double taker_size_frac = 0.0065; // 0.65% (used on confirmed on-chain spike + momentum)

    // Inventory hard limits (per-coin notional / equity)
    double inventory_max_pct = 0.04; // ±4%

    // On-chain volume spike detector (parallel thread, lock-free ring)
    double spike_z_threshold   = 3.5;   // z-score for "strong" spike → scalping trigger
    double spike_lookback_s    = 15.0;
    double spike_ewma_alpha    = 0.25;
    bool   require_positive_imbalance_for_scalp = true;

    // Latency defensive (end-to-end decision window)
    double max_latency_ms = 20.0;       // >20ms → immediate Defensive (no new quotes)

    // Risk / slippage / manipulation gates (exact sequential order per spec)
    double max_global_exposure_pct = 0.18;
    double max_impact_bps          = 6.0;   // estimated market impact for proposed size
    double max_cancel_to_trade     = 0.60;  // own + observed proxy
    int    max_open_orders         = 40;

    // Small-cap specifics (tighter for TRON-class names)
    bool   small_cap_mode          = true;
    double small_cap_inventory_pct = 0.025; // tighter 2.5%

    // EWMA / momentum for predictive aspect
    double imb_ewma_alpha          = 0.28;
    int    top_k_levels            = 8;     // for imbalance / liquidity calc (matches depth20)

    // Misc
    uint64_t rng_seed = 0xC0FFEE42;
    bool     enable_onchain_mock  = true;   // for paper/backtest; production wires real feed
};

// Loads from JSON if path non-empty (isolated loader, never hot-path).
// Returns defaults + overrides on success; always valid struct.
AdaptiveHybridConfig load_adaptive_hybrid_config(const std::string& json_path = "");

// end global
