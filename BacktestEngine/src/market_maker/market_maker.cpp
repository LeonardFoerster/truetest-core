#include "market_maker.h"
#include <chrono>
#include <cmath>
#include <numeric>

MarketMaker::MarketMaker()
    : gen_(static_cast<unsigned>(std::chrono::system_clock::now().time_since_epoch().count()))
    , dis_(0.0, 0.005) {}

MarketMaker::MarketMaker(unsigned rng_seed)
    : gen_(rng_seed)
    , dis_(0.0, 0.005) {}

void MarketMaker::add_orders(std::shared_ptr<orderbook> ob, double current_price, int num_orders)
{
    for (int i = 0; i < num_orders; ++i)
    {
        double spread = dis_(gen_);
        double buy_price = current_price * (1 - spread);
        double sell_price = current_price * (1 + spread);

        auto buy_order = std::make_shared<order>(
            ob_order_type::good_till_cancel, OrderIdGenerator::next(), side::buy,
            Price::from_double(buy_price), 100);
        auto sell_order = std::make_shared<order>(
            ob_order_type::good_till_cancel, OrderIdGenerator::next(), side::sell,
            Price::from_double(sell_price), 100);

        ob->add_order(buy_order);
        ob->add_order(sell_order);
    }
}

double MarketMaker::compute_volatility() const
{
    if (price_history_.size() < 2)
        return 0.0;

    // Compute standard deviation of returns
    std::vector<double> returns;
    returns.reserve(price_history_.size() - 1);
    for (std::size_t i = 1; i < price_history_.size(); ++i)
    {
        if (price_history_[i - 1] > 0.0)
            returns.push_back((price_history_[i] - price_history_[i - 1]) / price_history_[i - 1]);
    }

    if (returns.empty())
        return 0.0;

    double mean = std::accumulate(returns.begin(), returns.end(), 0.0)
                  / static_cast<double>(returns.size());
    double sq_sum = 0.0;
    for (double r : returns)
        sq_sum += (r - mean) * (r - mean);

    return std::sqrt(sq_sum / static_cast<double>(returns.size()));
}

std::vector<mm_order> MarketMaker::compute_replenish(double current_price)
{
    std::vector<mm_order> orders;

    if (current_price <= 0.0)
        return orders;

    // Update price history for volatility
    price_history_.push_back(current_price);
    if (price_history_.size() > volatility_window_)
        price_history_.pop_front();

    // Compute spread: base spread widened by recent volatility
    double vol = compute_volatility();
    double half_spread = base_spread_pct_ + vol * vol_spread_mult_;

    // Place orders at multiple levels away from mid
    for (int i = 1; i <= levels_per_side_; ++i)
    {
        double distance = half_spread * i;

        double bid_price = current_price * (1.0 - distance);
        double ask_price = current_price * (1.0 + distance);

        // Depth increases with distance from mid (more liquidity further out)
        int depth = base_depth_ * i;

        if (Price::from_double(bid_price) > Price())
        {
            orders.push_back({order_side::buy, bid_price, depth});
        }
        orders.push_back({order_side::sell, ask_price, depth});
    }

    return orders;
}

void MarketMaker::replenish(std::shared_ptr<orderbook> ob, double current_price)
{
    auto orders = compute_replenish(current_price);
    for (const auto& mo : orders)
    {
        Price p = Price::from_double(mo.price);
        auto ob_side = (mo.side == order_side::buy) ? side::buy : side::sell;
        auto ob_order = std::make_shared<order>(
            ob_order_type::good_till_cancel, OrderIdGenerator::next(),
            ob_side, p, static_cast<quantity>(mo.quantity));
        ob->add_order(ob_order);
    }
}
