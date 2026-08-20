#pragma once

#include "execution_adapter.h"
#include "queue_aware_book_adapter.h"

#include <memory>
#include <utility>
#include <vector>

// Paper hybrid: QueueAware for passive limits, deterministic LocalBook for
// BBO-crossing limits, and the configured LocalBook path for market/stop exits.
// Without this, --maker-queue-model can strand both market orders and limits
// that were already marketable when submitted.
class HybridPaperAdapter : public IExecutionAdapter
{
public:
    HybridPaperAdapter(std::shared_ptr<LocalBookAdapter> local,
                       std::shared_ptr<QueueAwareBookAdapter> queue,
                       std::shared_ptr<orderbook> shared_book,
                       std::shared_ptr<LocalBookAdapter> aggressive_limits)
        : local_(std::move(local))
        , queue_(std::move(queue))
        , shared_book_(std::move(shared_book))
        , aggressive_limits_(std::move(aggressive_limits))
    {
    }

    void set_mid_price(double price) override
    {
        if (local_) local_->set_mid_price(price);
        if (has_distinct_aggressive_limits())
            aggressive_limits_->set_mid_price(price);
        if (queue_) queue_->set_mid_price(price);
    }

    void set_l2_seeded(bool seeded) override
    {
        if (local_) local_->set_l2_seeded(seeded);
        if (has_distinct_aggressive_limits())
            aggressive_limits_->set_l2_seeded(seeded);
    }

    void submit_order(const order_event& o) override
    {
        if (o.get_order_type() == order_type::limit)
        {
            const double bbo_mid = marketable_limit_bbo_mid(o);
            const bool immediate = o.get_tif() == time_in_force::ioc
                || o.get_tif() == time_in_force::fok;
            if (aggressive_limits_ && (bbo_mid > 0.0 || immediate))
            {
                // LocalBookAdapter classifies crossing limits as taker from
                // its reference mid; use the same validated BBO midpoint that
                // selected this path. Matching still respects the limit and
                // executes at the resting counterparty price.
                if (bbo_mid > 0.0)
                    aggressive_limits_->set_mid_price(bbo_mid);
                aggressive_limits_->submit_order_against_external(o);
                migrate_aggressive_residual(o);
            }
            else if (queue_)
            {
                queue_->submit_order(o);
            }
            return;
        }
        // Market and triggered stop-market orders retain the configured local
        // fee/impact/latency model, but may consume only external liquidity.
        if (local_) local_->submit_order_against_external(o);
    }

    bool poll_fills(std::vector<fill_event>& out) override
    {
        bool any = false;
        if (local_ && local_->poll_fills(out)) any = true;
        if (has_distinct_aggressive_limits()
            && aggressive_limits_->poll_fills(out)) any = true;
        if (queue_ && queue_->poll_fills(out)) any = true;
        return any;
    }

    bool cancel_order(uint64_t order_id) override
    {
        // Prefer queue (limits live there) then local. Do not OR a always-true
        // local cancel that would mask "unknown id".
        if (queue_ && queue_->cancel_order(order_id))
            return true;
        if (has_distinct_aggressive_limits()
            && aggressive_limits_->cancel_order(order_id))
            return true;
        if (local_ && local_->cancel_order(order_id))
            return true;
        return false;
    }

    bool modify_order(uint64_t order_id, double new_price, double new_qty) override
    {
        // Local may modify resting GTC. QueueAware has no real modify —
        // fail closed (never cancel-and-return-true; that lied to the engine).
        if (local_ && local_->modify_order(order_id, new_price, new_qty))
            return true;
        if (has_distinct_aggressive_limits()
            && aggressive_limits_->modify_order(
                order_id, new_price, new_qty))
            return true;
        (void)order_id;
        (void)new_price;
        (void)new_qty;
        return false;
    }

    void advance_time(std::chrono::system_clock::time_point ts) override
    {
        if (local_) local_->advance_time(ts);
        if (has_distinct_aggressive_limits())
            aggressive_limits_->advance_time(ts);
        if (queue_) queue_->advance_time(ts);
    }

