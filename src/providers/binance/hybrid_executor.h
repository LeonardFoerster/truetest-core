#pragma once
#ifdef HAS_BINANCE

#include "../../execution/execution_adapter.h"
#include "../../execution/fee_model.h"
#include "../../execution/latency_model.h"
#include "../../execution/queue_aware_book_adapter.h"
#include "../../execution/queue_model.h"
#include "../../orderbook/orderbook.h"
#include "../../orderbook/fill_model.h"
#include "../../types/order_id.h"
#include "binance_executor.h"

#include <algorithm>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

class HybridExecutor : public IExecutionAdapter
{
public:
    HybridExecutor(std::shared_ptr<BinanceExecutor> paper_exec,
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
            // Use realistic queue-position modeling for passive limits
            book_adapter_ = std::make_unique<QueueAwareBookAdapter>(
                std::move(maker_queue_model),
                fee_model ? std::move(fee_model) : std::make_shared<ZeroFeeModel>(),
                latency_model_);   // reuse the same latency model if present
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
        // Advance the sim-time proxy so previously buffered fills can
        // release on this poll cycle. Orders flow through here in
        // monotonically non-decreasing eligible_ts order (enforced by
        // the engine's pending_orders_ priority queue), so max() is
        // defensive.
        if (o.get_earliest_eligible_ts() > now_proxy_)
            now_proxy_ = o.get_earliest_eligible_ts();

        if (latency_model_)
            order_latencies_[o.get_order_id()] = latency_model_->get_order_latency();

        if (o.get_order_type() == order_type::market) {
            paper_->submit_order(o);
        } else {
            book_adapter_->submit_order(o);
        }
    }

    bool poll_fills(std::vector<fill_event>& out) override
    {
        std::vector<fill_event> inner;
        paper_->poll_fills(inner);
        book_adapter_->poll_fills(inner);

        // Fast path - no latency configured, pass fills through.
        if (!latency_model_) {
            if (inner.empty()) return false;
            for (auto& f : inner) out.push_back(std::move(f));
            return true;
        }

        // Buffer each fill with its release_ts = fill_ts + per-order
        // latency sampled at submit time. The fill's own timestamp is
        // left alone - it records when the book matched; release_ts
        // records when the engine can see it.
        for (auto& f : inner) {
            auto it = order_latencies_.find(f.get_order_id());
            auto latency = (it != order_latencies_.end())
                ? it->second : latency_duration(0);
            delayed_fills_.push_back({std::move(f), f.get_timestamp() + latency});
        }

        bool released = false;
        auto new_end = std::remove_if(delayed_fills_.begin(), delayed_fills_.end(),
            [&](delayed_fill& df) {
                if (df.release_ts <= now_proxy_) {
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
        if (new_end != delayed_fills_.end()) {
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

        // Re-seed only our own synthetic quotes. clear() would also destroy
        // any strategy limit order resting in the shared book — leaving it
        // tracked as open but unable to ever fill.
        for (auto id : quote_ids_)
            book_->cancel_order(id);
        quote_ids_.clear();

        trades crossed;
        double spread_step = mid * spread_step_factor_;
        for (int i = 1; i <= 10; ++i) {
            double bid_px = mid - i * spread_step;
            double ask_px = mid + i * spread_step;
            quantity qty = static_cast<quantity>(qty_scale_);
            const auto bid_id = OrderIdGenerator::next();
            auto t1 = book_->add_order(std::make_shared<order>(
                ob_order_type::good_till_cancel, bid_id,
                side::buy, Price::from_double(bid_px), qty));
            quote_ids_.push_back(bid_id);
            crossed.insert(crossed.end(), t1.begin(), t1.end());
            const auto ask_id = OrderIdGenerator::next();
            auto t2 = book_->add_order(std::make_shared<order>(
                ob_order_type::good_till_cancel, ask_id,
                side::sell, Price::from_double(ask_px), qty));
            quote_ids_.push_back(ask_id);
            crossed.insert(crossed.end(), t2.begin(), t2.end());
        }

        // Quotes that crossed a resting strategy limit are real maker fills
        // for it — surface them via the inner adapter (LocalBookAdapter).
        if (book_adapter_ && !crossed.empty())
            book_adapter_->on_book_trades(crossed, now_proxy_);

        book_adapter_->set_mid_price(mid);
        paper_->set_last_price(mid);
    }

    void set_l2_seeded(bool seeded) override
    {
        if (book_adapter_) book_adapter_->set_l2_seeded(seeded);
    }

    // Engine deliver_mm / bar-sweep paths call these via IExecutionAdapter
    // (no dynamic_cast to LocalBookAdapter). Forward to the inner paper
    // book adapter so hybrid-registered symbols get the same maker fills.
    void on_book_trades(const trades& trs,
                        std::chrono::system_clock::time_point ts) override
    {
        if (book_adapter_)
            book_adapter_->on_book_trades(trs, ts);
    }

    bool sweep_resting_range(const std::string& symbol,
                             double low, double high,
                             std::chrono::system_clock::time_point ts) override
    {
        return book_adapter_
            ? book_adapter_->sweep_resting_range(symbol, low, high, ts)
            : false;
    }

private:
    struct delayed_fill {
        fill_event fill;
        std::chrono::system_clock::time_point release_ts;
    };

    std::shared_ptr<BinanceExecutor> paper_;
    std::shared_ptr<orderbook> book_;
    std::unique_ptr<IExecutionAdapter> book_adapter_;
    // Our own synthetic quote ids, so re-seeding cancels only these.
    std::vector<uint64_t> quote_ids_;
    double qty_scale_ = 1e8;
    double spread_step_factor_ = 0.0001;

    std::shared_ptr<ILatencyModel> latency_model_;
    std::unordered_map<uint64_t, latency_duration> order_latencies_;
    std::vector<delayed_fill> delayed_fills_;
    std::chrono::system_clock::time_point now_proxy_{};
};

#endif // HAS_BINANCE
