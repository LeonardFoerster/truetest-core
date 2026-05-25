#include "gbm_generator.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

namespace truetest::simulation {

namespace {

inline provider::bar make_bar(const std::string& symbol,
                              const std::string& date_str,
                              double open, double high, double low, double close,
                              int64_t volume) {
    provider::bar b;
    b.date = date_str;
    b.symbol = symbol;
    b.open = open;
    b.high = high;
    b.low = low;
    b.close = close;
    b.volume = volume;
    return b;
}

inline provider::tick make_tick(const std::string& symbol,
                                int64_t step,
                                double price,
                                int64_t qty) {
    provider::tick t;
    // Use a synthetic monotonic timestamp (milliseconds since arbitrary epoch)
    t.timestamp = std::chrono::system_clock::time_point(
        std::chrono::milliseconds(1'700'000'000'000LL + step * 60'000)); // ~1 min steps
    t.symbol = symbol;
    t.price = price;
    t.quantity = qty;
    t.side = 2; // unknown
    return t;
}

} // anonymous namespace

GBMGenerator::GBMGenerator() = default;

McGeneratorConfig GBMGenerator::default_config() const {
    McGeneratorConfig cfg;
    cfg.symbol = "BTCUSDT";
    cfg.initial_price = 65000.0;
    cfg.n_steps = 5000;
    cfg.dt = 1.0 / (252.0 * 24.0 * 60.0); // ~1-minute bars
    cfg.mu = 0.0;
    cfg.sigma = 0.65;          // realistic-ish annualized vol for crypto
    cfg.emit_synthetic_l2 = false;
    cfg.base_spread_bps = 4.0;
    return cfg;
}

SyntheticPath GBMGenerator::generate(uint64_t seed, const McGeneratorConfig& cfg) {
    SyntheticPath path;
    path.symbol = cfg.symbol;
    path.seed_used = seed;

    const int64_t n = cfg.n_steps;
    if (n <= 0) return path;

    path.mids.reserve(n);
    path.volumes.reserve(n);
    path.bars.reserve(n);
    path.ticks.reserve(n);

    std::mt19937_64 rng(seed);
    std::normal_distribution<double> normal(0.0, 1.0);

    double price = cfg.initial_price;
    const double drift = (cfg.mu - 0.5 * cfg.sigma * cfg.sigma) * cfg.dt;
    const double vol = cfg.sigma * std::sqrt(cfg.dt);

    // Simple volume model: constant mean + small noise
    std::uniform_real_distribution<double> vol_noise(0.7, 1.3);

    for (int64_t i = 0; i < n; ++i) {
        const double z = normal(rng);
        const double prev_price = price;
        price = price * std::exp(drift + vol * z);

        const double high = std::max(prev_price, price) * (1.0 + 0.0005 * std::abs(z));
        const double low  = std::min(prev_price, price) * (1.0 - 0.0005 * std::abs(z));
        const double open = prev_price;
        const double close = price;

        const int64_t volume = static_cast<int64_t>(1000.0 * vol_noise(rng));

        // Date string is synthetic but sortable
        char date_buf[32];
        std::snprintf(date_buf, sizeof(date_buf), "2024-01-%02lld %02lld:%02lld",
                      1 + (i / (24*60)), (i / 60) % 24, i % 60);

        path.bars.push_back(make_bar(cfg.symbol, date_buf, open, high, low, close, volume));
        path.ticks.push_back(make_tick(cfg.symbol, i, close, volume / 10 + 1));
        path.mids.push_back(close);
        path.volumes.push_back(static_cast<double>(volume));

        // Very basic synthetic L2 (only if requested) — constant spread for Phase 1
        if (cfg.emit_synthetic_l2) {
            provider::l2_snapshot snap;
            snap.timestamp = path.ticks.back().timestamp;
            snap.symbol = cfg.symbol;

            const double spread = cfg.base_spread_bps * 1e-4 * close;
            const double bid = close - spread * 0.5;
            const double ask = close + spread * 0.5;

            // Phase 5: slightly more realistic stylized depth (multiple levels + variation)
            for (int lvl = 0; lvl < 3; ++lvl) {
                double lvl_size = 3.0 + 4.0 * normal(rng) * cfg.depth_noise;
                snap.bids.push_back({bid - lvl * 0.01, std::max(0.5, lvl_size)});
                snap.asks.push_back({ask + lvl * 0.01, std::max(0.5, lvl_size)});
            }

            path.l2_snapshots.push_back(std::move(snap));
        }
    }

    return path;
}

std::vector<SyntheticPath> GBMGenerator::generate_batch(
    size_t n_paths,
    uint64_t base_seed,
    const McGeneratorConfig& cfg) {

    std::vector<SyntheticPath> paths;
    paths.reserve(n_paths);

    for (size_t i = 0; i < n_paths; ++i) {
        // Deterministic per-path seed derivation (good statistical independence + reproducibility)
        const uint64_t path_seed = base_seed ^ (static_cast<uint64_t>(i) * 0x9e3779b97f4a7c15ULL);
        paths.push_back(generate(path_seed, cfg));
    }
    return paths;
}

} // namespace truetest::simulation
