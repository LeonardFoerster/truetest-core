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
#include <limits>
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
// default is CONSERVATIVE (size_ahead = +inf so on_trade never fills);
// bar sweep_resting_range still fills on [low,high]. Opt-in
// set_join_front_without_l2(true) restores legacy join-front.
// V1 limitations: trade side ignored (correct when at top-of-book, approx
// otherwise); market/stop orders are not handled here — use HybridPaperAdapter
// (limits → queue-aware, market/stop → LocalBook). Hybrid queue limits
// fail-closed on modify (no cancel+resubmit rewrite).
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
        // Unknown level: default is CONSERVATIVE (size_ahead = +inf so
        // on_trade never fills without L2 — FR-queue-no-l2-optimistic-join).
        // Bar sweep_resting_range still fills on [low,high]. Opt-in
        // set_join_front_without_l2(true) restores legacy optimistic join.
        const auto key = make_key(po.symbol, po.side, po.price);
        auto it = levels_.find(key);
        if (it != levels_.end())
        {
            po.size_ahead = it->second.aggregate_size;
        }
        else if (join_front_without_l2_)
        {
            po.size_ahead = 0.0;
        }
        else
        {
            // No L2 level: size_ahead = +inf so on_trade never fills without depth.
            po.size_ahead = std::numeric_limits<double>::infinity();
        }
        po.submit_ts = o.get_earliest_eligible_ts();
        po.strategy_name = o.get_strategy_name();
        po.opener_order_id = o.get_opener_order_id();
        po.recv_ns = o.get_recv_ns();
        orders_[po.order_id] = std::move(po);
    }

    // Opt-in optimistic join when L2 level is unknown (tests / explicit research).
    // Default false: research-honest — no front-of-queue fills without depth.
    void set_join_front_without_l2(bool enable) { join_front_without_l2_ = enable; }
    bool join_front_without_l2() const { return join_front_without_l2_; }

    bool poll_fills(std::vector<fill_event>& out) override
    {
        if (pending_fills_.empty()) return false;
        out.insert(out.end(),
                   std::make_move_iterator(pending_fills_.begin()),
                   std::make_move_iterator(pending_fills_.end()));
        pending_fills_.clear();
        return true;
    }

    // Hard cap on undrained fill events (MEDIUM-04). Engine must poll after
    // on_trade/sweep; if not, further fills are refused (fail-closed) so the
    // buffer cannot grow unboundedly.
    void set_max_pending_fills(std::size_t n) { max_pending_fills_ = n ? n : 1; }
    std::size_t max_pending_fills() const { return max_pending_fills_; }
    std::size_t dropped_fills_for_cap() const { return dropped_fills_for_cap_; }

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

        // No resting limits and no tracked levels → pure no-op (paper tape under
        // maker_queue feeds every bar/tick; do not grow levels_ unboundedly).
        if (orders_.empty() && levels_.empty())
            return;

        // Same-price residual qty is shared across resting limits. Walk them
        // in deterministic FIFO (submit_ts, then order_id) so MC/backtest
        // fill attribution does not depend on unordered_map bucket order.
        trade_candidates_.clear();
        if (!orders_.empty())
        {
            trade_candidates_.reserve(orders_.size());
            for (const auto& [oid, po] : orders_)
            {
                if (po.symbol == symbol
                    && std::abs(po.price - trade_price) < 1e-12)
                {
                    trade_candidates_.push_back(oid);
                }
            }
            std::sort(trade_candidates_.begin(), trade_candidates_.end(),
                      [this](std::uint64_t a, std::uint64_t b) {
                          const auto& pa = orders_.find(a)->second;
                          const auto& pb = orders_.find(b)->second;
                          if (pa.submit_ts != pb.submit_ts)
                              return pa.submit_ts < pb.submit_ts;
                          return a < b;
                      });
        }

        double remaining_qty = trade_qty;

        for (const std::uint64_t oid : trade_candidates_)
        {
            auto it = orders_.find(oid);
            if (it == orders_.end()) continue;
            auto& po = it->second;

            // No L2 observation: skip tape fills (infinite size_ahead).
            if (!std::isfinite(po.size_ahead))
                continue;

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

                // remaining after this fill — engine uses is_partial() to keep
                // order_status::partially_filled / open-order rows correct.
                const double rem = std::max(0.0, po.qty_remaining - fill_qty);
                fill_event f(trade_ts, po.symbol, po.order_id,
                             po.side, fill_qty, trade_price, commission, rem);
                if (!po.strategy_name.empty()) f.set_strategy_name(po.strategy_name);
                if (po.opener_order_id != 0) f.set_opener_order_id(po.opener_order_id);
                f.set_recv_ns(po.recv_ns);
                if (po.recv_ns > 0)
                {
                    const int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count();
                    f.set_latency_ns(now_ns - po.recv_ns);
                }
                if (!try_emplace_fill(std::move(f)))
                    continue; // fail-closed: leave order qty unchanged this print

                po.qty_remaining -= fill_qty;
                remaining_qty    -= fill_qty;

                if (po.qty_remaining <= 0.0)
                    orders_.erase(it);
            }
        }

        // Record trade volume for (old - new - trades = cancels) inference.
        // Only touch existing level keys (or keys with resting orders) so a
        // long volatile paper tape does not insert a new map node per price.
        auto mark_existing = [&](order_side s) {
            const auto key = make_key(symbol, s, trade_price);
            auto lit = levels_.find(key);
            if (lit != levels_.end())
                lit->second.trades_since_update += trade_qty;
        };
        mark_existing(order_side::buy);
        mark_existing(order_side::sell);
    }

    // Bar-mode: OHLCV [low, high] traded through our limit even when the
    // synthetic close-only paper tape never prints at that price. Mirrors
    // LocalBookAdapter::sweep_resting_range so Hybrid + --maker-queue-model
    // does not under-fill passive limits vs non-hybrid bar path.
    // bar_volume > 0 caps aggregate fill qty (FIFO by submit_ts); residual stays.
    bool sweep_resting_range(const std::string& symbol,
                             double low, double high,
                             std::chrono::system_clock::time_point ts,
                             double bar_volume = 0.0) override
    {
        last_sweep_fill_qty_ = 0.0;
        if (orders_.empty() || !(low > 0.0) || !(high > 0.0))
            return false;
        if (low > high) std::swap(low, high);
        current_time_ = ts;

        // Deterministic walk (submit_ts, order_id) — same as on_trade.
        trade_candidates_.clear();
        trade_candidates_.reserve(orders_.size());
        for (const auto& [oid, po] : orders_)
        {
            if (po.symbol != symbol) continue;
            if (pending_cancels_.count(oid)) continue;
            const bool traversed = (po.side == order_side::buy)
                ? (low <= po.price) : (high >= po.price);
            if (!traversed) continue;
            if (!(po.qty_remaining > 0.0)) continue;
            trade_candidates_.push_back(oid);
        }
        if (trade_candidates_.empty()) return false;

        std::sort(trade_candidates_.begin(), trade_candidates_.end(),
                  [this](std::uint64_t a, std::uint64_t b) {
                      const auto& pa = orders_.find(a)->second;
                      const auto& pb = orders_.find(b)->second;
                      if (pa.submit_ts != pb.submit_ts)
                          return pa.submit_ts < pb.submit_ts;
                      return a < b;
                  });

        double volume_left = (bar_volume > 0.0) ? bar_volume
                                                : std::numeric_limits<double>::infinity();
        bool any = false;
        for (const std::uint64_t oid : trade_candidates_)
        {
            if (!(volume_left > 0.0)) break;
            auto it = orders_.find(oid);
            if (it == orders_.end()) continue;
            auto& po = it->second;

            const double fill_qty = std::min(po.qty_remaining, volume_left);
            if (!(fill_qty > 0.0))
                continue;

            double commission = 0.0;
            if (fee_model_)
                commission = fee_model_->compute_commission(
                    po.side, fill_qty, po.price, /*is_taker=*/false);

            const double rem = po.qty_remaining - fill_qty;

            fill_event f(ts, po.symbol, po.order_id,
                         po.side, fill_qty, po.price, commission, rem);
            if (!po.strategy_name.empty()) f.set_strategy_name(po.strategy_name);
            if (po.opener_order_id != 0) f.set_opener_order_id(po.opener_order_id);
            f.set_recv_ns(po.recv_ns);
            if (po.recv_ns > 0)
            {
                const int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                f.set_latency_ns(now_ns - po.recv_ns);
            }
            if (!try_emplace_fill(std::move(f)))
                continue; // fail-closed: leave qty_remaining unchanged

            po.qty_remaining = rem;
            volume_left -= fill_qty;
            last_sweep_fill_qty_ += fill_qty;
            any = true;
            if (rem <= 1e-12)
                orders_.erase(it);
            else
                po.size_ahead = 0.0; // at front after partial bar fill
        }
        return any;
    }

    double last_sweep_fill_qty() const override { return last_sweep_fill_qty_; }

    std::size_t live_order_count() const { return orders_.size(); }
    std::size_t live_quote_count() const override { return orders_.size(); }
    // Diagnostics / hotpath tests: number of tracked L2 level nodes.
    std::size_t tracked_level_count() const { return levels_.size(); }

    // 0 = all at front, 10000 = all at back.
    // Orders without finite size_ahead (no L2) report as fully at back (10000).
    std::uint32_t avg_queue_position_bps() const override
    {
        if (orders_.empty()) return 0;
        double sum_frac = 0.0;
        std::size_t n   = 0;
        for (const auto& [_, po] : orders_)
        {
            if (!std::isfinite(po.size_ahead))
            {
                sum_frac += 1.0; // treat unknown as back of queue
                ++n;
                continue;
            }
            const auto it = levels_.find(make_key(po.symbol, po.side, po.price));
            const double denom = (it != levels_.end()) ? it->second.aggregate_size : 0.0;
            const double frac  = (denom > 0.0) ? std::min(1.0, po.size_ahead / denom) : 0.0;
            sum_frac += frac;
            ++n;
        }
        return static_cast<std::uint32_t>(
            std::lround((sum_frac / static_cast<double>(n)) * 10000.0));
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

    bool try_emplace_fill(fill_event f)
    {
        if (pending_fills_.size() >= max_pending_fills_)
        {
            ++dropped_fills_for_cap_;
            return false;
        }
        pending_fills_.push_back(std::move(f));
        return true;
    }

    std::shared_ptr<IQueueModel>   queue_model_;
    std::shared_ptr<IFeeModel>     fee_model_;
    std::shared_ptr<ILatencyModel> latency_model_;
    bool join_front_without_l2_{false}; // default: conservative no-L2 join
    std::size_t max_pending_fills_{4096};
    std::size_t dropped_fills_for_cap_{0};
    double last_sweep_fill_qty_{0.0};

    std::unordered_map<std::uint64_t, paper_order> orders_;
    std::map<level_key, level_state>               levels_;
    std::vector<fill_event>                        pending_fills_;
    std::unordered_map<std::uint64_t, std::chrono::system_clock::time_point> pending_cancels_;
    // Reused by on_trade for deterministic same-price walk (no per-trade grow after warm).
    std::vector<std::uint64_t>                     trade_candidates_;
    std::chrono::system_clock::time_point          current_time_{};
    double                                         mid_price_ = 0.0;
};
