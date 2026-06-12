#pragma once

#include "../core/event.h"
#include "../orderbook/orderbook.h"
#include "../orderbook/fill_model.h"
#include "fee_model.h"
#include "impact_model.h"
#include "latency_model.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <random>
#include <unordered_map>
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

    // Shadow: real trade prints fed so the adapter can match open orders
    // against the tape. Default no-op for backtest/live.
    virtual void on_trade(const std::string& /*symbol*/,
                          double /*trade_price*/,
                          double /*trade_qty*/,
                          std::chrono::system_clock::time_point /*trade_ts*/) {}

    // Drains time-sensitive state (e.g. cancel-in-flight windows).
    virtual void advance_time(std::chrono::system_clock::time_point /*ts*/) {}

    virtual void on_l2_snapshot(
        const std::string& /*symbol*/,
        const std::vector<std::pair<double, double>>& /*bids*/,
        const std::vector<std::pair<double, double>>& /*asks*/) {}

    virtual void on_l2_update(
        const std::string& /*symbol*/,
        order_side /*side*/,
        double /*price*/,
        double /*new_size*/) {}

    // Optional hook for adapters that maintain a seeded book (LocalBookAdapter
    // and QueueAwareBookAdapter). Default no-op.
    virtual void set_mid_price(double /*price*/) {}

    // Returns 0 from adapters that don't model queue position; dashboard
    // renders em-dash. QueueAwareBookAdapter overrides.
    virtual std::size_t   live_quote_count()         const { return 0; }
    virtual std::uint32_t avg_queue_position_bps()   const { return 0; }

    // Rich queue modeling stats (primarily from TradeTapeShadowAdapter for
    // shadow --queue-model l2-snapshot). Used for dashboard and divergence
    // reporting. Default 0 for adapters that don't track.
    virtual std::size_t queue_submitted_with_queue() const { return 0; }
    virtual std::size_t queue_filled_after_drain()   const { return 0; }
    virtual std::size_t queue_blocked_at_eos()       const { return 0; }
};

class LocalBookAdapter : public IExecutionAdapter
{
public:
    LocalBookAdapter(std::shared_ptr<orderbook> ob,
                     std::shared_ptr<IFeeModel> fee_model,
                     std::shared_ptr<IFillModel> fill_model,
                     unsigned rng_seed = 42,
                     double market_aggression = 1.1,
                     double qty_scale = 1e8,
                     std::shared_ptr<ILatencyModel> latency_model = nullptr,
                     std::shared_ptr<IImpactModel>  impact_model  = nullptr,
                     bool walked_book_impact = false)
        : ob_(std::move(ob))
        , fee_model_(std::move(fee_model))
        , fill_model_(std::move(fill_model))
        , fill_rng_(rng_seed)
        , fill_dist_(0.0, 1.0)
        , market_aggression_(market_aggression)
        , qty_scale_(qty_scale)
        , latency_model_(std::move(latency_model))
        , impact_model_(std::move(impact_model))
        , walked_book_impact_(walked_book_impact) {}

    void set_mid_price(double price) { mid_price_ = price; }
    // Symbol carries real L2 depth — enables the walked-book VWAP
    // reference for market orders (walked_book_impact).
    void set_l2_seeded(bool seeded) { l2_seeded_ = seeded; }

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
        {
            double ref_price = (mid_price_ > 0.0) ? mid_price_ : o.get_price();
            bool walked_used = false;

            // Walked-book impact: when L2 depth is real, the actual VWAP
            // of the levels we'd consume IS the honest reference price.
            // Suppresses bar-spread (already does on l2_seeded_) AND the
            // parametric impact model — the walk doesn't compose with a
            // square-root guess on top of the same depth.
            if (walked_book_impact_ && l2_seeded_)
            {
                const double vwap = walked_book_vwap(o.get_side(), o.get_quantity());
                if (vwap > 0.0) {
                    ref_price = vwap;
                    walked_used = true;
                }
                // vwap == 0 → insufficient depth, fall through to mid +
                // impact_model. Underpricing impact for a sweep is worse
                // than admitting the parametric estimate.
            }

            if (!walked_used)
            {
                // Impact BEFORE aggression so aggression still guarantees an
                // immediate cross. ZeroImpactModel (default) is a pass-through.
                if (impact_model_)
                    ref_price = impact_model_->effective_price(o.get_side(),
                                                               o.get_quantity(),
                                                               ref_price);
            }

            book_price = (book_side == side::buy) ? Price::from_double(ref_price * market_aggression_)
                                                  : Price::from_double(ref_price * (2.0 - market_aggression_));
        }
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

        // GTC remainder rests on the book. Track it so later quote
        // updates that cross its level (delivered via on_book_trades)
        // surface as maker fills — without this, resting limits never
        // fill against the synthetic book.
        if (book_order_type == ob_order_type::good_till_cancel &&
            book_order->get_remaining_quantity() > 0)
        {
            resting_[o.get_order_id()] =
                resting_info{book_order, o.get_symbol(), o.get_side()};
        }

        double fade_rate = fill_model_ ? fill_model_->get_fade_rate() : 0.0;

