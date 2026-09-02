#pragma once

#include "../core/event.h"
#include "../orderbook/orderbook.h"
#include "../orderbook/fill_model.h"
#include "fee_model.h"
#include "impact_model.h"
#include "latency_model.h"
#include "async_support.h"
#include "reproducibility/deterministic_rng.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

// One causal identity namespace shared by every child of a composite paper
// adapter. Children stamp at fill creation time; the composite can then merge
// independently buffered fills without making identity or event order depend
// on how often the engine polls. Engine-thread ownership makes this a plain
// counter rather than an atomic hot-path contention point.
class FillIdSequence final
{
public:
    [[nodiscard]] std::uint64_t next() noexcept
    {
        if (next_ == 0)
            return 0; // exhausted: canonical ingress rejects instead of wrap
        const auto result = next_;
        next_ = next_ == std::numeric_limits<std::uint64_t>::max()
            ? 0 : next_ + 1;
        return result;
    }

private:
    std::uint64_t next_{1};
};

class IExecutionAdapter
{
public:
    virtual ~IExecutionAdapter() = default;
    virtual void submit_order(const order_event& o) = 0;
    virtual bool poll_fills(std::vector<fill_event>& out) = 0;

    // Live execution ingress may not destructively drain a batch before the
    // engine has committed each economic fill. Transactional adapters expose
    // a non-destructive front item and remove it only after explicit ACK from
    // the canonical FillProcessor. Legacy simulation adapters retain the
    // bulk poll contract; live mode refuses those at the canonical boundary.
    virtual bool supports_transactional_fill_delivery() const noexcept
    {
        return false;
    }
    virtual bool peek_fill(fill_event& /*out*/) { return false; }
    virtual bool acknowledge_fill(std::uint64_t /*fill_id*/) { return false; }

    // Cold-path composition hook. Leaf adapters that synthesize fills replace
    // their private sequence with the composite-owned causal namespace.
    virtual void set_fill_id_sequence(
        std::shared_ptr<FillIdSequence> /*sequence*/) {}

    virtual bool cancel_order(uint64_t order_id) { (void)order_id; return false; }
    virtual bool modify_order(uint64_t order_id, double new_price, double new_qty)
    {
        (void)order_id; (void)new_price; (void)new_qty; return false;
    }

    // Shadow: real trade prints fed so the adapter can match open orders
    // against the tape. Default no-op for backtest/live.
    virtual void on_trade(const std::string& /*symbol*/,
                          double /*trade_price*/,
                          double /*trade_qty*/,
                          std::chrono::system_clock::time_point /*trade_ts*/) {}

    // A sell aggressor consumes bids only; a buy aggressor consumes asks
    // only. Strict recorded replay may reject an unknown aggressor rather
    // than inventing fills on both sides of the book.
    virtual void on_trade(const std::string& symbol,
                          double trade_price,
                          double trade_qty,
                          std::optional<order_side> aggressor_side,
                          std::chrono::system_clock::time_point trade_ts)
    {
        (void)aggressor_side;
        on_trade(symbol, trade_price, trade_qty, trade_ts);
    }

    // Drains time-sensitive state (e.g. cancel-in-flight windows).
    virtual void advance_time(std::chrono::system_clock::time_point /*ts*/) {}

    virtual void on_l2_snapshot(
        const std::string& /*symbol*/,
        const std::vector<std::pair<double, double>>& /*bids*/,
        const std::vector<std::pair<double, double>>& /*asks*/) {}

    virtual void on_l2_snapshot(
        const std::string& symbol,
        const std::vector<std::pair<double, double>>& bids,
        const std::vector<std::pair<double, double>>& asks,
        std::chrono::system_clock::time_point /*event_ts*/)
    {
        on_l2_snapshot(symbol, bids, asks);
    }

    virtual void on_l2_update(
        const std::string& /*symbol*/,
        order_side /*side*/,
        double /*price*/,
        double /*new_size*/) {}

    virtual void on_l2_update(
        const std::string& symbol,
        order_side side,
        double price,
        double new_size,
        std::chrono::system_clock::time_point /*event_ts*/)
    {
        on_l2_update(symbol, side, price, new_size);
    }

