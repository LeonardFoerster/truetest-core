#pragma once

#include "../core/event.h"
#include "execution_adapter.h"
#include "fee_model.h"
#include "latency_model.h"
#include "queue_model.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

// Paper-fills adapter that models queue position against real venue depth:
// records limit orders (never inserted into any book), advances them as
// the trade tape consumes the front of each level, and fires a fill once
// a trade overflows size_ahead. Cancel attribution on L2 shrinkage beyond
// observed trade volume is delegated to a pluggable IQueueModel.
// Requires a trade tape (no trades -> no fills, by design). Without L2,
// every order starts at the front (over-optimistic).
// V1 limitations: trade side ignored (correct when at top-of-book, approx
// otherwise); market orders rejected; modify = cancel + submit.
class QueueAwareBookAdapter : public IExecutionAdapter
{
public:
    explicit QueueAwareBookAdapter(std::shared_ptr<IQueueModel> queue_model,
                                   std::shared_ptr<IFeeModel> fee_model = nullptr,
                                   std::shared_ptr<ILatencyModel> latency_model = nullptr)
        : queue_model_(std::move(queue_model))
        , fee_model_(std::move(fee_model))
        , latency_model_(std::move(latency_model)) {}

    void set_mid_price(double price) override { mid_price_ = price; }

