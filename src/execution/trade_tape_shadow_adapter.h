#pragma once

#include "../core/event.h"
#include "execution_adapter.h"
#include "fee_model.h"
#include "latency_model.h"
#include "queue_position_model.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// Shadow-mode adapter. Matches open orders against the live trade tape —
// when a real trade prints at a price crossing our limit, synthesize a
// fill at the trade price. ShadowTracker compares these against the sim
// fills to surface real slippage and fill-rate divergence.
// BUY @P: trade_price ≤ P. SELL @P: trade_price ≥ P. MARKET: first trade.
// Pre-submit trades (ts < submit_ts) ignored. No queue modeling
// (optimistic). Not thread-safe.
// Optional ILatencyModel represents wire + exchange-ingest delay ON TOP of
// the engine-side latency already applied — so submit_ts is
// earliest_eligible_ts + wire_latency, and trades printing during that
// window are correctly missed on the shadow side.
class TradeTapeShadowAdapter : public IExecutionAdapter
{
public:
    TradeTapeShadowAdapter() = default;
    explicit TradeTapeShadowAdapter(std::shared_ptr<ILatencyModel> latency_model,
                                    std::shared_ptr<IFeeModel> fee_model = nullptr)
        : latency_model_(std::move(latency_model))
        , fee_model_(std::move(fee_model)) {}

    void set_fee_model(std::shared_ptr<IFeeModel> fm) { fee_model_ = std::move(fm); }
    void set_queue_model(std::shared_ptr<IQueuePositionModel> qm) { queue_model_ = std::move(qm); }

    void submit_order(const order_event& o) override
    {
        open_order oo;
        oo.engine_id     = o.get_order_id();
        oo.symbol        = o.get_symbol();
        oo.side          = o.get_side();
        oo.type          = o.get_order_type();
        oo.limit_price   = o.get_price();
        oo.qty_remaining = o.get_quantity();
        auto arrival_ts  = o.get_earliest_eligible_ts();
        if (latency_model_)
            arrival_ts += latency_model_->get_order_latency();
        oo.submit_ts     = arrival_ts;
        oo.strategy_name = o.get_strategy_name();
        oo.opener_order_id = o.get_opener_order_id();
        if (queue_model_ && o.get_order_type() == order_type::limit)
        {
            oo.queue_ahead   = queue_model_->queue_ahead(
                oo.symbol, oo.side, oo.limit_price, arrival_ts);
            oo.initial_queue = oo.queue_ahead;
            if (oo.queue_ahead > 0.0)
                ++stats_.submitted_with_queue;
        }
        open_orders_.push_back(std::move(oo));
    }

    bool poll_fills(std::vector<fill_event>& out) override
    {
        if (pending_fills_.empty()) return false;
        out.insert(out.end(),
                   std::make_move_iterator(pending_fills_.begin()),
                   std::make_move_iterator(pending_fills_.end()));
        pending_fills_.clear();
        return true;
    }

    bool cancel_order(uint64_t engine_order_id) override
    {
        // Same "too slow to pull" semantics as LocalBookAdapter.
        if (latency_model_)
        {
            auto it = std::find_if(open_orders_.begin(), open_orders_.end(),
                [engine_order_id](const open_order& oo) {
                    return oo.engine_id == engine_order_id;
                });
            if (it == open_orders_.end()) return false;
            const auto lat = latency_model_->get_cancel_latency();
            pending_cancels_[engine_order_id] = current_time_ + lat;
            return true;
        }

        auto it = std::remove_if(open_orders_.begin(), open_orders_.end(),
            [engine_order_id](const open_order& oo) {
                return oo.engine_id == engine_order_id;
            });
        bool found = (it != open_orders_.end());
        open_orders_.erase(it, open_orders_.end());
        return found;
    }

