#pragma once
#ifdef HAS_BINANCE

#include "../../execution/execution_adapter.h"
#include "../../execution/fee_model.h"
#include "../../orderbook/orderbook.h"
#include "../../orderbook/fill_model.h"
#include "../../types/order_id.h"
#include "binance_executor.h"

#include <memory>
#include <vector>

// HybridExecutor: combines paper market fills (via BinanceExecutor) with
// book-based limit fills (via LocalBookAdapter) for realistic paper-mode
// limit order simulation.
//
// Owns the synthetic orderbook seeding that was previously driven by the
// engine. on_mid_price() rebuilds a narrow synthetic book around the given
// mid-price, updates both paper/book executors, and triggers any limit
// fills that the new levels make possible.
class HybridExecutor : public IExecutionAdapter
{
public:
    HybridExecutor(std::shared_ptr<BinanceExecutor> paper_exec,
                   std::shared_ptr<orderbook> book,
                   std::shared_ptr<IFeeModel> fee_model = nullptr,
                   std::shared_ptr<IFillModel> fill_model = nullptr,
                   double qty_scale = 1e8,
                   double spread_step_factor = 0.0001)
        : paper_(std::move(paper_exec))
        , book_(std::move(book))
        , book_adapter_(std::make_unique<LocalBookAdapter>(
              book_,
              fee_model ? std::move(fee_model) : std::make_shared<ZeroFeeModel>(),
              fill_model ? std::move(fill_model)
                         : std::make_shared<RealisticFillModel>(0.05, 0.8, 5.0)))
        , qty_scale_(qty_scale)
        , spread_step_factor_(spread_step_factor)
    {}

    void submit_order(const order_event& o) override
    {
        if (o.get_order_type() == order_type::market) {
            paper_->submit_order(o);
        } else {
            book_adapter_->submit_order(o);
        }
    }

    bool poll_fills(std::vector<fill_event>& out) override
    {
        bool had_paper = paper_->poll_fills(out);
        bool had_book = book_adapter_->poll_fills(out);
        return had_paper || had_book;
    }

    bool cancel_order(uint64_t order_id) override
    {
        // Try both adapters — order could be in either
        bool cancelled = book_adapter_->cancel_order(order_id);
        if (!cancelled)
            cancelled = paper_->cancel_order(order_id);
        return cancelled;
    }

    bool modify_order(uint64_t order_id, double new_price, double new_qty) override
    {
        // Only book adapter supports modify (limit orders on local book)
        return book_adapter_->modify_order(order_id, new_price, new_qty);
    }

    // Reseed the synthetic book around `mid` and forward the mid/last
    // prices to the internal adapters. Called by the provider whenever
    // a new market event updates the mid-price.
    void on_mid_price(double mid)
    {
        if (!(mid > 0.0) || !book_) return;

        book_->clear();
        double spread_step = mid * spread_step_factor_;
        for (int i = 1; i <= 10; ++i) {
            double bid_px = mid - i * spread_step;
            double ask_px = mid + i * spread_step;
            quantity qty = static_cast<quantity>(qty_scale_);
            book_->add_order(std::make_shared<order>(
                ob_order_type::good_till_cancel, OrderIdGenerator::next(),
                side::buy, Price::from_double(bid_px), qty));
            book_->add_order(std::make_shared<order>(
                ob_order_type::good_till_cancel, OrderIdGenerator::next(),
                side::sell, Price::from_double(ask_px), qty));
        }

        book_adapter_->set_mid_price(mid);
        paper_->set_last_price(mid);
    }

private:
    std::shared_ptr<BinanceExecutor> paper_;
    std::shared_ptr<orderbook> book_;
    std::unique_ptr<LocalBookAdapter> book_adapter_;
    double qty_scale_ = 1e8;
    double spread_step_factor_ = 0.0001;
};

#endif // HAS_BINANCE