    void on_l2_snapshot(const std::string& symbol,
                        const std::vector<std::pair<double, double>>& bids,
                        const std::vector<std::pair<double, double>>& asks) override
    {
        if (queue_) queue_->on_l2_snapshot(symbol, bids, asks);
        if (local_) local_->on_l2_snapshot(symbol, bids, asks);
        if (has_distinct_aggressive_limits())
            aggressive_limits_->on_l2_snapshot(symbol, bids, asks);
    }

    void on_l2_snapshot(const std::string& symbol,
                        const std::vector<std::pair<double, double>>& bids,
                        const std::vector<std::pair<double, double>>& asks,
                        std::chrono::system_clock::time_point event_ts) override
    {
        if (queue_) queue_->on_l2_snapshot(symbol, bids, asks, event_ts);
        if (local_) local_->on_l2_snapshot(symbol, bids, asks, event_ts);
        if (has_distinct_aggressive_limits())
            aggressive_limits_->on_l2_snapshot(symbol, bids, asks, event_ts);
    }

    void on_l2_update(const std::string& symbol,
                      order_side side,
                      double price,
                      double new_size) override
    {
        if (queue_) queue_->on_l2_update(symbol, side, price, new_size);
        if (local_) local_->on_l2_update(symbol, side, price, new_size);
        if (has_distinct_aggressive_limits())
            aggressive_limits_->on_l2_update(
                symbol, side, price, new_size);
    }

    void on_l2_update(const std::string& symbol,
                      order_side side,
                      double price,
                      double new_size,
                      std::chrono::system_clock::time_point event_ts) override
    {
        if (queue_) queue_->on_l2_update(symbol, side, price, new_size, event_ts);
        if (local_) local_->on_l2_update(symbol, side, price, new_size, event_ts);
        if (has_distinct_aggressive_limits())
            aggressive_limits_->on_l2_update(
                symbol, side, price, new_size, event_ts);
    }

    void on_trade(const std::string& symbol,
                  double trade_price,
                  double trade_qty,
                  std::chrono::system_clock::time_point trade_ts) override
    {
        if (queue_) queue_->on_trade(symbol, trade_price, trade_qty, trade_ts);
        if (local_) local_->on_trade(symbol, trade_price, trade_qty, trade_ts);
        if (has_distinct_aggressive_limits())
            aggressive_limits_->on_trade(
                symbol, trade_price, trade_qty, trade_ts);
    }

    void on_trade(const std::string& symbol,
                  double trade_price,
                  double trade_qty,
                  std::optional<order_side> aggressor_side,
                  std::chrono::system_clock::time_point trade_ts) override
    {
        if (queue_)
            queue_->on_trade(symbol, trade_price, trade_qty, aggressor_side, trade_ts);
        if (local_)
            local_->on_trade(symbol, trade_price, trade_qty, aggressor_side, trade_ts);
        if (has_distinct_aggressive_limits())
            aggressive_limits_->on_trade(
                symbol, trade_price, trade_qty, aggressor_side, trade_ts);
    }

    void on_book_trades(const trades& trs,
                        std::chrono::system_clock::time_point ts) override
    {
        if (local_) local_->on_book_trades(trs, ts);
        if (has_distinct_aggressive_limits())
            aggressive_limits_->on_book_trades(trs, ts);
    }