    void advance_time(std::chrono::system_clock::time_point ts) override
    {
        current_time_ = ts;
        if (pending_cancels_.empty()) return;
        for (auto it = pending_cancels_.begin(); it != pending_cancels_.end(); )
        {
            if (it->second <= ts)
            {
                const auto eid = it->first;
                auto oo_it = std::remove_if(open_orders_.begin(), open_orders_.end(),
                    [eid](const open_order& oo) { return oo.engine_id == eid; });
                open_orders_.erase(oo_it, open_orders_.end());
                it = pending_cancels_.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    bool modify_order(uint64_t engine_order_id,
                      double new_price, double new_qty) override
    {
        for (auto& oo : open_orders_)
        {
            if (oo.engine_id == engine_order_id)
            {
                oo.limit_price   = new_price;
                oo.qty_remaining = new_qty;
                return true;
            }
        }
        return false;
    }

    void on_trade(const std::string& symbol,
                  double trade_price,
                  double trade_qty,
                  std::chrono::system_clock::time_point trade_ts) override
    {
        if (open_orders_.empty() || !(trade_qty > 0.0) || !(trade_price > 0.0))
            return;

        double remaining_tape_qty = trade_qty;

        for (auto it = open_orders_.begin(); it != open_orders_.end() && remaining_tape_qty > 0.0;)
        {
            open_order& oo = *it;
            if (oo.symbol != symbol || trade_ts < oo.submit_ts)
            {
                ++it;
                continue;
            }

            const bool crosses =
                (oo.type == order_type::market) ||
                (oo.side == order_side::buy  && trade_price <= oo.limit_price) ||
                (oo.side == order_side::sell && trade_price >= oo.limit_price);

            if (!crosses)
            {
                ++it;
                continue;
            }

            // Drain queue ahead before our order can fill. A trade at-or-
            // better-than our limit consumes the level we sit on, so the
            // tape qty pays down our queue first; only what's left can
            // touch our resting size.
            if (oo.queue_ahead > 0.0)
            {
                const double eaten = std::min(oo.queue_ahead, remaining_tape_qty);
                oo.queue_ahead    -= eaten;
                oo.queue_consumed += eaten;
                remaining_tape_qty -= eaten;
                if (oo.queue_ahead > 1e-12 || remaining_tape_qty <= 0.0)
                {
                    ++it;
                    continue;
                }
                // Queue just drained on this print; the remainder of
                // this trade can fill our order. Count once per order.
                if (oo.initial_queue > 0.0 && !oo.counted_drain)
                {
                    ++stats_.filled_after_drain;
                    oo.counted_drain = true;
                }
            }

            const double fill_qty = std::min(oo.qty_remaining, remaining_tape_qty);
            // Fill at the trade price — the level the rest of the market
            // got. Using the limit would put BUY @100 worse than a @99 tape.
            const double fill_price = trade_price;

            oo.qty_remaining   -= fill_qty;
            remaining_tape_qty -= fill_qty;

            const double rem = std::max(0.0, oo.qty_remaining);

            // Tape-crossing fills are always takers — TieredFeeModel needs
            // is_taker=true or shadow P&L ignores taker fees entirely.
            double commission = 0.0;
            if (fee_model_)
                commission = fee_model_->compute_commission(
                    oo.side, fill_qty, fill_price, /*is_taker=*/true);

            fill_event fe(trade_ts, oo.symbol, oo.engine_id, oo.side,
                          fill_qty, fill_price,
                          commission, rem, ++next_fill_id_);
            if (!oo.strategy_name.empty()) fe.set_strategy_name(oo.strategy_name);
            if (oo.opener_order_id != 0) fe.set_opener_order_id(oo.opener_order_id);
            fe.set_source(fill_source::exchange);
            pending_fills_.push_back(std::move(fe));

            if (oo.qty_remaining <= 1e-12)
                it = open_orders_.erase(it);
            else
                ++it;
        }
    }

    void on_l2_snapshot(
        const std::string& symbol,
        const std::vector<std::pair<double, double>>& bids,
        const std::vector<std::pair<double, double>>& asks) override
    {
        if (queue_model_) queue_model_->on_snapshot(symbol, bids, asks);
    }

    void on_l2_update(
        const std::string& symbol, order_side side,
        double price, double new_size) override
    {
        if (queue_model_) queue_model_->on_update(symbol, side, price, new_size);
    }

    std::size_t open_order_count() const { return open_orders_.size(); }
    std::size_t pending_fill_count() const { return pending_fills_.size(); }

    struct queue_stats
    {
        std::size_t submitted_with_queue = 0; // orders that saw initial_queue > 0
        std::size_t filled_after_drain   = 0; // ... and where queue drained, fill emitted
        std::size_t blocked_at_eos       = 0; // ... and still queue-blocked at session end
    };
    // Caller-side: difference (submitted_with_queue - filled_after_drain
    // - blocked_at_eos) is "queue drained but session ended before our
    // turn touched the tape" — typically the same as blocked_at_eos for
    // short sessions, divergent on long ones.
    queue_stats get_queue_stats() const
    {
        auto s = stats_;
        for (const auto& oo : open_orders_)
            if (oo.initial_queue > 0.0 && oo.queue_ahead > 0.0)
                ++s.blocked_at_eos;
        return s;
    }

    // IExecutionAdapter overrides for Phase 2 queue stats exposure.
    std::size_t queue_submitted_with_queue() const override { return get_queue_stats().submitted_with_queue; }
    std::size_t queue_filled_after_drain()   const override { return get_queue_stats().filled_after_drain; }
    std::size_t queue_blocked_at_eos()       const override { return get_queue_stats().blocked_at_eos; }

private:
    struct open_order
    {
        uint64_t     engine_id      = 0;
        std::string  symbol;
        order_side   side           = order_side::buy;
        order_type   type           = order_type::limit;
        double       limit_price    = 0.0;
        double       qty_remaining  = 0.0;
        // L2-snapshot queue position. Default 0 means "no queue model
        // configured" or "we improve the BBO" — both fall through to
        // the legacy fill-on-cross behaviour.
        double       queue_ahead    = 0.0;
        double       initial_queue  = 0.0;
        double       queue_consumed = 0.0;
        bool         counted_drain  = false;
        std::chrono::system_clock::time_point submit_ts;
        // Per-lot attribution (Phase 1 deepdive consolidation).
        std::string   strategy_name;
        std::uint64_t opener_order_id = 0;
    };

    queue_stats stats_;

    std::vector<open_order> open_orders_;
    std::vector<fill_event> pending_fills_;
    uint64_t next_fill_id_ = 0;
    std::shared_ptr<ILatencyModel>       latency_model_;
    std::shared_ptr<IFeeModel>           fee_model_;
    std::shared_ptr<IQueuePositionModel> queue_model_;
    std::unordered_map<uint64_t, std::chrono::system_clock::time_point> pending_cancels_;
    std::chrono::system_clock::time_point current_time_{};
};
