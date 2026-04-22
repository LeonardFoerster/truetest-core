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
#include <utility>
#include <vector>

// Shadow-mode "exchange" adapter. Matches open orders against the live
// trade tape instead of a paper-seeded orderbook — when a real trade
// prints at a price that would have crossed one of our open limits, we
// synthesize a fill at that price. ShadowTracker then compares these
// trade-tape fills against the engine's primary (simulated) fills to
// surface real slippage and fill-rate divergence.
//
// Matching rules:
//   - BUY limit @ P: fills when a real trade prints at price ≤ P after
//     submit_ts.
//   - SELL limit @ P: fills when a real trade prints at price ≥ P after
//     submit_ts.
//   - MARKET: fills at the next trade price after submit_ts, regardless
//     of side.
//   - Trade qty caps the fill qty; partial fills leave the order live.
//   - Pre-submit trades (ts < submit_ts) are ignored so we never claim a
//     fill we couldn't actually have reached.
//
// Intentional simplifications:
//   - No queue-position modeling — a trade crossing our limit always fills
//     us. This is optimistic vs. real-venue queue depth but matches what
//     most shadow systems assume (and is still strictly more realistic
//     than the paper-seeded HybridExecutor it replaces).
//   - Not thread-safe. The engine drives submit / cancel / on_trade /
//     poll_fills from a single loop thread in shadow mode.
//
// Latency:
//   An optional ILatencyModel represents the wire + exchange-ingest delay
//   on top of whatever engine-side latency has already been applied (the
//   engine's own latency_model gates `earliest_eligible_ts` before the
//   order ever reaches this adapter). The shadow `submit_ts` is therefore
//   `earliest_eligible_ts + wire_latency`, so any real trade printing
//   during the wire-latency window is correctly *missed* on the shadow
//   side — exactly the kind of fill the engine would have booked on the
//   sim side but never actually got at the exchange.
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
        auto it = std::remove_if(open_orders_.begin(), open_orders_.end(),
            [engine_order_id](const open_order& oo) {
                return oo.engine_id == engine_order_id;
            });
        bool found = (it != open_orders_.end());
        open_orders_.erase(it, open_orders_.end());
        return found;
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

    // Called by the engine for every real trade print on the shadowed
    // symbol. For each open order, evaluate the matching rule and emit a
    // fill if crossed.
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
            // Fill at the trade price — that's the execution level the rest
            // of the market actually got. Using the limit price would give
            // a BUY @100 crossed by a trade @99 a worse fill than everyone
            // else on the tape, which is the wrong direction of optimism.
            const double fill_price = trade_price;

            oo.qty_remaining   -= fill_qty;
            remaining_tape_qty -= fill_qty;

            const double rem = std::max(0.0, oo.qty_remaining);

            // A tape-crossing fill is always by definition a taker trade —
            // we only match when a real trade prints through our resting
            // limit (or on any print, for markets). Pass is_taker=true so
            // TieredFeeModel uses the taker bps. Without this, shadow-side
            // P&L ignored Binance's 0.1% spot fees entirely.
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

    // For tests / diagnostics.
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
};
