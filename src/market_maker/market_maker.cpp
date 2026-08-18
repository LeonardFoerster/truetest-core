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

// Calibration survives reset(): it is configuration, not per-trial state.
void MarketMaker::set_calibration(const mm_calibration& c)
{
    levels_per_side_ = c.levels_per_side;
    base_depth_ = c.base_depth;
    base_spread_pct_ = c.base_spread_pct;
    vol_spread_mult_ = c.vol_spread_mult;
    max_half_spread_pct_ = c.max_half_spread_pct;
    quantity_scale_ = c.quantity_scale;
}

void MarketMaker::add_orders(std::shared_ptr<orderbook> ob, double current_price, int num_orders)
{
    for (int i = 0; i < num_orders; ++i)
    {
        double spread = dis_(gen_);
        double buy_price = current_price * (1 - spread);
        double sell_price = current_price * (1 + spread);

        auto buy_order = ob->create_order(
            ob_order_type::good_till_cancel, OrderIdGenerator::next(), side::buy,
            Price::from_double(buy_price),
            static_cast<quantity>(std::round(100 * quantity_scale_)));
        auto sell_order = ob->create_order(
            ob_order_type::good_till_cancel, OrderIdGenerator::next(), side::sell,
            Price::from_double(sell_price),
            static_cast<quantity>(std::round(100 * quantity_scale_)));

        ob->add_external_order(buy_order);
        ob->add_external_order(sell_order);
    }
}

double MarketMaker::compute_volatility() const
{
    if (price_history_.size() < 2)
        return 0.0;

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

std::vector<mm_order> MarketMaker::compute_replenish(double current_price,
                                                     bool update_history)
{
    std::vector<mm_order> orders;

    if (current_price <= 0.0)
        return orders;

    if (update_history)
    {
        price_history_.push_back(current_price);
        if (price_history_.size() > volatility_window_)
            price_history_.pop_front();
    }

    double vol = compute_volatility();
    // Capped: one large bar return (gap) would otherwise widen the book
    // beyond every crossing limit and silently stop taker fills.
    double half_spread = std::min(base_spread_pct_ + vol * vol_spread_mult_,
                                  max_half_spread_pct_);

    for (int i = 1; i <= levels_per_side_; ++i)
    {
        double distance = half_spread * i;

        double bid_price = current_price * (1.0 - distance);
        double ask_price = current_price * (1.0 + distance);

        double depth = static_cast<double>(base_depth_ * i);

        if (Price::from_double(bid_price) > Price())
        {
            orders.push_back({order_side::buy, bid_price, depth});
        }
        orders.push_back({order_side::sell, ask_price, depth});
    }

    return orders;
}

trades MarketMaker::replenish(std::shared_ptr<orderbook> ob, double current_price,
                              bool update_history)
{
    // Pull our previous quotes first (no-op for ids already filled or
    // cancelled), so the book carries exactly one calibrated ladder.
    auto& live = live_quotes_[ob.get()];
    for (order_id id : live)
        ob->cancel_order(id);
    live.clear();

    trades crossings;
    auto orders = compute_replenish(current_price, update_history);
    for (const auto& mo : orders)
    {
        Price p = Price::from_double(mo.price);
        auto ob_side = (mo.side == order_side::buy) ? side::buy : side::sell;
        const order_id id = OrderIdGenerator::next();
        auto ob_order = ob->create_order(
            ob_order_type::good_till_cancel, id,
            ob_side, p,
            static_cast<quantity>(std::round(mo.quantity * quantity_scale_)));
        auto trs = ob->add_external_order(ob_order);
        crossings.insert(crossings.end(), trs.begin(), trs.end());
        live.push_back(id);
    }
    return crossings;
}

// Phase B (MC reuse)
void MarketMaker::reset(unsigned rng_seed)
{
    if (rng_seed != 0)
        gen_.seed(rng_seed);
    else
        gen_.seed(static_cast<unsigned>(std::chrono::system_clock::now().time_since_epoch().count()));

    price_history_.clear();
    live_quotes_.clear();
}
