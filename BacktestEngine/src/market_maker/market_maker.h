#pragma once
#include "../orderbook/orderbook.h"
#include "../threading/ring_buffer.h"
#include "../core/event.h"
#include "../types/order_id.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <random>

struct mm_order
{
    order_side side;
    double price;
    int quantity;
};

class MarketMaker
{
public:
    MarketMaker();
    explicit MarketMaker(unsigned rng_seed);

    // One-time seed (legacy, still used for initial book population)
    void add_orders(std::shared_ptr<orderbook> ob, double current_price, int num_orders = 10);

    // Reactive replenish: called after each market event in single-threaded mode.
    // Replenishes depleted levels, adjusts spread and depth based on volatility.
    void replenish(std::shared_ptr<orderbook> ob, double current_price);

    // Compute replenish orders without touching the orderbook.
    // Used by MarketMakerWorker (extended preset) to produce orders for the inbound ring.
    std::vector<mm_order> compute_replenish(double current_price);

private:
    double compute_volatility() const;

    std::mt19937 gen_;
    std::uniform_real_distribution<> dis_;

    // Volatility tracking: rolling window of recent prices
    std::deque<double> price_history_;
    static constexpr std::size_t volatility_window_ = 50;

    // Replenish parameters
    int levels_per_side_ = 10;
    int base_depth_ = 100;              // base quantity per level
    double base_spread_pct_ = 0.002;    // 0.2% base half-spread
    double vol_spread_mult_ = 5.0;      // volatility multiplier for spread widening
};
