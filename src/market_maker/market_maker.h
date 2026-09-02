#pragma once
#include "../orderbook/orderbook.h"
#include "../threading/ring_buffer.h"
#include "../core/event.h"
#include "../types/order_id.h"
#include "../reproducibility/deterministic_rng.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <unordered_map>
#include <vector>

// SCOPE BOUNDARY (R1/A02): this file is a *simulation* component. It seeds
// synthetic counterparty liquidity into a backtest orderbook so bar-mode
// replay has something to fill against. It is not a trading strategy, carries
// no inventory state, and its ladder is not a quote decision.
//
// The inventory-aware market-making *strategy* lives in
// src/strategy/market_making/ and never includes this header (enforced by
// scripts/check-layer-deps.sh, check C). Keep the two apart: merging them
// would let simulation furniture decide real quotes.
struct mm_order
{
    order_side side;
    double price;
    double quantity;
};

// Calibration for the synthetic book seeded in bar-mode backtests. The
// seeded spread/depth is the sole source of spread cost for taker fills,
// so these should be tuned to the target market's typical book. Level i
// rests at mid × (1 ± i × (base_spread_pct + vol × vol_spread_mult)),
// depth = base_depth × i.
struct mm_calibration
{
    int levels_per_side = 10;
    int base_depth = 100;
    double base_spread_pct = 0.002;
    // Spread widening per unit of realized bar-return volatility. Real
    // spreads run at a few percent OF bar vol, not multiples of it — a
    // multiplier > 1 inflates the seeded book until nothing fills.
    double vol_spread_mult = 0.25;
    // Cap on the vol-widened per-level half-spread. Without it a single
    // large bar return (e.g. a gap) widens the seeded book so far that
    // no crossing limit reaches it and taker fills silently stop.
    double max_half_spread_pct = 0.05;
    double quantity_scale = 1e8;
};

class MarketMaker
{
public:
    MarketMaker();
    explicit MarketMaker(std::uint64_t rng_seed);

    void set_calibration(const mm_calibration& c);

    void add_orders(std::shared_ptr<orderbook> ob, double current_price, int num_orders = 10);

    // Quote update: cancels this MarketMaker's previous resting orders on
    // the book, then seeds the calibrated ladder around current_price.
    // Without the pull, depth would stack at stale price levels every bar
    // and re-centered books (gap opens, stop triggers) would fill against
    // liquidity the market already moved through.
    //
    // update_history=false re-centers the ladder without feeding
    // current_price into the volatility window — used for intra-bar
    // anchors (bar open before the pending drain, stop trigger price)
    // that would otherwise manufacture phantom open/close volatility.
    //
    // Returns the trades produced by inserting the new quotes. A new
    // quote that crosses a resting strategy order IS that order's fill
    // (the market moved through its level); the engine must route these
    // to the execution adapter or resting limits never fill.
    trades replenish(std::shared_ptr<orderbook> ob, double current_price,
                     bool update_history = true);

    std::vector<mm_order> compute_replenish(double current_price,
                                            bool update_history = true);

    // Phase B (MC reuse)
    void reset(std::uint64_t rng_seed = 0);

private:
    double compute_volatility() const;

    truetest::reproducibility::DeterministicRng rng_;

    std::deque<double> price_history_;
    static constexpr std::size_t volatility_window_ = 50;

    // Live quote ids per book (one MarketMaker serves multiple symbols'
    // books). Cancelling an already-filled id is a no-op in the orderbook.
    std::unordered_map<const orderbook*, std::vector<order_id>> live_quotes_;

    int levels_per_side_ = 10;
    int base_depth_ = 100;
    double base_spread_pct_ = 0.002;
    double vol_spread_mult_ = 0.25;
    double max_half_spread_pct_ = 0.05;
    double quantity_scale_ = 1e8;
};
