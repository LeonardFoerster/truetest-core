#pragma once

#include "../core/event.h"
#include "../orderbook/orderbook.h"
#include "../orderbook/fill_model.h"
#include "fee_model.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <random>
#include <vector>

class IExecutionAdapter
{
public:
    virtual ~IExecutionAdapter() = default;
    virtual void submit_order(const order_event& o) = 0;
    virtual bool poll_fills(std::vector<fill_event>& out) = 0;
    virtual bool cancel_order(uint64_t order_id) { (void)order_id; return false; }
    virtual bool modify_order(uint64_t order_id, double new_price, double new_qty)
    {
        (void)order_id; (void)new_price; (void)new_qty; return false;
    }
};

class LocalBookAdapter : public IExecutionAdapter
{
public:
    LocalBookAdapter(std::shared_ptr<orderbook> ob,
                     std::shared_ptr<IFeeModel> fee_model,
                     std::shared_ptr<IFillModel> fill_model,
                     unsigned rng_seed = 42,
                     double market_aggression = 1.1,
                     double qty_scale = 1e8)
        : ob_(std::move(ob))
        , fee_model_(std::move(fee_model))
        , fill_model_(std::move(fill_model))
        , fill_rng_(rng_seed)
        , fill_dist_(0.0, 1.0)
        , market_aggression_(market_aggression)
        , qty_scale_(qty_scale) {}

    void set_mid_price(double price) { mid_price_ = price; }

    void set_debug_fills(bool enabled, int budget = 20)
    {
        debug_fills_ = enabled;
        debug_fills_left_ = budget;
    }

    void submit_order(const order_event& o) override
    {
        if (o.get_order_type() == order_type::stop || o.get_order_type() == order_type::stop_limit)
            return;

        if (fill_model_ && o.get_order_type() == order_type::limit && mid_price_ > 0.0)
        {
            double distance = std::abs(o.get_price() - mid_price_) / mid_price_;
            double prob = fill_model_->get_fill_probability(o.get_side(), distance);
            if (fill_dist_(fill_rng_) > prob)
                return;
        }

        ob_order_type book_order_type;
        switch (o.get_tif()) {
        case time_in_force::fok: book_order_type = ob_order_type::fill_or_kill; break;
        case time_in_force::ioc: book_order_type = ob_order_type::immediate_or_cancel; break;
        default:                 book_order_type = ob_order_type::good_till_cancel; break;
        }

        side book_side = (o.get_side() == order_side::buy) ? side::buy : side::sell;

        Price book_price;
        if (o.get_order_type() == order_type::market)
            book_price = (book_side == side::buy) ? Price::from_double(o.get_price() * market_aggression_)
                                                  : Price::from_double(o.get_price() * (2.0 - market_aggression_));
        else
            book_price = Price::from_double(o.get_price());

        quantity book_quantity = static_cast<quantity>(std::round(o.get_quantity() * qty_scale_));

        auto book_order = std::make_shared<order>(
            book_order_type, o.get_order_id(), book_side, book_price, book_quantity);

        double pre_bid = 0.0, pre_ask = 0.0;
        if (debug_fills_ && debug_fills_left_ > 0)
        {
            auto infos = ob_->get_order_infos();
            if (!infos.get_bids().empty())
                pre_bid = infos.get_bids().front().price_.to_double();
            if (!infos.get_asks().empty())
                pre_ask = infos.get_asks().front().price_.to_double();
        }

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
                    bool is_taker = (o.get_order_type() == order_type::market) ||
                        (o.get_side() == order_side::buy && mid_price_ > 0.0 && o.get_price() >= mid_price_) ||
                        (o.get_side() == order_side::sell && mid_price_ > 0.0 && o.get_price() <= mid_price_);
                    commission = fee_model_->compute_commission(o.get_side(), fill_qty, fill_price, is_taker);
                }

                double remaining = static_cast<double>(book_order->get_remaining_quantity()) / qty_scale_;

                pending_fills_.emplace_back(
                    o.get_earliest_eligible_ts(),
                    o.get_symbol(),
                    o.get_order_id(),
                    o.get_side(),
                    fill_qty,
                    fill_price,
                    commission,
                    remaining,
                    next_fill_id_++
                );
                pending_fills_.back().set_recv_ns(o.get_recv_ns());
                if (o.get_recv_ns() > 0)
                {
                    int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count();
                    pending_fills_.back().set_latency_ns(now_ns - o.get_recv_ns());
                }

                if (debug_fills_ && debug_fills_left_ > 0)
                {
                    const char* side_str = (o.get_side() == order_side::buy) ? "BUY " : "SELL";
                    std::fprintf(stderr,
                        "[debug-fills] id=%llu %s intended=%.4f book=%.4f fill=%.4f "
                        "bid=%.4f ask=%.4f mid=%.4f qty=%.6f\n",
                        (unsigned long long)o.get_order_id(),
                        side_str,
                        o.get_price(),
                        book_price.to_double(),
                        fill_price,
                        pre_bid,
                        pre_ask,
                        mid_price_,
                        fill_qty);
                    --debug_fills_left_;
                }
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

    bool cancel_order(uint64_t order_id) override
    {
        ob_->cancel_order(order_id);
        return true;
    }

    bool modify_order(uint64_t order_id, double new_price, double new_qty) override
    {
        Price book_price = Price::from_double(new_price);
        quantity book_qty = static_cast<quantity>(std::round(new_qty * qty_scale_));
        return ob_->modify_order(order_id, book_price, book_qty);
    }

private:
    std::shared_ptr<orderbook> ob_;
    std::shared_ptr<IFeeModel> fee_model_;
    std::shared_ptr<IFillModel> fill_model_;
    std::vector<fill_event> pending_fills_;
    std::mt19937 fill_rng_;
    std::uniform_real_distribution<double> fill_dist_;
    double mid_price_ = 0.0;
    double market_aggression_ = 1.1;
    double qty_scale_ = 1e8;
    uint64_t next_fill_id_ = 1;
    bool debug_fills_ = false;
    int debug_fills_left_ = 0;
};

class ExchangeAdapter : public IExecutionAdapter
{
public:
    void submit_order(const order_event& /*o*/) override {}
    bool poll_fills(std::vector<fill_event>& /*out*/) override { return false; }
};
