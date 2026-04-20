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
    double quantity;
};

class MarketMaker
{
public:
    MarketMaker();
    explicit MarketMaker(unsigned rng_seed);

    void add_orders(std::shared_ptr<orderbook> ob, double current_price, int num_orders = 10);

    void replenish(std::shared_ptr<orderbook> ob, double current_price);

    std::vector<mm_order> compute_replenish(double current_price);

private:
    double compute_volatility() const;

    std::mt19937 gen_;
    std::uniform_real_distribution<> dis_;

    std::deque<double> price_history_;
    static constexpr std::size_t volatility_window_ = 50;

    int levels_per_side_ = 10;
    int base_depth_ = 100;
    double base_spread_pct_ = 0.002;
    double vol_spread_mult_ = 5.0;
};
