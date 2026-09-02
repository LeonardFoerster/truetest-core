#include "gbm_generator.h"

#include "reproducibility/deterministic_rng.h"
#include "reproducibility/deterministic_seed.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
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
                                std::int64_t timestamp_ms,
                                double price,
                                int64_t qty) {
    provider::tick t;
    // Use a synthetic monotonic timestamp (milliseconds since arbitrary epoch)
    t.timestamp = std::chrono::system_clock::time_point(
        std::chrono::milliseconds(timestamp_ms));
    t.symbol = symbol;
    t.price = price;
    t.quantity = qty;
    t.side = 2; // unknown
    return t;
}

} // anonymous namespace

GBMGenerator::GBMGenerator() = default;

McGeneratorConfig GBMGenerator::default_config() const {
    return McGeneratorConfig{};
}

SyntheticPath GBMGenerator::generate(uint64_t seed, const McGeneratorConfig& cfg) {
    validate_mc_generator_config(cfg);
    SyntheticPath path;
    path.symbol = cfg.symbol;
    path.seed_used = seed;

    const int64_t n = cfg.n_steps;
    path.mids.reserve(n);
    path.volumes.reserve(n);
    path.bars.reserve(n);
    path.ticks.reserve(n);

    const reproducibility::DeterministicSeedDeriver seeds(seed);
    reproducibility::DeterministicRng price_rng(
        seeds.derive(reproducibility::SeedDomain::synthetic_price));
    reproducibility::DeterministicRng volume_rng(
        seeds.derive(reproducibility::SeedDomain::synthetic_volume));
    reproducibility::DeterministicRng l2_rng(
        seeds.derive(reproducibility::SeedDomain::synthetic_l2));

    double price = cfg.initial_price;
    const double drift = (cfg.mu - 0.5 * cfg.sigma * cfg.sigma) * cfg.dt;
    const double vol = cfg.sigma * std::sqrt(cfg.dt);
    if (!std::isfinite(drift) || !std::isfinite(vol))
        throw std::invalid_argument(
            "MC parameters produce non-finite GBM coefficients");

    // Simple volume model: constant mean + small noise
    const std::int64_t step_interval_ms = mc_step_interval_ms(cfg);

    for (int64_t i = 0; i < n; ++i) {
        const double z = price_rng.standard_normal();
        const double prev_price = price;
        price = price * std::exp(drift + vol * z);

        const double high = std::max(prev_price, price) * (1.0 + 0.0005 * std::abs(z));
        const double low  = std::min(prev_price, price) * (1.0 - 0.0005 * std::abs(z));
        const double open = prev_price;
        const double close = price;
        if (!(open > 0.0) || !(close > 0.0)
            || !std::isfinite(open) || !std::isfinite(high)
            || !std::isfinite(low) || !std::isfinite(close)
            || high < std::max(open, close)
            || low > std::min(open, close) || high < low)
            throw std::runtime_error(
                "MC path left the supported finite positive price domain");

        const int64_t volume = static_cast<int64_t>(
            1000.0 * volume_rng.uniform(0.7, 1.3));

        // Epoch-ms date strings: parseable with second/ms resolution, strictly
        // increasing by 1 minute (avoids midnight collapse + invalid calendar days).
        const int64_t ts_ms = kSyntheticFirstCloseTimeMs
            + i * step_interval_ms;
        char date_buf[32];
        std::snprintf(date_buf, sizeof(date_buf), "%lld", static_cast<long long>(ts_ms));

        path.bars.push_back(make_bar(cfg.symbol, date_buf, open, high, low, close, volume));
        path.ticks.push_back(make_tick(
            cfg.symbol, ts_ms, close, volume / 10 + 1));
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
            if (!std::isfinite(spread) || !(bid > 0.0)
                || !std::isfinite(bid) || !std::isfinite(ask)
                || ask < bid)
                throw std::runtime_error(
                    "MC parameters produced an invalid synthetic L2 quote");

            // Phase 5: slightly more realistic stylized depth (multiple levels + variation)
            for (int lvl = 0; lvl < 3; ++lvl) {
                const double lvl_size = 3.0
                    + 4.0 * l2_rng.standard_normal() * cfg.depth_noise;
                if (!std::isfinite(lvl_size)
                    || lvl_size < static_cast<double>(
                        std::numeric_limits<std::int64_t>::min())
                    || lvl_size > static_cast<double>(
                        std::numeric_limits<std::int64_t>::max()))
                    throw std::runtime_error(
                        "MC parameters produced invalid synthetic L2 depth");
                const auto quantity = std::max<std::int64_t>(1, std::llround(lvl_size));
                const double bid_level = bid - static_cast<double>(lvl) * 0.01;
                const double ask_level = ask + static_cast<double>(lvl) * 0.01;
                if (!(bid_level > 0.0) || !std::isfinite(bid_level)
                    || !std::isfinite(ask_level) || ask_level < bid_level)
                    throw std::runtime_error(
                        "MC path produced an invalid synthetic L2 level");
                snap.bids.push_back({bid_level, quantity});
                snap.asks.push_back({ask_level, quantity});
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

    validate_mc_batch_capacity(n_paths, cfg);
    std::vector<SyntheticPath> paths;
    paths.reserve(n_paths);

    for (size_t i = 0; i < n_paths; ++i) {
        // Canonical per-trial seed (same as MonteCarloController / derive_mc_trial_seed)
        paths.push_back(generate(derive_mc_trial_seed(base_seed, i), cfg));
    }
    return paths;
}

} // namespace truetest::simulation