        for (const auto& trade : resulting_trades)
        {
            const bool we_are_bid = (trade.get_bid_trade().orderId_ == o.get_order_id());
            const auto& our_trade_info  = we_are_bid ? trade.get_bid_trade() : trade.get_ask_trade();
            const auto& counter_trade   = we_are_bid ? trade.get_ask_trade() : trade.get_bid_trade();

            // The resting counterparty may itself be a tracked strategy
            // order (strategy-vs-strategy crossing) — surface its maker
            // fill too, not just the aggressor's.
            if (counter_trade.orderId_ != o.get_order_id())
                record_resting_fill(counter_trade, o.get_earliest_eligible_ts());

            if (our_trade_info.orderId_ == o.get_order_id())
            {
                // Fill at the resting counterparty's price — honest
                // passive-side pricing, one event per walked level. The
                // aggressor's book price (mid × aggression for market) is
                // only a crossing limit, never a recorded fill price.
                double fill_price = counter_trade.price_.to_double();
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
        // Models "too slow to pull": cancel starts an in-flight window
        // and the order keeps matching until advance_time() drains it.
        if (latency_model_)
        {
            const auto lat = latency_model_->get_cancel_latency();
            pending_cancels_[order_id] = current_time_ + lat;
            return true;
        }
        ob_->cancel_order(order_id);
        resting_.erase(order_id);
        return true;
    }

    bool modify_order(uint64_t order_id, double new_price, double new_qty) override
    {
        // The book replaces the order object on modify; the tracked
        // pointer would go stale, so the modified order leaves the
        // resting set (it still matches normally at submit-time crossings).
        resting_.erase(order_id);
        Price book_price = Price::from_double(new_price);
        quantity book_qty = static_cast<quantity>(std::round(new_qty * qty_scale_));
        return ob_->modify_order(order_id, book_price, book_qty);
    }

    void advance_time(std::chrono::system_clock::time_point ts) override
    {
        current_time_ = ts;
        for (auto it = pending_cancels_.begin(); it != pending_cancels_.end(); )
        {
            if (it->second <= ts)
            {
                ob_->cancel_order(it->first);
                resting_.erase(it->first);
                it = pending_cancels_.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    // Trades produced by external book activity (MarketMaker quote
    // updates crossing our resting orders). Each trade whose maker side
    // is a tracked resting order becomes a maker fill at that order's
    // own limit price.
    void on_book_trades(const trades& trs,
                        std::chrono::system_clock::time_point ts)
    {
        for (const auto& tr : trs)
        {
            record_resting_fill(tr.get_bid_trade(), ts);
            record_resting_fill(tr.get_ask_trade(), ts);
        }
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
    // Volume-weighted average price for walking `qty` through the
    // passive side of the book. Returns 0 when the book has fewer
    // resting units than requested — the caller falls back to its
    // parametric path because partial-walk VWAP systematically
    // understates impact for sweeps. Cost: a single
    // ob_->get_order_infos() snapshot per call; only invoked from
    // submit_order, not from the market-data hot path.
    double walked_book_vwap(order_side side, double qty) const
    {
        const auto infos = ob_->get_order_infos();
        const auto& levels = (side == order_side::buy)
            ? infos.get_asks() : infos.get_bids();
        double remaining = qty * qty_scale_;
        double cost = 0.0, consumed = 0.0;
        for (const auto& lvl : levels) {
            if (remaining <= 0.0) break;
            const double take = std::min(remaining,
                static_cast<double>(lvl.quantity_));
            cost     += take * lvl.price_.to_double();
            consumed += take;
            remaining -= take;
        }
        if (remaining > 0.0 || consumed <= 0.0) return 0.0;
        return cost / consumed;
    }

    struct resting_info
    {
        order_pointer book_order;   // live book object; remaining qty stays current
        std::string symbol;
        order_side side;
    };

    void record_resting_fill(const trade_info& ti,
                             std::chrono::system_clock::time_point ts)
    {
        auto it = resting_.find(ti.orderId_);
        if (it == resting_.end())
            return;

        const double fill_qty = static_cast<double>(ti.quantity_) / qty_scale_;
        const double fill_price = ti.price_.to_double();  // our own limit price
        double commission = 0.0;
        if (fee_model_)
            commission = fee_model_->compute_commission(
                it->second.side, fill_qty, fill_price, /*is_taker=*/false);

        const double remaining =
            static_cast<double>(it->second.book_order->get_remaining_quantity())
            / qty_scale_;

        pending_fills_.emplace_back(
            ts,
            it->second.symbol,
            ti.orderId_,
            it->second.side,
            fill_qty,
            fill_price,
            commission,
            remaining,
            next_fill_id_++);

        if (it->second.book_order->is_filled())
            resting_.erase(it);
    }

    std::unordered_map<uint64_t, resting_info> resting_;

    std::shared_ptr<ILatencyModel> latency_model_;
    std::shared_ptr<IImpactModel>  impact_model_;
    bool l2_seeded_ = false;
    bool walked_book_impact_ = false;
    std::unordered_map<uint64_t, std::chrono::system_clock::time_point> pending_cancels_;
    std::chrono::system_clock::time_point current_time_{};
};

class ExchangeAdapter : public IExecutionAdapter
{
public:
    void submit_order(const order_event& /*o*/) override {}
    bool poll_fills(std::vector<fill_event>& /*out*/) override { return false; }
};