    bool sweep_resting_range(const std::string& symbol,
                             double low, double high,
                             std::chrono::system_clock::time_point ts,
                             double bar_volume = 0.0) override
    {
        // Passive limits live on queue; the configured local path holds
        // market/stop state and the aggressive local path is normally empty
        // after transferring a partial taker residual into QueueAware.
        // Volume-capped: local first, then queue with remaining budget only
        // (MEDIUM-03 — no double-application of full bar_volume to both).
        // Local goes first deliberately: it holds protective SL/TP and
        // aggressive entries, which must not be starved of the bar's
        // synthetic volume by passive resting limits competing for the same
        // budget — see BacktestDefects.HybridSweep_VolumeBudgetNotDoubleApplied,
        // which locks in this ordering as intentional, not just "no double count".
        bool any = false;
        const bool volume_capped = bar_volume > 0.0;
        double vol_left = bar_volume;
        if (local_ && local_->sweep_resting_range(symbol, low, high, ts, vol_left))
        {
            any = true;
            if (volume_capped)
            {
                vol_left -= local_->last_sweep_fill_qty();
                if (vol_left < 0.0) vol_left = 0.0;
            }
        }
        if (has_distinct_aggressive_limits())
        {
            if (!volume_capped || vol_left > 0.0)
            {
                if (aggressive_limits_->sweep_resting_range(
                        symbol, low, high, ts,
                        volume_capped ? vol_left : 0.0))
                {
                    any = true;
                    if (volume_capped)
                    {
                        vol_left -=
                            aggressive_limits_->last_sweep_fill_qty();
                        if (vol_left < 0.0) vol_left = 0.0;
                    }
                }
            }
        }
        if (queue_)
        {
            if (volume_capped && !(vol_left > 0.0))
                return any;
            if (queue_->sweep_resting_range(symbol, low, high, ts,
                                            volume_capped ? vol_left : 0.0))
                any = true;
        }
        return any;
    }

    double last_sweep_fill_qty() const override
    {
        const double local_qty = local_ ? local_->last_sweep_fill_qty() : 0.0;
        const double aggressive_qty = has_distinct_aggressive_limits()
            ? aggressive_limits_->last_sweep_fill_qty() : 0.0;
        const double queue_qty = queue_ ? queue_->last_sweep_fill_qty() : 0.0;
        return local_qty + aggressive_qty + queue_qty;
    }

    std::size_t live_quote_count() const override
    {
        std::size_t n = 0;
        if (local_) n += local_->live_quote_count();
        if (has_distinct_aggressive_limits())
            n += aggressive_limits_->live_quote_count();
        if (queue_) n += queue_->live_quote_count();
        return n;
    }

    std::uint32_t avg_queue_position_bps() const override
    {
        if (queue_) return queue_->avg_queue_position_bps();
        return 0;
    }

    LocalBookAdapter* local() const { return local_.get(); }
    QueueAwareBookAdapter* queue() const { return queue_.get(); }

private:
    void migrate_aggressive_residual(const order_event& original)
    {
        if (!aggressive_limits_ || !shared_book_ || !queue_
            || original.get_tif() == time_in_force::ioc
            || original.get_tif() == time_in_force::fok)
            return;

        const double remaining = aggressive_limits_->remaining_quantity_base(
            original.get_order_id());
        if (!(remaining > 0.0))
            return;

        if (!aggressive_limits_->cancel_order(original.get_order_id()))
            return;
        order_event residual = original;
        residual.set_quantity(remaining);
        queue_->submit_order(residual);
    }

    bool has_distinct_aggressive_limits() const noexcept
    {
        return aggressive_limits_
            && aggressive_limits_.get() != local_.get();
    }

    double marketable_limit_bbo_mid(const order_event& o) const noexcept
    {
        if (!shared_book_ || !(o.get_price() > 0.0))
            return 0.0;

        const double bid = shared_book_->best_external_bid_price();
        const double ask = shared_book_->best_external_ask_price();
        if (bid > 0.0 && ask > 0.0 && bid > ask)
            return 0.0;
        const double contra = o.get_side() == order_side::buy ? ask : bid;
        if (!(contra > 0.0))
            return 0.0;
        const bool marketable = o.get_side() == order_side::buy
            ? o.get_price() >= contra
            : o.get_price() <= contra;
        if (!marketable)
            return 0.0;
        return bid > 0.0 && ask > 0.0 ? (bid + ask) * 0.5 : contra;
    }

    std::shared_ptr<LocalBookAdapter> local_;
    std::shared_ptr<QueueAwareBookAdapter> queue_;
    std::shared_ptr<orderbook> shared_book_;
    std::shared_ptr<LocalBookAdapter> aggressive_limits_;
};
