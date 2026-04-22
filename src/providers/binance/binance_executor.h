#pragma once
#ifdef HAS_BINANCE

#include "execution/execution_adapter.h"
#include "execution/fee_model.h"
#include "ui/console_dashboard.h"

#include <algorithm>
#include <cstdio>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

class BinanceExecutor : public IExecutionAdapter
{
public:
    BinanceExecutor() = default;

    const std::string& last_error() const { return last_error_; }

    void set_last_price(double price) { last_price_ = price; }

    void set_symbol(const std::string& sym) { symbol_ = sym; }

    // When set, paper-order log lines are delivered to the dashboard's
    // recent-events pane instead of stdout, so they don't interleave
    // with the TUI repaint and push the box down.
    void set_dashboard(std::weak_ptr<truetest::ui::ConsoleDashboard> dash)
    {
        dashboard_ = std::move(dash);
    }

    void set_fee_model(std::shared_ptr<IFeeModel> fm) { fee_model_ = std::move(fm); }

    void submit_order(const order_event& o) override
    {
        last_error_.clear();

        const double px = last_price_ > 0 ? last_price_ : o.get_price();
        const char* side = (o.get_side() == order_side::buy) ? "BUY" : "SELL";

        // When the dashboard owns stdout, suppress per-order output entirely
        // — neither stdout nor the recent-events pane. The summary counters
        // (Events / Fills / Round-trips) already reflect the order flow, and
        // a busy strategy would otherwise scroll a [PAPER] line per trade
        // and drown out everything else in the TUI. In headless / log-file
        // runs we still want one line per order for replay grep-ability.
        if (dashboard_.expired())
        {
            std::cout << "  [PAPER] " << side
                      << " " << o.get_quantity() << " " << o.get_symbol()
                      << " @ " << px << "\n";
        }

        if (o.get_order_type() == order_type::market && last_price_ > 0)
        {
            // Market orders are always taker. Without this commission,
            // paper-mode P&L ignored Binance's 0.1% spot fees entirely
            // and diverged from shadow/live after the very first fill.
            double commission = 0.0;
            if (fee_model_)
                commission = fee_model_->compute_commission(
                    o.get_side(), o.get_quantity(), last_price_,
                    /*is_taker=*/true);

            pending_fills_.emplace_back(
                o.get_earliest_eligible_ts(),
                o.get_symbol(),
                o.get_order_id(),
                o.get_side(),
                o.get_quantity(),
                last_price_,
                commission
            );
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
        auto it = std::remove_if(pending_fills_.begin(), pending_fills_.end(),
            [order_id](const fill_event& f) { return f.get_order_id() == order_id; });
        bool found = (it != pending_fills_.end());
        pending_fills_.erase(it, pending_fills_.end());
        return found;
    }

private:
    std::string symbol_;
    double last_price_ = 0.0;
    std::vector<fill_event> pending_fills_;
    std::string last_error_;
    std::weak_ptr<truetest::ui::ConsoleDashboard> dashboard_;
    std::shared_ptr<IFeeModel> fee_model_;
};

#endif // HAS_BINANCE
