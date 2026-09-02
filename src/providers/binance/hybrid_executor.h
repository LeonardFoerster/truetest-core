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

class BinanceHybridLatencyRelay final : public ILatencyModel
{
public:
    explicit BinanceHybridLatencyRelay(
        std::shared_ptr<ILatencyModel> upstream)
        : upstream_(std::move(upstream)) {}

    latency_duration get_order_latency() override
    {
        return upstream_->get_order_latency();
    }

    latency_duration get_market_data_latency() override
    {
        return upstream_->get_market_data_latency();
    }

    latency_duration get_cancel_latency() override
    {
        last_cancel_latency_ = upstream_->get_cancel_latency();
        cancel_sampled_ = true;
        return last_cancel_latency_;
    }

    void clear_cancel_sample() noexcept { cancel_sampled_ = false; }
    bool cancel_sampled() const noexcept { return cancel_sampled_; }
    latency_duration last_cancel_latency() const noexcept
    {
        return last_cancel_latency_;
    }

private:
    std::shared_ptr<ILatencyModel> upstream_;
    latency_duration last_cancel_latency_{};
    bool cancel_sampled_{false};
};

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
        quote_ids_.reserve(20);
        inner_fills_.reserve(64);
        delayed_fills_.reserve(64);
        order_latencies_.reserve(64);
        if (latency_model_)
            inner_latency_model_ =
                std::make_shared<BinanceHybridLatencyRelay>(latency_model_);
        auto effective_fee = fee_model
            ? std::move(fee_model)
            : std::make_shared<ZeroFeeModel>();
        if (maker_queue_model)
        {
            // Use realistic queue-position modeling for passive limits
            book_adapter_ = std::make_unique<QueueAwareBookAdapter>(
                std::move(maker_queue_model),
                effective_fee,
                inner_latency_model_);
            // Marketable limits are takers even while queue modeling is on.
            // Keep a LocalBook path over the same shared book for them; a
            // single BBO decision below selects exactly one adapter.
            aggressive_limit_adapter_ = std::make_unique<LocalBookAdapter>(
                book_, effective_fee, std::make_shared<PerfectFillModel>(),
                42u, 1.1, qty_scale_);
        }
        else
        {
            auto effective_fill = fill_model
                ? std::move(fill_model)
                : std::make_shared<RealisticFillModel>(0.05, 0.8, 5.0);
            book_adapter_ = std::make_unique<LocalBookAdapter>(
                book_, effective_fee, effective_fill,
                42u, 1.1, qty_scale_, inner_latency_model_);
        }
        bind_fill_id_sequence(fill_ids_);
    }

    void set_fill_id_sequence(
        std::shared_ptr<FillIdSequence> sequence) override
    {
        if (!sequence)
            return;
        fill_ids_ = std::move(sequence);
        bind_fill_id_sequence(fill_ids_);
    }

    void submit_order(const order_event& o) override
    {
        // Advance the sim-time proxy so previously buffered fills can
        // release on this poll cycle. Orders flow through here in
        // monotonically non-decreasing eligible_ts order (enforced by
        // the engine's pending_orders_ priority queue), so max() is
        // defensive.
        if (o.get_earliest_eligible_ts() > now_proxy_)
            advance_time(o.get_earliest_eligible_ts());

        if (latency_model_)
            order_latencies_[o.get_order_id()] = {
                latency_model_->get_order_latency(), o.get_tif()};

        if (o.get_order_type() == order_type::market) {
            paper_->submit_order(o);
        } else if (aggressive_limit_adapter_) {
            const double bbo_mid = marketable_limit_bbo_mid(o);
            const bool immediate = o.get_tif() == time_in_force::ioc
                || o.get_tif() == time_in_force::fok;
            if (!(bbo_mid > 0.0) && !immediate) {
                book_adapter_->submit_order(o);
                return;
            }
            // LocalBookAdapter uses its reference mid to tag limit fees.
            // A valid BBO midpoint makes this crossing limit unambiguously
            // taker while the actual match still occurs at resting prices.
            if (bbo_mid > 0.0)
                aggressive_limit_adapter_->set_mid_price(bbo_mid);
            aggressive_limit_adapter_->submit_order_against_external(o);
            migrate_aggressive_residual(o);
        } else {
            book_adapter_->submit_order(o);
        }
    }

    bool poll_fills(std::vector<fill_event>& out) override
    {
        poll_inner_fills();

        // Fast path - no latency configured, pass fills through.
        if (!latency_model_) {
            if (inner_fills_.empty()) return false;
            for (auto& f : inner_fills_) out.push_back(std::move(f));
            inner_fills_.clear();
            return true;
        }

        // Buffer each fill with its release_ts = fill_ts + per-order
        // latency sampled at submit time. The fill's own timestamp is
        // left alone - it records when the book matched; release_ts
        // records when the engine can see it.
        append_inner_to_delayed();

        std::sort(delayed_fills_.begin(), delayed_fills_.end(),
                  [](const delayed_fill& lhs, const delayed_fill& rhs)
                  {
                      if (lhs.release_ts != rhs.release_ts)
                          return lhs.release_ts < rhs.release_ts;
                      return lhs.fill.get_fill_id()
                          < rhs.fill.get_fill_id();
                  });

        bool released = false;
        auto new_end = std::remove_if(delayed_fills_.begin(), delayed_fills_.end(),
            [&](delayed_fill& df) {
                if (df.release_ts <= now_proxy_) {
                    const auto order_id = df.fill.get_order_id();
                    if (df.final_for_order)
                        order_latencies_.erase(order_id);
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
        // Capture every synchronously matched fill before touching resting
        // state. This assigns its submit-time wire latency and makes the
        // match irreversible even when cancel immediately follows submit.
        if (latency_model_)
            buffer_inner_fills();
        if (inner_latency_model_)
            inner_latency_model_->clear_cancel_sample();

        bool cancelled = book_adapter_->cancel_order(order_id);
        if (!cancelled && aggressive_limit_adapter_)
            cancelled = aggressive_limit_adapter_->cancel_order(order_id);

        // paper_ contains fills that matched synchronously at submit time;
        // delayed_fills_ contains the same fact behind simulated wire delay.
        // Neither is a live order that a later cancel may erase. Only resting
        // book/queue state is cancellable here.
        if (latency_model_)
        {
            const auto it = order_latencies_.find(order_id);
            if (cancelled && it != order_latencies_.end()
                && inner_latency_model_
                && inner_latency_model_->cancel_sampled())
            {
                it->second.cancel_pending = true;
                it->second.cancel_release_ts = now_proxy_
                    + inner_latency_model_->last_cancel_latency();
                if (it->second.cancel_release_ts < next_cancel_cleanup_)
                    next_cancel_cleanup_ = it->second.cancel_release_ts;
            }
            else
            {
                // No live adapter state remains, or the immediately applied
                // cancel used the latency-free taker-transfer adapter.
                order_latencies_.erase(order_id);
            }
        }
        return cancelled;
    }

    bool modify_order(uint64_t order_id, double new_price, double new_qty) override
    {
        if (book_adapter_->modify_order(order_id, new_price, new_qty))
            return true;
        return aggressive_limit_adapter_
            && aggressive_limit_adapter_->modify_order(
                order_id, new_price, new_qty);
    }

    void advance_time(std::chrono::system_clock::time_point ts) override
    {
        if (ts > now_proxy_)
            now_proxy_ = ts;
        paper_->advance_time(ts);
        book_adapter_->advance_time(ts);
        if (aggressive_limit_adapter_)
            aggressive_limit_adapter_->advance_time(ts);
        if (latency_model_ && next_cancel_cleanup_ <= ts)
        {
            // Fills won during the cancel window must retain the original
            // order latency. Drain them before retiring due cancel state.
            buffer_inner_fills();
            next_cancel_cleanup_ =
                std::chrono::system_clock::time_point::max();
            for (auto it = order_latencies_.begin();
                 it != order_latencies_.end(); )
            {
                if (it->second.cancel_pending
                    && it->second.cancel_release_ts <= ts)
                    it = order_latencies_.erase(it);
                else
                {
                    if (it->second.cancel_pending
                        && it->second.cancel_release_ts
                            < next_cancel_cleanup_)
                        next_cancel_cleanup_ =
                            it->second.cancel_release_ts;
                    ++it;
                }
            }
        }
    }

    void on_l2_snapshot(
        const std::string& symbol,
        const std::vector<std::pair<double, double>>& bids,
        const std::vector<std::pair<double, double>>& asks) override
    {
        mark_l2_seeded();
        book_adapter_->on_l2_snapshot(symbol, bids, asks);
        if (aggressive_limit_adapter_)
            aggressive_limit_adapter_->on_l2_snapshot(symbol, bids, asks);
    }

    void on_l2_update(
        const std::string& symbol,
        order_side side,
        double price,
        double new_size) override
    {
        mark_l2_seeded();
        book_adapter_->on_l2_update(symbol, side, price, new_size);
        if (aggressive_limit_adapter_)
            aggressive_limit_adapter_->on_l2_update(
                symbol, side, price, new_size);
    }

    void on_trade(const std::string& symbol,
                  double trade_price,
                  double trade_qty,
                  std::chrono::system_clock::time_point trade_ts) override
    {
        book_adapter_->on_trade(
            symbol, trade_price, trade_qty, trade_ts);
        if (aggressive_limit_adapter_)
            aggressive_limit_adapter_->on_trade(
                symbol, trade_price, trade_qty, trade_ts);
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

        book_adapter_->set_mid_price(mid);
        if (aggressive_limit_adapter_)
            aggressive_limit_adapter_->set_mid_price(mid);
        paper_->set_last_price(mid);
        if (l2_seeded_)
            return;

        double spread_step = mid * spread_step_factor_;
        for (int i = 1; i <= 10; ++i) {
            double bid_px = mid - i * spread_step;
            double ask_px = mid + i * spread_step;
            quantity qty = static_cast<quantity>(qty_scale_);
            const auto bid_id = OrderIdGenerator::next();
            auto t1 = book_->add_external_order(std::make_shared<order>(
                ob_order_type::good_till_cancel, bid_id,
                side::buy, Price::from_double(bid_px), qty));
            quote_ids_.push_back(bid_id);
            if (book_adapter_ && !t1.empty())
                book_adapter_->on_book_trades(t1, now_proxy_);
            if (aggressive_limit_adapter_ && !t1.empty())
                aggressive_limit_adapter_->on_book_trades(t1, now_proxy_);
            const auto ask_id = OrderIdGenerator::next();
            auto t2 = book_->add_external_order(std::make_shared<order>(
                ob_order_type::good_till_cancel, ask_id,
                side::sell, Price::from_double(ask_px), qty));
            quote_ids_.push_back(ask_id);
            if (book_adapter_ && !t2.empty())
                book_adapter_->on_book_trades(t2, now_proxy_);
            if (aggressive_limit_adapter_ && !t2.empty())
                aggressive_limit_adapter_->on_book_trades(t2, now_proxy_);
        }

    }

    void set_l2_seeded(bool seeded) override
    {
        if (seeded) mark_l2_seeded();
        else l2_seeded_ = false;
        if (book_adapter_) book_adapter_->set_l2_seeded(seeded);
        if (aggressive_limit_adapter_)
            aggressive_limit_adapter_->set_l2_seeded(seeded);
    }

    // Engine deliver_mm / bar-sweep paths call these via IExecutionAdapter
    // (no dynamic_cast to LocalBookAdapter). Forward to the inner paper
    // book adapter so hybrid-registered symbols get the same maker fills.
    void on_book_trades(const trades& trs,
                        std::chrono::system_clock::time_point ts) override
    {
        if (book_adapter_)
            book_adapter_->on_book_trades(trs, ts);
        if (aggressive_limit_adapter_)
            aggressive_limit_adapter_->on_book_trades(trs, ts);
    }

    bool sweep_resting_range(const std::string& symbol,
                             double low, double high,
                             std::chrono::system_clock::time_point ts,
                             double bar_volume = 0.0) override
    {
        if (!aggressive_limit_adapter_)
            return book_adapter_
                ? book_adapter_->sweep_resting_range(
                      symbol, low, high, ts, bar_volume)
                : false;

        bool any = aggressive_limit_adapter_->sweep_resting_range(
            symbol, low, high, ts, bar_volume);
        double remaining_volume = bar_volume;
        if (bar_volume > 0.0)
        {
            remaining_volume -=
                aggressive_limit_adapter_->last_sweep_fill_qty();
            if (!(remaining_volume > 0.0))
                return any;
        }
        if (book_adapter_->sweep_resting_range(
                symbol, low, high, ts,
                bar_volume > 0.0 ? remaining_volume : 0.0))
            any = true;
        return any;
    }

    double last_sweep_fill_qty() const override
    {
        const double passive = book_adapter_
            ? book_adapter_->last_sweep_fill_qty() : 0.0;
        const double aggressive = aggressive_limit_adapter_
            ? aggressive_limit_adapter_->last_sweep_fill_qty() : 0.0;
        return passive + aggressive;
    }

    std::size_t pending_latency_order_count() const noexcept
    {
        return order_latencies_.size();
    }

private:
    void mark_l2_seeded()
    {
        l2_seeded_ = true;
        if (!book_)
            return;
        for (auto id : quote_ids_)
            book_->cancel_order(id);
        quote_ids_.clear();
    }

    void migrate_aggressive_residual(const order_event& original)
    {
        if (!aggressive_limit_adapter_ || !book_
            || original.get_tif() == time_in_force::ioc
            || original.get_tif() == time_in_force::fok)
            return;

        const double remaining =
            aggressive_limit_adapter_->remaining_quantity_base(
                original.get_order_id());
        if (!(remaining > 0.0))
            return;

        // Remove the now-passive remainder from the temporary taker adapter
        // before registering it with QueueAware. The taker adapter is built
        // without cancel latency specifically for this internal ownership
        // transfer; user cancels still use QueueAware latency semantics.
        if (!aggressive_limit_adapter_->cancel_order(
                original.get_order_id()))
            return;
        order_event residual = original;
        residual.set_quantity(remaining);
        book_adapter_->submit_order(residual);
    }

    void poll_inner_fills()
    {
        inner_fills_.clear();
        const auto drain = [&](IExecutionAdapter* child)
        {
            if (!child)
                return;
            (void)child->poll_fills(inner_fills_);
        };
        drain(paper_.get());
        if (aggressive_limit_adapter_)
            drain(aggressive_limit_adapter_.get());
        drain(book_adapter_.get());
        std::sort(inner_fills_.begin(), inner_fills_.end(),
                  [](const fill_event& lhs, const fill_event& rhs)
                  {
                      return lhs.get_fill_id() < rhs.get_fill_id();
                  });
    }

    void buffer_inner_fills()
    {
        poll_inner_fills();
        append_inner_to_delayed();
    }

    void append_inner_to_delayed()
    {
        for (auto& f : inner_fills_)
        {
            const auto it = order_latencies_.find(f.get_order_id());
            const auto latency = it != order_latencies_.end()
                ? it->second.latency : latency_duration(0);
            const bool final_for_order = f.get_remaining_qty() <= 1e-12
                || (it != order_latencies_.end()
                    && (it->second.tif == time_in_force::ioc
                        || it->second.tif == time_in_force::fok));
            const auto release_ts = f.get_timestamp() + latency;
            delayed_fills_.push_back(
                {std::move(f), release_ts, final_for_order});
        }
        inner_fills_.clear();
    }

    double marketable_limit_bbo_mid(const order_event& o) const noexcept
    {
        if (o.get_order_type() != order_type::limit || !book_
            || !(o.get_price() > 0.0))
            return 0.0;

        const double bid = book_->best_external_bid_price();
        const double ask = book_->best_external_ask_price();
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

    void bind_fill_id_sequence(
        const std::shared_ptr<FillIdSequence>& sequence)
    {
        if (paper_)
            paper_->set_fill_id_sequence(sequence);
        if (book_adapter_)
            book_adapter_->set_fill_id_sequence(sequence);
        if (aggressive_limit_adapter_)
            aggressive_limit_adapter_->set_fill_id_sequence(sequence);
    }

    struct delayed_fill {
        fill_event fill;
        std::chrono::system_clock::time_point release_ts;
        bool final_for_order{false};
    };

    struct order_latency_state {
        latency_duration latency{};
        time_in_force tif{time_in_force::gtc};
        bool cancel_pending{false};
        std::chrono::system_clock::time_point cancel_release_ts{};
    };

    std::shared_ptr<BinanceExecutor> paper_;
    std::shared_ptr<orderbook> book_;
    std::unique_ptr<IExecutionAdapter> book_adapter_;
    std::unique_ptr<LocalBookAdapter> aggressive_limit_adapter_;
    // Our own synthetic quote ids, so re-seeding cancels only these.
    std::vector<uint64_t> quote_ids_;
    double qty_scale_ = 1e8;
    double spread_step_factor_ = 0.0001;
    bool l2_seeded_ = false;

    std::shared_ptr<ILatencyModel> latency_model_;
    std::shared_ptr<BinanceHybridLatencyRelay> inner_latency_model_;
    std::unordered_map<uint64_t, order_latency_state> order_latencies_;
    std::vector<fill_event> inner_fills_;
    std::vector<delayed_fill> delayed_fills_;
    std::shared_ptr<FillIdSequence> fill_ids_ =
        std::make_shared<FillIdSequence>();
    std::chrono::system_clock::time_point now_proxy_{};
    std::chrono::system_clock::time_point next_cancel_cleanup_{
        std::chrono::system_clock::time_point::max()};
};

#endif // HAS_BINANCE
