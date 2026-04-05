#pragma once
#ifdef HAS_BINANCE

#include "../../execution/execution_adapter.h"
#include "../../execution/fee_model.h"
#include "../../orderbook/orderbook.h"
#include "../../orderbook/fill_model.h"
#include "binance_executor.h"

#include <memory>
#include <vector>

// HybridExecutor: combines paper market fills (via BinanceExecutor) with
// book-based limit fills (via LocalBookAdapter) for realistic paper-mode
// limit order simulation.
class HybridExecutor : public IExecutionAdapter
{
public:
    HybridExecutor(std::shared_ptr<BinanceExecutor> paper_exec,
                   std::shared_ptr<orderbook> book,
                   std::shared_ptr<IFeeModel> fee_model = nullptr,
                   std::shared_ptr<IFillModel> fill_model = nullptr)
        : paper_(std::move(paper_exec))
        , book_(std::move(book))
        , book_adapter_(std::make_unique<LocalBookAdapter>(
              book_,
              fee_model ? std::move(fee_model) : std::make_shared<ZeroFeeModel>(),
              fill_model ? std::move(fill_model)
                         : std::make_shared<RealisticFillModel>(0.05, 0.8, 5.0)))
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

    // Forward mid-price updates to book adapter for fill probability
    void update_mid_price(double mid)
    {
        book_adapter_->set_mid_price(mid);
    }

    // Forward price to paper executor for market fill simulation
    void update_last_price(double price)
    {
        paper_->set_last_price(price);
    }

    // Access the underlying orderbook for seeding
    std::shared_ptr<orderbook>& get_book() { return book_; }

private:
    std::shared_ptr<BinanceExecutor> paper_;
    std::shared_ptr<orderbook> book_;
    std::unique_ptr<LocalBookAdapter> book_adapter_;
};

#endif // HAS_BINANCE