    // Optional hook for adapters that maintain a seeded book (LocalBookAdapter
    // and QueueAwareBookAdapter). Default no-op.
    virtual void set_mid_price(double /*price*/) {}

    // Returns 0 from adapters that don't model queue position; dashboard
    // renders em-dash. QueueAwareBookAdapter overrides.
    virtual std::size_t   live_quote_count()         const { return 0; }
    virtual std::uint32_t avg_queue_position_bps()   const { return 0; }

    // Rich queue modeling stats (primarily from TradeTapeShadowAdapter for
    // shadow --queue-model l2-snapshot). Used for dashboard and divergence
    // reporting. Default 0 for adapters that don't track.
    virtual std::size_t queue_submitted_with_queue() const { return 0; }
    virtual std::size_t queue_filled_after_drain()   const { return 0; }
    virtual std::size_t queue_blocked_at_eos()       const { return 0; }

    // --- Capability / adapter kind queries (added to eliminate ad-hoc
    // dynamic_cast<ConcreteAdapter*> proliferation in engine/router).
    // All have cheap default implementations.

    // True for live ExecutionBridge adapters that perform async submit/cancel
    // over the wire and report results via poll_submit_results + synth meta.
    virtual bool supports_async_submit() const { return false; }

    // Optional hook for LocalBookAdapter (and QueueAware in hybrid) when the
    // symbol carries real L2 depth from the venue (shadow mode). Default no-op.
    // Used to suppress bar-spread adjustments and mark seeded state.
    virtual void set_l2_seeded(bool /*seeded*/) {}

    // Last transient error string. Used by dashboard for the "bridge" row.
    // Bridge overrides to surface transport / rate-limiter / submit errors.
    // Return by const ref to be compatible with existing implementations
    // (e.g. BinanceExecutor).
    virtual const std::string& last_error() const { static const std::string empty{}; return empty; }

    // Capability query returning the narrow async support interface when
    // present. Preferred over dynamic_cast<IAsyncSubmitSupport*>.
    // Callers: engine ctor wiring for unknown-fill handler, drain paths.
    virtual IAsyncSubmitSupport* get_async_support() { return nullptr; }

    // External book trades (e.g. MarketMaker re-quotes) that may cross
    // resting strategy limits. LocalBookAdapter records maker fills;
    // HybridExecutor forwards to its inner book adapter. Live bridges
    // and tape-only adapters default to no-op (not a live submit path).
    virtual void on_book_trades(const trades& /*trs*/,
                                std::chrono::system_clock::time_point /*ts*/) {}

    // Bar-mode traversal: fill resting limits whose price lies inside
    // [low, high]. Returns true if any fill was produced (caller should
    // poll_fills). Default false — paper LocalBookAdapter / Hybrid only.
    // bar_volume > 0 caps aggregate fill quantity across orders (submission
    // FIFO for orders at the same price level); <=0 fills full remainders.
    virtual bool sweep_resting_range(const std::string& /*symbol*/,
                                     double /*low*/, double /*high*/,
                                     std::chrono::system_clock::time_point /*ts*/,
                                     double /*bar_volume*/ = 0.0)
    {
        return false;
    }

    // Qty filled by the most recent successful sweep_resting_range (0 if none).
    // Hybrid uses this to subtract local consumption before queue sweep.
    virtual double last_sweep_fill_qty() const { return 0.0; }
};

class LocalBookAdapter : public IExecutionAdapter
{
public:
    LocalBookAdapter(std::shared_ptr<orderbook> ob,
                     std::shared_ptr<IFeeModel> fee_model,
                     std::shared_ptr<IFillModel> fill_model,
                     std::uint64_t rng_seed = 42,
                     double market_aggression = 1.1,
                     double qty_scale = 1e8,
                     std::shared_ptr<ILatencyModel> latency_model = nullptr,
                     std::shared_ptr<IImpactModel>  impact_model  = nullptr,
                     bool walked_book_impact = false)
        : ob_(std::move(ob))
        , fee_model_(std::move(fee_model))
        , fill_model_(std::move(fill_model))
        , fill_rng_(rng_seed)
        , market_aggression_(market_aggression)
        , qty_scale_(qty_scale)
        , latency_model_(std::move(latency_model))
        , impact_model_(std::move(impact_model))
        , walked_book_impact_(walked_book_impact) {}