    void submit_order(const order_event& o) override
    {
        if (o.get_order_type() != order_type::limit)
            return;

        paper_order po;
        po.order_id  = o.get_order_id();
        po.symbol    = o.get_symbol();
        po.side      = o.get_side();
        po.price     = o.get_price();
        po.qty_remaining = o.get_quantity();

        // Join back of queue: size_ahead = current aggregate depth.
        // Unknown level -> front (optimistic "no L2 data" degradation).
        const auto key = make_key(po.symbol, po.side, po.price);
        auto it = levels_.find(key);
        po.size_ahead = (it != levels_.end()) ? it->second.aggregate_size : 0.0;
        po.submit_ts = o.get_earliest_eligible_ts();
        po.strategy_name = o.get_strategy_name();
        po.opener_order_id = o.get_opener_order_id();
        po.recv_ns = o.get_recv_ns();
        orders_[po.order_id] = std::move(po);
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

    bool cancel_order(uint64_t order_id) override
    {
        if (orders_.find(order_id) == orders_.end()) return false;

        // Latency model: defer removal until advance_time drains it;
        // trades during the window still consume our queue.
        if (latency_model_)
        {
            const auto lat = latency_model_->get_cancel_latency();
            pending_cancels_[order_id] = current_time_ + lat;
            return true;
        }
        orders_.erase(order_id);
        return true;
    }

    void advance_time(std::chrono::system_clock::time_point ts) override
    {
        current_time_ = ts;
        for (auto it = pending_cancels_.begin(); it != pending_cancels_.end(); )
        {
            if (it->second <= ts)
            {
                orders_.erase(it->first);
                it = pending_cancels_.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    // Levels tracked locally but missing from the snapshot are not
    // zeroed here - the next update stream reconciles them.
    void on_l2_snapshot(const std::string& symbol,
                        const std::vector<std::pair<double, double>>& bids,
                        const std::vector<std::pair<double, double>>& asks) override
    {
        for (const auto& [px, sz] : bids)
            on_l2_update(symbol, order_side::buy, px, sz);
        for (const auto& [px, sz] : asks)
            on_l2_update(symbol, order_side::sell, px, sz);
    }

    void on_l2_update(const std::string& symbol,
                      order_side side,
                      double price,
                      double new_size) override
    {
        const auto key = make_key(symbol, side, price);
        auto& lv = levels_[key];
        const double old_size = lv.aggregate_size;
        const double delta    = new_size - old_size;

        if (delta < 0.0)
        {
            // Trades since the last update don't count as cancels.
            const double reduction       = -delta;
            const double trades_observed = lv.trades_since_update;
            const double cancels         = std::max(0.0, reduction - trades_observed);
            if (cancels > 0.0 && queue_model_)
            {
                for (auto& [oid, po] : orders_)
                {
                    if (po.symbol == symbol && po.side == side
                        && std::abs(po.price - price) < 1e-12)
                    {
                        po.size_ahead =
                            queue_model_->update_on_cancels(
                                po.size_ahead,
                                old_size,
                                cancels);
                    }
                }
            }
        }
        // delta > 0: additions join the back -> size_ahead unchanged.
        lv.aggregate_size       = new_size;
        lv.trades_since_update  = 0.0;
    }

    void on_trade(const std::string& symbol,
                  double trade_price,
                  double trade_qty,
                  std::chrono::system_clock::time_point trade_ts) override
    {
        if (!(trade_qty > 0.0)) return;
        current_time_ = trade_ts;

        double remaining_qty = trade_qty;

        for (auto it = orders_.begin(); it != orders_.end(); )
        {
            auto& po = it->second;
            if (po.symbol != symbol
                || std::abs(po.price - trade_price) >= 1e-12)
            {
                ++it;
                continue;
            }

            const double consumed_ahead = std::min(remaining_qty, po.size_ahead);
            po.size_ahead  -= consumed_ahead;
            remaining_qty  -= consumed_ahead;

            if (po.size_ahead <= 0.0 && remaining_qty > 0.0)
            {
                const double fill_qty = std::min(remaining_qty, po.qty_remaining);
                double commission = 0.0;
                if (fee_model_)
                    commission = fee_model_->compute_commission(
                        po.side, fill_qty, trade_price, /*is_taker=*/false);

                fill_event f(trade_ts, po.symbol, po.order_id,
                             po.side, fill_qty, trade_price, commission);
                if (!po.strategy_name.empty()) f.set_strategy_name(po.strategy_name);
                if (po.opener_order_id != 0) f.set_opener_order_id(po.opener_order_id);
                f.set_recv_ns(po.recv_ns);
                if (po.recv_ns > 0)
                {
                    const int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count();
                    f.set_latency_ns(now_ns - po.recv_ns);
                }
                pending_fills_.push_back(std::move(f));

                po.qty_remaining -= fill_qty;
                remaining_qty    -= fill_qty;

                if (po.qty_remaining <= 0.0)
                {
                    it = orders_.erase(it);
                    continue;
                }
            }
            ++it;
        }

        // Record trade volume for (old - new - trades = cancels) inference.
        // Do NOT decrement aggregate_size - venue already did. Mark both
        // sides in V1; mismatched side's accumulator drains on its next L2.
        auto mark = [&](order_side s) {
            const auto key = make_key(symbol, s, trade_price);
            auto& lv = levels_[key];
            lv.trades_since_update += trade_qty;
        };
        mark(order_side::buy);
        mark(order_side::sell);
    }

    std::size_t live_order_count() const { return orders_.size(); }
    std::size_t live_quote_count() const override { return orders_.size(); }

    // 0 = all at front, 10000 = all at back.
    std::uint32_t avg_queue_position_bps() const override
    {
        if (orders_.empty()) return 0;
        double sum_frac = 0.0;
        std::size_t n   = 0;
        for (const auto& [_, po] : orders_)
        {
            const auto it = levels_.find(make_key(po.symbol, po.side, po.price));
            const double denom = (it != levels_.end()) ? it->second.aggregate_size : 0.0;
            const double frac  = (denom > 0.0) ? std::min(1.0, po.size_ahead / denom) : 0.0;
            sum_frac += frac;
            ++n;
        }
        return static_cast<std::uint32_t>(std::lround((sum_frac / n) * 10000.0));
    }

private:
    struct paper_order
    {
        std::uint64_t order_id{0};
        std::string   symbol;
        order_side    side{order_side::buy};
        double        price{0.0};
        double        qty_remaining{0.0};
        double        size_ahead{0.0};
        std::chrono::system_clock::time_point submit_ts{};
        // Per-lot attribution captured at submit time (Phase 1 deepdive).
        std::string   strategy_name;
        std::uint64_t opener_order_id = 0;
        int64_t       recv_ns = 0;  // tick ingress for tick-to-trade samples
    };
    struct level_state
    {
        double aggregate_size{0.0};
        double trades_since_update{0.0};
    };

    using level_key = std::tuple<std::string, order_side, std::int64_t>;

    static level_key make_key(const std::string& sym, order_side s, double price)
    {
        return {sym, s, static_cast<std::int64_t>(std::llround(price * 1e8))};
    }

    std::shared_ptr<IQueueModel>   queue_model_;
    std::shared_ptr<IFeeModel>     fee_model_;
    std::shared_ptr<ILatencyModel> latency_model_;

    std::unordered_map<std::uint64_t, paper_order> orders_;
    std::map<level_key, level_state>               levels_;
    std::vector<fill_event>                        pending_fills_;
    std::unordered_map<std::uint64_t, std::chrono::system_clock::time_point> pending_cancels_;
    std::chrono::system_clock::time_point          current_time_{};
    double                                         mid_price_ = 0.0;
};
