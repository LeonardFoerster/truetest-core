#pragma once

#include "../core/event.h"
#include "../orderbook/orderbook.h"
#include "../orderbook/fill_model.h"
#include "fee_model.h"

#include <cmath>
#include <memory>
#include <random>
#include <vector>

class IExecutionAdapter
{
public:
    virtual ~IExecutionAdapter() = default;
    virtual void submit_order(const order_event& o) = 0;
    virtual bool poll_fills(std::vector<fill_event>& out) = 0;
};

// Wraps the local orderbook for backtest and shadow modes.
// Orders are matched immediately against the simulated book.
class LocalBookAdapter : public IExecutionAdapter
{
public:
    LocalBookAdapter(std::shared_ptr<orderbook> ob,
                     std::shared_ptr<IFeeModel> fee_model,
                     std::shared_ptr<IFillModel> fill_model,
                     unsigned rng_seed = 42)
        : ob_(std::move(ob))
        , fee_model_(std::move(fee_model))
        , fill_model_(std::move(fill_model))
        , fill_rng_(rng_seed)
        , fill_dist_(0.0, 1.0) {}

    void set_mid_price(double price) { mid_price_ = price; }

    void submit_order(const order_event& o) override
    {
        // Stop and stop-limit orders are handled by the engine, not the book
        if (o.get_order_type() == order_type::stop || o.get_order_type() == order_type::stop_limit)
            return;

        // Fill probability check for limit orders
        if (fill_model_ && o.get_order_type() == order_type::limit && mid_price_ > 0.0)
        {
            double distance = std::abs(o.get_price() - mid_price_) / mid_price_;
            double prob = fill_model_->get_fill_probability(o.get_side(), distance);
            if (fill_dist_(fill_rng_) > prob)
                return; // missed fill probability roll
        }

        // Map time_in_force to ob_order_type
        ob_order_type book_order_type;
        switch (o.get_tif()) {
        case time_in_force::fok: book_order_type = ob_order_type::fill_or_kill; break;
        case time_in_force::ioc: book_order_type = ob_order_type::immediate_or_cancel; break;
        default:                 book_order_type = ob_order_type::good_till_cancel; break;
        }

        side book_side = (o.get_side() == order_side::buy) ? side::buy : side::sell;

        // Market orders use aggressive price to sweep available liquidity
        Price book_price;
        if (o.get_order_type() == order_type::market)
            book_price = (book_side == side::buy) ? Price::from_double(o.get_price() * 1.1)
                                                  : Price::from_double(o.get_price() * 0.9);
        else
            book_price = Price::from_double(o.get_price());

        // Scale fractional qty to integer for the orderbook (1e8 scale, like satoshis)
        quantity book_quantity = static_cast<quantity>(std::round(o.get_quantity() * qty_scale_));

        auto book_order = std::make_shared<order>(
            book_order_type, o.get_order_id(), book_side, book_price, book_quantity);

        trades resulting_trades = ob_->add_order(book_order);

        double fade_rate = fill_model_ ? fill_model_->get_fade_rate() : 0.0;

        for (const auto& trade : resulting_trades)
        {
            const auto& our_trade_info =
                (trade.get_bid_trade().orderId_ == o.get_order_id())
                    ? trade.get_bid_trade() : trade.get_ask_trade();

            if (our_trade_info.orderId_ == o.get_order_id())
            {
                double fill_price = our_trade_info.price_.to_double();
                // Unscale from orderbook integer qty back to fractional
                double fill_qty = static_cast<double>(our_trade_info.quantity_) / qty_scale_;

                if (fade_rate > 0.0)
                {
                    fill_qty *= (1.0 - fade_rate);
                    if (fill_qty <= 0.0)
                        continue;
                }

                double commission = 0.0;
                if (fee_model_)
                {
                    // An order is a taker if it crosses the spread (market orders,
                    // or limit orders priced through the opposite side)
                    bool is_taker = (o.get_order_type() == order_type::market) ||
                        (o.get_side() == order_side::buy && mid_price_ > 0.0 && o.get_price() >= mid_price_) ||
                        (o.get_side() == order_side::sell && mid_price_ > 0.0 && o.get_price() <= mid_price_);
                    commission = fee_model_->compute_commission(o.get_side(), fill_qty, fill_price, is_taker);
                }

                pending_fills_.emplace_back(
                    o.get_earliest_eligible_ts(),
                    o.get_symbol(),
                    o.get_order_id(),
                    o.get_side(),
                    fill_qty,
                    fill_price,
                    commission
                );
            }
        }
    }

    bool poll_fills(std::vector<fill_event>& out) override
    {
        if (pending_fills_.empty())
            return false;

        out.insert(out.end(),
                   std::make_move_iterator(pending_fills_.begin()),
                   std::make_move_iterator(pending_fills_.end()));
        pending_fills_.clear();
        return true;
    }

private:
    std::shared_ptr<orderbook> ob_;
    std::shared_ptr<IFeeModel> fee_model_;
    std::shared_ptr<IFillModel> fill_model_;
    std::vector<fill_event> pending_fills_;
    std::mt19937 fill_rng_;
    std::uniform_real_distribution<double> fill_dist_;
    double mid_price_ = 0.0;
    static constexpr double qty_scale_ = 1e8;  // fractional qty → integer scale factor
};

// Stub for future live exchange execution.
// submit_order would forward to exchange REST/WS API.
// poll_fills would check for execution reports.
class ExchangeAdapter : public IExecutionAdapter
{
public:
    void submit_order(const order_event& /*o*/) override
    {
        // TODO: forward to exchange API
    }

    bool poll_fills(std::vector<fill_event>& /*out*/) override
    {
        // TODO: poll exchange for fill reports
        return false;
    }
};
