#pragma once
#ifdef HAS_BINANCE

#include "execution/execution_adapter.h"

#include <algorithm>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

class BinanceExecutor : public IExecutionAdapter
{
public:
    BinanceExecutor() = default;

    const std::string& last_error() const { return last_error_; }

    void set_last_price(double price) { last_price_ = price; }

    void set_symbol(const std::string& sym) { symbol_ = sym; }

    void submit_order(const order_event& o) override
    {
        last_error_.clear();

        std::cout << "  [PAPER] " << (o.get_side() == order_side::buy ? "BUY" : "SELL")
                  << " " << o.get_quantity() << " " << o.get_symbol()
                  << " @ " << (last_price_ > 0 ? last_price_ : o.get_price()) << "\n";

        if (o.get_order_type() == order_type::market && last_price_ > 0)
        {
            pending_fills_.emplace_back(
                o.get_earliest_eligible_ts(),
                o.get_symbol(),
                o.get_order_id(),
                o.get_side(),
                o.get_quantity(),
                last_price_,
                0.0
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
};

#endif // HAS_BINANCE