    LocalBookAdapter(const LocalBookAdapter&) = delete;
    LocalBookAdapter& operator=(const LocalBookAdapter&) = delete;
    LocalBookAdapter(LocalBookAdapter&&) = delete;
    LocalBookAdapter& operator=(LocalBookAdapter&&) = delete;

    void set_mid_price(double price) override { mid_price_ = price; }
    void set_fill_id_sequence(
        std::shared_ptr<FillIdSequence> sequence) override
    {
        if (sequence)
            fill_ids_ = std::move(sequence);
    }
    // Symbol carries real L2 depth — enables the walked-book VWAP
    // reference for market orders (walked_book_impact).
    void set_l2_seeded(bool seeded) override { l2_seeded_ = seeded; }

    void set_debug_fills(bool enabled, int budget = 20)
    {
        debug_fills_ = enabled;
        debug_fills_left_ = budget;
    }

    void submit_order(const order_event& o) override
    {
        submit_order_impl(o, false);
    }

    // Hybrid taker path: the shared book may also contain locally resting
    // strategy orders. Only venue/synthetic external depth is eligible as a
    // counterparty, so a strategy can never trade with itself here.
    void submit_order_against_external(const order_event& o)
    {
        submit_order_impl(o, true);
    }

    double remaining_quantity_base(std::uint64_t order_id) const noexcept
    {
        const auto body = ob_ ? ob_->get_order(order_id) : nullptr;
        if (!body || !(qty_scale_ > 0.0))
            return 0.0;
        return static_cast<double>(body->get_remaining_quantity())
            / qty_scale_;
    }

private:
    void submit_order_impl(const order_event& o, bool external_only)
    {
        if (o.get_order_type() == order_type::stop || o.get_order_type() == order_type::stop_limit)
            return;

        double fill_probability = 1.0;
        if (fill_model_ && o.get_order_type() == order_type::limit && mid_price_ > 0.0)
        {
            double distance = std::abs(o.get_price() - mid_price_) / mid_price_;
            fill_probability = fill_model_->get_fill_probability(o.get_side(), distance);
            if (fill_rng_.uniform_unit() > fill_probability)
                return;
        }

        ob_order_type book_order_type;
        switch (o.get_tif()) {
        case time_in_force::fok: book_order_type = ob_order_type::fill_or_kill; break;
        case time_in_force::ioc: book_order_type = ob_order_type::immediate_or_cancel; break;
        default:                 book_order_type = ob_order_type::good_till_cancel; break;
        }

        side book_side = (o.get_side() == order_side::buy) ? side::buy : side::sell;

        Price book_price;
        double raw_reference_price = o.get_price();
        double reference_price = o.get_price();
        if (o.get_order_type() == order_type::market)
        {
            double ref_price = (mid_price_ > 0.0) ? mid_price_ : o.get_price();
            raw_reference_price = ref_price;
            bool walked_used = false;

            // Walked-book impact: when L2 depth is real, the actual VWAP
            // of the levels we'd consume IS the honest reference price.
            // Suppresses bar-spread (already does on l2_seeded_) AND the
            // parametric impact model - the walk doesn't compose with a
            // square-root guess on top of the same depth.
            if (walked_book_impact_ && l2_seeded_)
            {
                const quantity requested = static_cast<quantity>(
                    std::round(o.get_quantity() * qty_scale_));
                const double vwap = external_only
                    ? ob_->external_vwap(book_side, requested)
                    : walked_book_vwap(o.get_side(), o.get_quantity());
                if (vwap > 0.0) {
                    ref_price = vwap;
                    walked_used = true;
                }
                // vwap == 0 -> insufficient depth, fall through to mid +
                // impact_model. Underpricing impact for a sweep is worse
                // than admitting the parametric estimate.
            }

            if (!walked_used)
            {
                // Impact BEFORE aggression so aggression still guarantees an
                // immediate cross. ZeroImpactModel (default) is a pass-through.
                if (impact_model_)
                    ref_price = impact_model_->effective_price(o.get_side(),
                                                               o.get_quantity(),
                                                               ref_price);
            }

            reference_price = ref_price;

            book_price = (book_side == side::buy) ? Price::from_double(ref_price * market_aggression_)
                                                  : Price::from_double(ref_price * (2.0 - market_aggression_));
        }
        else
            book_price = Price::from_double(o.get_price());

        // Apply fill-fade BEFORE matching so book and portfolio qty stay aligned.
        // Post-match shrink left the book fully consumed while portfolio understated.
        double match_qty = o.get_quantity();
        const double fade_rate = fill_model_ ? fill_model_->get_fade_rate() : 0.0;
        if (fade_rate > 0.0)
        {
            match_qty *= (1.0 - fade_rate);
            if (!(match_qty > 0.0))
                return;
        }

        quantity book_quantity = static_cast<quantity>(std::round(match_qty * qty_scale_));
        if (book_quantity <= 0)
            return;

        fill_provenance provenance;
        provenance.model = l2_seeded_
            ? fill_execution_model::l2_local_book
            : fill_execution_model::synthetic_local_liquidity;
        provenance.reason = fill_execution_reason::aggressive_ladder_match;
        provenance.exploratory = true;
        provenance.intended_price = o.get_price();
        provenance.reference_price = reference_price;
        provenance.reference_timestamp = o.get_earliest_eligible_ts();
        provenance.fill_probability = fill_probability;
        provenance.modeled_impact_bps = signed_bps(
            o.get_side(), raw_reference_price, reference_price);
        const auto decision_ts = o.get_decision_ts();
        if (provenance.reference_timestamp > decision_ts)
            provenance.modeled_latency = std::chrono::duration_cast<std::chrono::nanoseconds>(
                provenance.reference_timestamp - decision_ts);

        // Prefer the orderbook object pool (prewarmed + forbid_runtime_grow)
        // over a freestanding heap allocation on every paper submit.
        auto book_order = ob_->create_order(
            book_order_type, o.get_order_id(), book_side, book_price, book_quantity);

        double pre_bid = 0.0, pre_ask = 0.0;
        if (debug_fills_ && debug_fills_left_ > 0)
        {
            auto infos = ob_->get_order_infos();
            if (!infos.get_bids().empty())
                pre_bid = infos.get_bids().front().price_.to_double();
            if (!infos.get_asks().empty())
                pre_ask = infos.get_asks().front().price_.to_double();
        }

        trades resulting_trades = external_only
            ? ob_->add_order_against_external(book_order)
            : ob_->add_order(book_order);

        // GTC remainder rests on the book. Track it so later quote
        // updates that cross its level (delivered via on_book_trades)
        // surface as maker fills — without this, resting limits never
        // fill against the synthetic book.
        if (book_order_type == ob_order_type::good_till_cancel &&
            book_order->get_remaining_quantity() > 0)
        {
            track_resting(o.get_order_id(), book_order,
                          o.get_symbol(), o.get_side(), provenance);
        }

        double remaining_after_leg =
            static_cast<double>(book_order->get_remaining_quantity()) / qty_scale_;
        for (const auto& trade : resulting_trades)
        {
            const bool we_are_bid = (trade.get_bid_trade().orderId_ == o.get_order_id());
            const auto& our_trade_info = we_are_bid ? trade.get_bid_trade() : trade.get_ask_trade();
            if (our_trade_info.orderId_ == o.get_order_id())
                remaining_after_leg +=
                    static_cast<double>(our_trade_info.quantity_) / qty_scale_;
        }

        for (const auto& trade : resulting_trades)
        {
            const bool we_are_bid = (trade.get_bid_trade().orderId_ == o.get_order_id());
            const auto& our_trade_info  = we_are_bid ? trade.get_bid_trade() : trade.get_ask_trade();
            const auto& counter_trade   = we_are_bid ? trade.get_ask_trade() : trade.get_bid_trade();

            // The resting counterparty may itself be a tracked strategy
            // order (strategy-vs-strategy crossing) — surface its maker
            // fill too, not just the aggressor's.
            if (counter_trade.orderId_ != o.get_order_id())
                record_resting_fill(counter_trade, o.get_earliest_eligible_ts(),
                                    fill_execution_reason::aggressive_ladder_match);

            if (our_trade_info.orderId_ == o.get_order_id())
            {
                // Fill at the resting counterparty's price — honest
                // passive-side pricing, one event per walked level. The
                // aggressor's book price (mid × aggression for market) is
                // only a crossing limit, never a recorded fill price.
                double fill_price = counter_trade.price_.to_double();
                double fill_qty = static_cast<double>(our_trade_info.quantity_) / qty_scale_;

                double commission = 0.0;
                if (fee_model_)
                {
                    bool is_taker = (o.get_order_type() == order_type::market) ||
                        (o.get_side() == order_side::buy && mid_price_ > 0.0 && o.get_price() >= mid_price_) ||
                        (o.get_side() == order_side::sell && mid_price_ > 0.0 && o.get_price() <= mid_price_);
                    commission = fee_model_->compute_commission(o.get_side(), fill_qty, fill_price, is_taker);
                }

                remaining_after_leg -= fill_qty;
                const double remaining = std::max(0.0, remaining_after_leg);

                auto fill_provenance = provenance;
                fill_provenance.modeled_spread_bps = signed_bps(
                    o.get_side(), reference_price, fill_price);

                pending_fills_.emplace_back(
                    o.get_earliest_eligible_ts(),
                    o.get_symbol(),
                    o.get_order_id(),
                    o.get_side(),
                    fill_qty,
                    fill_price,
                    commission,
                    remaining,
                    fill_ids_->next()
                );
                pending_fills_.back().set_source(fill_source::simulated);
                pending_fills_.back().set_provenance(fill_provenance);
                pending_fills_.back().set_recv_ns(o.get_recv_ns());
                if (o.get_recv_ns() > 0)
                {
                    int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count();
                    pending_fills_.back().set_latency_ns(now_ns - o.get_recv_ns());
                }

                if (debug_fills_ && debug_fills_left_ > 0)
                {
                    const char* side_str = (o.get_side() == order_side::buy) ? "BUY " : "SELL";
                    std::fprintf(stderr,
                        "[debug-fills] id=%llu %s intended=%.4f book=%.4f fill=%.4f "
                        "bid=%.4f ask=%.4f mid=%.4f qty=%.6f\n",
                        (unsigned long long)o.get_order_id(),
                        side_str,
                        o.get_price(),
                        book_price.to_double(),
                        fill_price,
                        pre_bid,
                        pre_ask,
                        mid_price_,
                        fill_qty);
                    --debug_fills_left_;
                }
            }
        }
    }

public:
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
        // Only succeed when we actually hold the order (resting or cancel
        // already in flight). Unknown ids must return false so Hybrid does
        // not treat "local always true" as a successful cancel.
        auto resting_it = resting_.find(order_id);
        const bool known = resting_it != resting_.end()
                        || pending_cancels_.count(order_id) > 0;
        if (!known)
            return false;

