#include "adaptive_hybrid_config.h"

#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
#include <stdexcept>

// global namespace (project convention)

AdaptiveHybridConfig load_adaptive_hybrid_config(const std::string& json_path)
{
    AdaptiveHybridConfig cfg; // all defaults

    if (json_path.empty()) {
        return cfg;
    }

    std::ifstream f(json_path);
    if (!f.is_open()) {
        std::cerr << "[AdaptiveHybrid] Warning: config file '" << json_path
                  << "' not found, using built-in defaults.\n";
        return cfg;
    }

    try {
        nlohmann::json j;
        f >> j;

        // Safe mapping with bounds clamping (never trust file)
        auto get_d = [&](const char* k, double def, double lo, double hi) {
            double v = j.value(k, def);
            return std::clamp(v, lo, hi);
        };

        cfg.imbalance_long_threshold  = get_d("imbalance_long_threshold",  cfg.imbalance_long_threshold,  0.01, 0.8);
        cfg.imbalance_short_threshold = get_d("imbalance_short_threshold", cfg.imbalance_short_threshold, -0.8, -0.01);
        cfg.imbalance_defensive_abs   = get_d("imbalance_defensive_abs",   cfg.imbalance_defensive_abs,   0.3, 0.95);

        cfg.spread_min_pct = get_d("spread_min_pct", cfg.spread_min_pct, 0.0001, 0.01);
        cfg.spread_max_pct = get_d("spread_max_pct", cfg.spread_max_pct, 0.001, 0.02);

        cfg.maker_size_frac = get_d("maker_size_frac", cfg.maker_size_frac, 0.0005, 0.02);
        cfg.taker_size_frac = get_d("taker_size_frac", cfg.taker_size_frac, 0.001, 0.03);

        cfg.inventory_max_pct       = get_d("inventory_max_pct", cfg.inventory_max_pct, 0.005, 0.15);
        cfg.spike_z_threshold       = get_d("spike_z_threshold", cfg.spike_z_threshold, 1.5, 8.0);
        cfg.spike_lookback_s        = get_d("spike_lookback_s",  cfg.spike_lookback_s,  2.0, 120.0);
        cfg.spike_ewma_alpha        = get_d("spike_ewma_alpha",  cfg.spike_ewma_alpha,  0.05, 0.8);

        cfg.max_latency_ms          = get_d("max_latency_ms", cfg.max_latency_ms, 1.0, 100.0);
        cfg.max_global_exposure_pct = get_d("max_global_exposure_pct", cfg.max_global_exposure_pct, 0.05, 0.6);
        cfg.max_impact_bps          = get_d("max_impact_bps", cfg.max_impact_bps, 0.5, 50.0);
        cfg.max_cancel_to_trade     = get_d("max_cancel_to_trade", cfg.max_cancel_to_trade, 0.1, 2.0);
        cfg.max_open_orders         = static_cast<int>(get_d("max_open_orders", cfg.max_open_orders, 5, 200));

        cfg.small_cap_mode          = j.value("small_cap_mode", cfg.small_cap_mode);
        cfg.small_cap_inventory_pct = get_d("small_cap_inventory_pct", cfg.small_cap_inventory_pct, 0.005, 0.10);

        cfg.imb_ewma_alpha = get_d("imb_ewma_alpha", cfg.imb_ewma_alpha, 0.05, 0.9);
        cfg.top_k_levels   = static_cast<int>(get_d("top_k_levels", cfg.top_k_levels, 3, 50));

        cfg.rng_seed = j.value("rng_seed", cfg.rng_seed);
        cfg.enable_onchain_mock = j.value("enable_onchain_mock", cfg.enable_onchain_mock);

    } catch (const std::exception& e) {
        std::cerr << "[AdaptiveHybrid] JSON parse error in '" << json_path
                  << "': " << e.what() << " — falling back to defaults.\n";
    }
    return cfg;
}

// end global namespace
