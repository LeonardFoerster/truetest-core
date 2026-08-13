#pragma once

#include "execution_adapter.h"
#include "queue_aware_book_adapter.h"

#include <memory>
#include <utility>
#include <vector>

// Paper hybrid: QueueAware for passive limits (queue realism), LocalBook for
// market/stop/stop_limit (protective exits + aggressive entries). Without this,
// --maker-queue-model silently no-ops market orders including ExitManager SL/TP.
class HybridPaperAdapter : public IExecutionAdapter
{
public:
    HybridPaperAdapter(std::shared_ptr<LocalBookAdapter> local,
                       std::shared_ptr<QueueAwareBookAdapter> queue)
        : local_(std::move(local))
        , queue_(std::move(queue))
    {
    }

    void set_mid_price(double price) override
    {
        if (local_) local_->set_mid_price(price);
        if (queue_) queue_->set_mid_price(price);
    }

    void set_l2_seeded(bool seeded) override
    {
        if (local_) local_->set_l2_seeded(seeded);
    }

    void submit_order(const order_event& o) override
    {
        if (o.get_order_type() == order_type::limit)
        {
            if (queue_) queue_->submit_order(o);
            return;
        }
        // market / stop / stop_limit → local book (immediate or adapter policy)
        if (local_) local_->submit_order(o);
    }

    bool poll_fills(std::vector<fill_event>& out) override
    {
        bool any = false;
        if (local_ && local_->poll_fills(out)) any = true;
        if (queue_ && queue_->poll_fills(out)) any = true;
        return any;
    }

    bool cancel_order(uint64_t order_id) override
    {
        // Prefer queue (limits live there) then local. Do not OR a always-true
        // local cancel that would mask "unknown id".
        if (queue_ && queue_->cancel_order(order_id))
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
        (void)order_id;
        (void)new_price;
        (void)new_qty;
        return false;
    }

    void advance_time(std::chrono::system_clock::time_point ts) override
    {
        if (local_) local_->advance_time(ts);
        if (queue_) queue_->advance_time(ts);
    }

    void on_l2_snapshot(const std::string& symbol,
                        const std::vector<std::pair<double, double>>& bids,
                        const std::vector<std::pair<double, double>>& asks) override
    {
        if (queue_) queue_->on_l2_snapshot(symbol, bids, asks);
        if (local_) local_->on_l2_snapshot(symbol, bids, asks);
    }

    void on_l2_update(const std::string& symbol,
                      order_side side,
                      double price,
                      double new_size) override
    {
        if (queue_) queue_->on_l2_update(symbol, side, price, new_size);
        if (local_) local_->on_l2_update(symbol, side, price, new_size);
    }

    void on_trade(const std::string& symbol,
                  double trade_price,
                  double trade_qty,
                  std::chrono::system_clock::time_point trade_ts) override
    {
        if (queue_) queue_->on_trade(symbol, trade_price, trade_qty, trade_ts);
        if (local_) local_->on_trade(symbol, trade_price, trade_qty, trade_ts);
    }

    void on_book_trades(const trades& trs,
                        std::chrono::system_clock::time_point ts) override
    {
        if (local_) local_->on_book_trades(trs, ts);
    }

    bool sweep_resting_range(const std::string& symbol,
                             double low, double high,
                             std::chrono::system_clock::time_point ts,
                             double bar_volume = 0.0) override
    {
        // Limits live on queue under maker_queue; local only holds market/stop.
        // Must forward to both or bar [low,high] silently misses QueueAware
        // resting limits (close-only tape requires exact price match).
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

    std::size_t live_quote_count() const override
    {
        std::size_t n = 0;
        if (local_) n += local_->live_quote_count();
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
    std::shared_ptr<LocalBookAdapter> local_;
    std::shared_ptr<QueueAwareBookAdapter> queue_;
};