        // Models "too slow to pull": cancel starts an in-flight window
        // and the order keeps matching until advance_time() drains it.
        if (latency_model_)
        {
            const auto lat = latency_model_->get_cancel_latency();
            pending_cancels_[order_id] = current_time_ + lat;
            return true;
        }
        ob_->cancel_order(order_id);
        if (resting_it != resting_.end())
            erase_resting(resting_it);
        return true;
    }

    bool modify_order(uint64_t order_id, double new_price, double new_qty) override
    {
        // Transactional amend (HIGH-01 memory-check):
        // 1) Unknown id → fail closed without touching the book.
        // 2) Cancel already in flight → fail closed too: the order still sits
        //    in resting_ until advance_time() drains the cancel window, but
        //    amending it now would be silently negated when that cancel fires.
        // 3) Keep resting_ until book modify succeeds (erase-before-commit
        //    dropped tracking on book failure → silent missed fills).
        // 4) On success re-bind to the live body (book cancel+recreates).
        auto rit = resting_.find(order_id);
        const bool in_resting = rit != resting_.end();
        if (!in_resting && pending_cancels_.count(order_id) == 0)
            return false;
        if (pending_cancels_.count(order_id) > 0)
            return false;

        std::string saved_symbol;
        order_side saved_side = order_side::buy;
        if (in_resting)
        {
            saved_symbol = rit->second.symbol;
            saved_side = rit->second.side;
        }

        Price book_price = Price::from_double(new_price);
        quantity book_qty = static_cast<quantity>(std::round(new_qty * qty_scale_));
        if (!ob_->modify_order(order_id, book_price, book_qty))
            return false; // resting_ still points at pre-modify body if present

        // Book replace: pre-modify order_pointer is semantically stale — rebind.
        if (in_resting)
        {
            if (auto body = ob_->get_order(order_id))
            {
                if (body->get_remaining_quantity() > 0)
                {
                    rit->second.book_order = body;
                    rit->second.symbol = std::move(saved_symbol);
                    rit->second.side = saved_side;
                    move_resting_to_back(&rit->second);
                }
                else
                    erase_resting(rit);
            }
            else
            {
                erase_resting(rit);
            }
        }
        return true;
    }

    void advance_time(std::chrono::system_clock::time_point ts) override
    {
        current_time_ = ts;
        for (auto it = pending_cancels_.begin(); it != pending_cancels_.end(); )
        {
            if (it->second <= ts)
            {
                ob_->cancel_order(it->first);
                auto resting_it = resting_.find(it->first);
                if (resting_it != resting_.end())
                    erase_resting(resting_it);
                it = pending_cancels_.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    // Trades produced by external book activity (MarketMaker quote
    // updates crossing our resting orders). Each trade whose maker side
    // is a tracked resting order becomes a maker fill at that order's
    // own limit price.
    void on_book_trades(const trades& trs,
                        std::chrono::system_clock::time_point ts) override
    {
        for (const auto& tr : trs)
        {
            record_resting_fill(tr.get_bid_trade(), ts,
                                fill_execution_reason::market_maker_requote);
            record_resting_fill(tr.get_ask_trade(), ts,
                                fill_execution_reason::market_maker_requote);
        }
    }

    // Bar-mode traversal fills: a resting limit whose level lies inside the
    // bar's [low, high] range was traded through intrabar even when the MM
    // re-quote anchors (open/close/stop refs) never crossed it — without
    // this, a buy limit at 99 misses a bar with low 98 / close 101 entirely.
    // Fills at the order's own limit price (maker). When bar_volume > 0,
    // aggregate fill qty across orders is capped by volume (residual stays
    // resting). Returns true if anything filled.
    bool sweep_resting_range(const std::string& symbol,
                             double low, double high,
                             std::chrono::system_clock::time_point ts,
                             double bar_volume = 0.0) override
    {
        last_sweep_fill_qty_ = 0.0;
        if (resting_.empty() || !(low > 0.0) || !(high > 0.0))
            return false;
        if (low > high) std::swap(low, high);

        double volume_left = (bar_volume > 0.0) ? bar_volume
                                                : std::numeric_limits<double>::infinity();
        bool any = false;
        resting_info* current = resting_fifo_head_;
        while (current != nullptr)
        {
            if (!(volume_left > 0.0)) break;

            resting_info* next = current->fifo_next;
            const uint64_t order_id = current->order_id;
            if (current->symbol != symbol)
            {
                current = next;
                continue;
            }
            // Skip orders with a cancel in flight — "too slow to pull"
            // is modeled at the crossing paths, but a traversal fill
            // during the cancel window would be generous, not adverse.
            if (pending_cancels_.count(order_id))
            {
                current = next;
                continue;
            }

            // Always use live book body for remaining qty/price (HIGH-02).
            if (auto live = ob_->get_order(order_id))
                current->book_order = live;
            else
            {
                erase_resting(current);
                current = next;
                continue;
            }

            const double px = current->book_order->get_price().to_double();
            const bool traversed = (current->side == order_side::buy)
                ? (low <= px) : (high >= px);
            if (!traversed)
            {
                current = next;
                continue;
            }

            const double remaining =
                static_cast<double>(current->book_order->get_remaining_quantity())
                / qty_scale_;
            if (remaining <= 0.0)
            {
                erase_resting(current);
                current = next;
                continue;
            }

            const double fill_qty = std::min(remaining, volume_left);
            if (!(fill_qty > 0.0))
            {
                current = next;
                continue;
            }

            double commission = 0.0;
            if (fee_model_)
                commission = fee_model_->compute_commission(
                    current->side, fill_qty, px, /*is_taker=*/false);

            const double rem_after = remaining - fill_qty;
            auto provenance = current->provenance;
            provenance.reason = fill_execution_reason::bar_range_sweep;
            provenance.reference_timestamp = ts;
            provenance.reference_price = px;
            provenance.modeled_spread_bps = 0.0;
            pending_fills_.emplace_back(
                ts, current->symbol, order_id, current->side,
                fill_qty, px, commission,
                rem_after, fill_ids_->next());
            pending_fills_.back().set_source(fill_source::simulated);
            pending_fills_.back().set_provenance(provenance);
            any = true;
            volume_left -= fill_qty;
            last_sweep_fill_qty_ += fill_qty;

            if (rem_after <= 1e-12)
            {
                ob_->cancel_order(order_id);
                erase_resting(current);
            }
            else
            {
                // Partial: shrink book qty; rebind live body after recreate.
                const quantity new_q = static_cast<quantity>(
                    std::round(rem_after * qty_scale_));
                if (new_q > 0)
                    ob_->modify_order(order_id,
                        Price::from_double(px), new_q);
                if (auto body = ob_->get_order(order_id))
                    current->book_order = body;
                else
                {
                    erase_resting(current);
                }
            }
            current = next;
        }
        return any;
    }

    double last_sweep_fill_qty() const override { return last_sweep_fill_qty_; }

private:
    static double signed_bps(order_side side,
                             double reference_price,
                             double observed_price) noexcept
    {
        if (!(reference_price > 0.0) || !(observed_price > 0.0))
            return 0.0;
        const double raw_bps = (observed_price / reference_price - 1.0) * 1.0e4;
        return side == order_side::buy ? raw_bps : -raw_bps;
    }

    std::shared_ptr<orderbook> ob_;
    std::shared_ptr<IFeeModel> fee_model_;
    std::shared_ptr<IFillModel> fill_model_;
    std::vector<fill_event> pending_fills_;
    truetest::reproducibility::DeterministicRng fill_rng_;
    double mid_price_ = 0.0;
    double market_aggression_ = 1.1;
    double qty_scale_ = 1e8;
    std::shared_ptr<FillIdSequence> fill_ids_ =
        std::make_shared<FillIdSequence>();
    bool debug_fills_ = false;
    int debug_fills_left_ = 0;
    double last_sweep_fill_qty_ = 0.0;
    // Volume-weighted average price for walking `qty` through the
    // passive side of the book. Returns 0 when the book has fewer
    // resting units than requested - the caller falls back to its
    // parametric path because partial-walk VWAP systematically
    // understates impact for sweeps. Cost: a single
    // ob_->get_order_infos() snapshot per call; only invoked from
    // submit_order, not from the market-data hot path.
    double walked_book_vwap(order_side side, double qty) const
    {
        const auto infos = ob_->get_order_infos();
        const auto& levels = (side == order_side::buy)
            ? infos.get_asks() : infos.get_bids();
        double remaining = qty * qty_scale_;
        double cost = 0.0, consumed = 0.0;
        for (const auto& lvl : levels) {
            if (remaining <= 0.0) break;
            const double take = std::min(remaining,
                static_cast<double>(lvl.quantity_));
            cost     += take * lvl.price_.to_double();
            consumed += take;
            remaining -= take;
        }
        if (remaining > 0.0 || consumed <= 0.0) return 0.0;
        return cost / consumed;
    }

    struct resting_info
    {
        order_pointer book_order;   // live book object; remaining qty stays current
        std::string symbol;
        order_side side = order_side::buy;
        uint64_t order_id = 0;
        fill_provenance provenance{};
        resting_info* fifo_prev = nullptr;
        resting_info* fifo_next = nullptr;
    };

    using resting_map = std::unordered_map<uint64_t, resting_info>;

    void link_resting_back(resting_info* info)
    {
        info->fifo_prev = resting_fifo_tail_;
        info->fifo_next = nullptr;
        if (resting_fifo_tail_ != nullptr)
            resting_fifo_tail_->fifo_next = info;
        else
            resting_fifo_head_ = info;
        resting_fifo_tail_ = info;
    }

    void unlink_resting(resting_info* info)
    {
        if (info->fifo_prev != nullptr)
            info->fifo_prev->fifo_next = info->fifo_next;
        else
            resting_fifo_head_ = info->fifo_next;

        if (info->fifo_next != nullptr)
            info->fifo_next->fifo_prev = info->fifo_prev;
        else
            resting_fifo_tail_ = info->fifo_prev;

        info->fifo_prev = nullptr;
        info->fifo_next = nullptr;
    }

    void move_resting_to_back(resting_info* info)
    {
        if (resting_fifo_tail_ == info) return;
        unlink_resting(info);
        link_resting_back(info);
    }

    void track_resting(uint64_t order_id,
                       order_pointer book_order,
                       const std::string& symbol,
                       order_side side,
                       const fill_provenance& provenance)
    {
        resting_info replacement{
            std::move(book_order), symbol, side, order_id, provenance, nullptr, nullptr};
        auto [it, inserted] = resting_.try_emplace(
            order_id, std::move(replacement));
        if (inserted)
        {
            link_resting_back(&it->second);
            return;
        }

        it->second.book_order = std::move(replacement.book_order);
        it->second.symbol = std::move(replacement.symbol);
        it->second.side = side;
        it->second.provenance = provenance;
        move_resting_to_back(&it->second);
    }

    void erase_resting(resting_map::iterator it)
    {
        unlink_resting(&it->second);
        resting_.erase(it);
    }

    void erase_resting(resting_info* info)
    {
        auto it = resting_.find(info->order_id);
        if (it != resting_.end() && &it->second == info)
            erase_resting(it);
    }

    void record_resting_fill(const trade_info& ti,
                             std::chrono::system_clock::time_point ts,
                             fill_execution_reason reason)
    {
        auto it = resting_.find(ti.orderId_);
        if (it == resting_.end())
            return;

        const double fill_qty = static_cast<double>(ti.quantity_) / qty_scale_;
        const double fill_price = ti.price_.to_double();  // our own limit price
        double commission = 0.0;
        if (fee_model_)
            commission = fee_model_->compute_commission(
                it->second.side, fill_qty, fill_price, /*is_taker=*/false);

        const double remaining =
            static_cast<double>(it->second.book_order->get_remaining_quantity())
            / qty_scale_;

        pending_fills_.emplace_back(
            ts,
            it->second.symbol,
            ti.orderId_,
            it->second.side,
            fill_qty,
            fill_price,
            commission,
            remaining,
            fill_ids_->next());
        auto provenance = it->second.provenance;
        provenance.reason = reason;
        provenance.reference_timestamp = ts;
        provenance.reference_price = fill_price;
        provenance.modeled_spread_bps = 0.0;
        pending_fills_.back().set_source(fill_source::simulated);
        pending_fills_.back().set_provenance(provenance);

        if (it->second.book_order->is_filled())
            erase_resting(it);
    }

    resting_map resting_;
    resting_info* resting_fifo_head_ = nullptr;
    resting_info* resting_fifo_tail_ = nullptr;

    std::shared_ptr<ILatencyModel> latency_model_;
    std::shared_ptr<IImpactModel>  impact_model_;
    bool l2_seeded_ = false;
    bool walked_book_impact_ = false;
    std::unordered_map<uint64_t, std::chrono::system_clock::time_point> pending_cancels_;
    std::chrono::system_clock::time_point current_time_{};
};

class ExchangeAdapter : public IExecutionAdapter
{
public:
    void submit_order(const order_event& /*o*/) override {}
    bool poll_fills(std::vector<fill_event>& /*out*/) override { return false; }
};
