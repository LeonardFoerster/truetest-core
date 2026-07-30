#pragma once
#ifdef HAS_BYBIT

// Venue-local paper + hybrid execution for Bybit non-live modes.
// Mirrors BitgetPaperExecutor / BitgetHybridExecutor without HAS_BINANCE.

#include "execution/execution_adapter.h"
#include "execution/fee_model.h"
#include "execution/latency_model.h"
#include "execution/queue_aware_book_adapter.h"
#include "execution/queue_model.h"
#include "orderbook/fill_model.h"
#include "orderbook/orderbook.h"
#include "types/order_id.h"
#include "ui/console_dashboard.h"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

class BybitPaperExecutor : public IExecutionAdapter
{
public:
    BybitPaperExecutor() = default;

    const std::string& last_error() const override { return last_error_; }

    void set_last_price(double price) { last_price_ = price; }
    void set_mid_price(double price) override { last_price_ = price; }

    void set_symbol(const std::string& sym) { symbol_ = sym; }

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

        if (dashboard_.expired())
        {
            std::cout << "  [PAPER] " << side
                      << " " << o.get_quantity() << " " << o.get_symbol()
                      << " @ " << px << "\n";
        }

        if (o.get_order_type() == order_type::market && last_price_ > 0)
        {
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
                commission);
            pending_fills_.back().set_recv_ns(o.get_recv_ns());
            if (o.get_recv_ns() > 0)
            {
                const int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                pending_fills_.back().set_latency_ns(now_ns - o.get_recv_ns());
            }
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

class BybitHybridExecutor : public IExecutionAdapter
{
public:
    BybitHybridExecutor(std::shared_ptr<BybitPaperExecutor> paper_exec,
                        std::shared_ptr<orderbook> book,
                        std::shared_ptr<IFeeModel> fee_model = nullptr,
                        std::shared_ptr<IFillModel> fill_model = nullptr,
                        double qty_scale = 1e8,
                        double spread_step_factor = 0.0001,
                        std::shared_ptr<ILatencyModel> latency_model = nullptr,
                        std::shared_ptr<IQueueModel> maker_queue_model = nullptr)
        : paper_(std::move(paper_exec))
        , book_(std::move(book))
        , qty_scale_(qty_scale)
        , spread_step_factor_(spread_step_factor)
        , latency_model_(std::move(latency_model))
    {
        if (maker_queue_model)
        {
            book_adapter_ = std::make_unique<QueueAwareBookAdapter>(
                std::move(maker_queue_model),
                fee_model ? std::move(fee_model) : std::make_shared<ZeroFeeModel>(),
                latency_model_);
        }
        else
        {
            book_adapter_ = std::make_unique<LocalBookAdapter>(
                book_,
                fee_model ? std::move(fee_model) : std::make_shared<ZeroFeeModel>(),
                fill_model ? std::move(fill_model)
                           : std::make_shared<RealisticFillModel>(0.05, 0.8, 5.0));
        }
    }

    void submit_order(const order_event& o) override
    {
        if (o.get_earliest_eligible_ts() > now_proxy_)
            now_proxy_ = o.get_earliest_eligible_ts();

        if (latency_model_)
            order_latencies_[o.get_order_id()] = latency_model_->get_order_latency();

        if (o.get_order_type() == order_type::market)
            paper_->submit_order(o);
        else
            book_adapter_->submit_order(o);
    }

    bool poll_fills(std::vector<fill_event>& out) override
    {
        std::vector<fill_event> inner;
        paper_->poll_fills(inner);
        book_adapter_->poll_fills(inner);

        if (!latency_model_)
        {
            if (inner.empty()) return false;
            for (auto& f : inner) out.push_back(std::move(f));
            return true;
        }

        for (auto& f : inner)
        {
            auto it = order_latencies_.find(f.get_order_id());
            auto latency = (it != order_latencies_.end())
                ? it->second : latency_duration(0);
            delayed_fills_.push_back({std::move(f), f.get_timestamp() + latency});
        }

        bool released = false;
        auto new_end = std::remove_if(delayed_fills_.begin(), delayed_fills_.end(),
            [&](delayed_fill& df) {
                if (df.release_ts <= now_proxy_)
                {
                    out.push_back(std::move(df.fill));
                    released = true;
                    return true;
                }
                return false;
            });
        delayed_fills_.erase(new_end, delayed_fills_.end());
        return released;
    }

    bool cancel_order(uint64_t order_id) override
    {
        bool cancelled = book_adapter_->cancel_order(order_id);
        if (!cancelled)
            cancelled = paper_->cancel_order(order_id);

        order_latencies_.erase(order_id);
        auto new_end = std::remove_if(delayed_fills_.begin(), delayed_fills_.end(),
            [order_id](const delayed_fill& df) {
                return df.fill.get_order_id() == order_id;
            });
        if (new_end != delayed_fills_.end())
        {
            delayed_fills_.erase(new_end, delayed_fills_.end());
            cancelled = true;
        }
        return cancelled;
    }

    bool modify_order(uint64_t order_id, double new_price, double new_qty) override
    {
        return book_adapter_->modify_order(order_id, new_price, new_qty);
    }

    void on_l2_snapshot(
        const std::string& symbol,
        const std::vector<std::pair<double, double>>& bids,
        const std::vector<std::pair<double, double>>& asks) override
    {
        book_adapter_->on_l2_snapshot(symbol, bids, asks);
    }

    void on_l2_update(
        const std::string& symbol,
        order_side side,
        double price,
        double new_size) override
    {
        book_adapter_->on_l2_update(symbol, side, price, new_size);
    }

    void on_mid_price(double mid)
    {
        if (!(mid > 0.0) || !book_) return;

        book_->clear();
        double spread_step = mid * spread_step_factor_;
        for (int i = 1; i <= 10; ++i)
        {
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

    void set_l2_seeded(bool seeded) override
    {
        if (book_adapter_) book_adapter_->set_l2_seeded(seeded);
    }

private:
    struct delayed_fill
    {
        fill_event fill;
        std::chrono::system_clock::time_point release_ts;
    };

    std::shared_ptr<BybitPaperExecutor> paper_;
    std::shared_ptr<orderbook> book_;
    std::unique_ptr<IExecutionAdapter> book_adapter_;
    double qty_scale_ = 1e8;
    double spread_step_factor_ = 0.0001;

    std::shared_ptr<ILatencyModel> latency_model_;
    std::unordered_map<uint64_t, latency_duration> order_latencies_;
    std::vector<delayed_fill> delayed_fills_;
    std::chrono::system_clock::time_point now_proxy_{};
};

#endif // HAS_BYBIT
