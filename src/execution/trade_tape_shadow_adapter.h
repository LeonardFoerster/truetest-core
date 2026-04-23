#pragma once

#include "../core/event.h"
#include "execution_adapter.h"
#include "fee_model.h"
#include "latency_model.h"

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
//
// BUY @P: trade_price ≤ P. SELL @P: trade_price ≥ P. MARKET: first trade.
// Pre-submit trades (ts < submit_ts) ignored. No queue modeling
// (optimistic). Not thread-safe.
//
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
            fe.set_source(fill_source::exchange);
            pending_fills_.push_back(std::move(fe));

            if (oo.qty_remaining <= 1e-12)
                it = open_orders_.erase(it);
            else
                ++it;
        }
    }

    std::size_t open_order_count() const { return open_orders_.size(); }
    std::size_t pending_fill_count() const { return pending_fills_.size(); }

private:
    struct open_order
    {
        uint64_t     engine_id     = 0;
        std::string  symbol;
        order_side   side          = order_side::buy;
        order_type   type          = order_type::limit;
        double       limit_price   = 0.0;
        double       qty_remaining = 0.0;
        std::chrono::system_clock::time_point submit_ts;
    };

    std::vector<open_order> open_orders_;
    std::vector<fill_event> pending_fills_;
    uint64_t next_fill_id_ = 0;
    std::shared_ptr<ILatencyModel> latency_model_;
    std::shared_ptr<IFeeModel>     fee_model_;
    std::unordered_map<uint64_t, std::chrono::system_clock::time_point> pending_cancels_;
    std::chrono::system_clock::time_point current_time_{};
};
