#pragma once

#include "execution_adapter.h"
#include "live_safety.h"
#include "order_transport.h"
#include "fill_transport.h"
#include "order_encoder.h"
#include "fill_parser.h"
#include "rate_limiter.h"
#include "async_support.h"
#include "../core/event.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef __x86_64__
#include <immintrin.h>
#endif

class ExecutionBridge : public IExecutionAdapter, public IAsyncSubmitSupport
{
public:
    // Source-compat aliases (transition only). New code should use the
    // top-level names from async_support.h or go through IAsyncSubmitSupport.
    using submit_result = ::submit_result;
    using synth_meta    = ::synth_meta;
    using synth_result  = ::synth_result;
    using unknown_fill_handler = ::unknown_fill_handler;

    struct deps
    {
        std::shared_ptr<IOrderTransport> order_tx;
        std::shared_ptr<IFillTransport>  fill_tx;
        std::shared_ptr<IOrderEncoder>   encoder;
        std::shared_ptr<IFillParser>     parser;

        // Immutable cold-path proof installed only after central startup
        // validation. Every bridge is a private-write adapter, so the public
        // default-invalid value refuses before transports or admission open.
        WriteSafetyReadiness write_safety_readiness;

        // Optional. If set, each submit consults it before sending and
        // blocks until a token is available - gates the venue's order-rate
        // cap (Binance spot: 50 orders / 10s). Left null, submit is ungated.
        std::shared_ptr<TokenBucketRateLimiter> order_rate_limiter;

        // Optional. Called per submit to produce the clientOrderId used for
        // exchange-side idempotency and as the bridge's internal key. When
        // null, the bridge falls back to "tt-<engine_order_id>", which is
        // only unique within a single process lifetime.
        std::function<std::string(uint64_t engine_order_id)> client_id_fn;

        // Optional. Fires when the parser declines the message via parse()
        // but accepts it via parse_position_snapshot() - i.e. server-pushed
        // position/balance state changes (futures ACCOUNT_UPDATE). Runs on
        // the fill transport's worker thread; must be thread-safe.
        std::function<void(const parsed_position_snapshot&)>
            position_snapshot_handler;

        // Funding/accounting decisions are parsed before the allocating
        // diagnostic snapshot path. Both callbacks run on the private fill
        // reader thread. The update callback may only enqueue into a
        // provider-owned SPSC ingress; false or any exception is terminal.
        std::function<bool(const parsed_funding_update&)>
            funding_update_handler;
        std::function<void()> funding_failure_handler;

        bool start_transport_thread = true;

        // Cold-start capacities for execution ingress.  Both are hard bounds:
        // exhaustion latches terminal admission instead of allocating/growing
        // silently on the private execution hot path.
        std::size_t fill_ingress_capacity = 4096;
        std::size_t execution_dedupe_capacity = 16384;
    };

    struct status_event
    {
        IFillTransport::lifecycle state = IFillTransport::lifecycle::closed;
        std::string note;
    };

    // Hook for venue-managed orders that we never submitted ourselves
    // (e.g. Binance OCO legs placed by BinanceOcoBracketAdapter). The
    // bridge's by_client_id_ lookup misses for these. If a handler is
    // installed, the bridge invokes it and, if it returns a synth_result,
    // enqueues the fill alongside normal fills AND records the
    // (engine_order_id -> opener_order_id + strategy) metadata for the
    // engine to register in its order_meta_ on the main thread.
    // Handler runs on the fill transport's worker thread -> must be
    // thread-safe. ExitManager's venue lookups are mutex-guarded; the
    // engine-side OrderIdGenerator::next() is atomic.

    // Types and handler type are now defined in async_support.h (top level)
    // for narrow capability consumption without depending on ExecutionBridge.
    // The using declarations below provide source compatibility for code that
    // still spelled ExecutionBridge::synth_result etc. during the transition.

    // IAsyncSubmitSupport overrides (also satisfy direct calls on concrete bridge).
    void set_unknown_fill_handler(unknown_fill_handler h) override
    {
        std::lock_guard<std::mutex> lk(handler_mu_);
        unknown_fill_handler_ = std::move(h);
    }

    void clear_unknown_fill_handler() override
    {
        std::lock_guard<std::mutex> lk(handler_mu_);
        unknown_fill_handler_ = {};
    }

    // Engine drains this BEFORE poll_fills so order_meta_ has the
    // mapping ready when lookup_opener fires inside the fill loop.
    bool poll_synth_meta(std::vector<synth_meta>& out) override
    {
        std::lock_guard<std::mutex> lk(synth_mu_);
        if (pending_synth_meta_.empty()) return false;
        out.insert(out.end(),
                   std::make_move_iterator(pending_synth_meta_.begin()),
                   std::make_move_iterator(pending_synth_meta_.end()));
        pending_synth_meta_.clear();
        return true;
    }

    ~ExecutionBridge()
    {
        close();  // ensure transport thread joined before destroying rings/mutexes/d_
    }

    explicit ExecutionBridge(deps d)
        : d_(std::move(d))
    {
        pending_fills_.reserve(d_.fill_ingress_capacity);
        execution_dedupe_.resize(d_.execution_dedupe_capacity);
        if (d_.fill_tx)
        {
            d_.fill_tx->set_on_message([this](std::string_view raw) {
                handle_message(raw);
            });
            d_.fill_tx->set_on_status([this](IFillTransport::lifecycle st,
                                             std::string_view note) {
                handle_status(st, note);
            });
        }
    }

    bool open()
    {
        // Establish a closed admission baseline before either transport may
        // synchronously invoke callbacks. A fatal callback during open must
        // remain latched and can never be erased by startup completion.
        ingress_failure_latched_.store(false, std::memory_order_release);
        accepting_orders_.store(false, std::memory_order_release);
        if (!d_.write_safety_readiness.permits_private_exchange_writes())
        {
            set_error(
                "ExecutionBridge: write safety readiness is not validated");
            return false;
        }
        if (!d_.order_tx || !d_.fill_tx)
        {
            set_error("ExecutionBridge: missing transport");
            return false;
        }
        if (!d_.order_tx->open())
        {
            set_error("ExecutionBridge: order transport open failed");
            return false;
        }
        if (!d_.fill_tx->open())
        {
            d_.order_tx->close();
            set_error("ExecutionBridge: fill transport open failed");
            return false;
        }

        if (ingress_failure_latched_.load(std::memory_order_acquire))
        {
            d_.fill_tx->close();
            d_.order_tx->close();
            return false;
        }
        accepting_orders_.store(true, std::memory_order_release);
        // Start background transport thread (only for live order paths).
        // The thread owns all actual calls to order_tx.
        if (d_.start_transport_thread)
        {
            transport_running_.store(true, std::memory_order_release);
            transport_thread_ = std::thread([this] { transport_loop(); });
        }

        return true;
    }

    void close()
    {
        quiesce();
        if (transport_thread_.joinable())
            transport_thread_.join();

        if (d_.fill_tx)  d_.fill_tx->close();
        if (d_.order_tx) d_.order_tx->close();
    }

    void quiesce()
    {
        accepting_orders_.store(false, std::memory_order_release);
        transport_running_.store(false, std::memory_order_release);
        // Serialise the admission check with the actual venue call. Without
        // this gate, a worker could observe accepting=true, then start a
        // submit after quiesce() had returned. Shutdown waits only for an
        // already-admitted mutation; it never permits a new one.
        std::lock_guard<std::mutex> admission_lock(venue_admission_mu_);
        if (d_.order_tx) d_.order_tx->quiesce();
        {
            std::lock_guard<std::mutex> lk(submit_queue_mu_);
            submit_queue_.clear();
        }
        clear_unknown_fill_handler();
    }

    void submit_order(const order_event& o) override
    {
        clear_error();

        if (!d_.write_safety_readiness.permits_private_exchange_writes())
        {
            set_error(
                "ExecutionBridge: write safety readiness is not validated");
            return;
        }

        if (!accepting_orders_.load(std::memory_order_acquire))
        {
            set_error("ExecutionBridge: live submission is quiesced");
            return;
        }

        if (!d_.encoder || !d_.order_tx)
        {
            set_error("ExecutionBridge: not configured");
            return;
        }

        const std::string client_id = d_.client_id_fn
            ? d_.client_id_fn(o.get_order_id())
            : make_client_id(o.get_order_id());
        auto enc = d_.encoder->encode_submit(o, client_id);

        tracked_order t;
        t.engine_id = o.get_order_id();
        t.client_id = client_id;
        t.symbol    = o.get_symbol();
        t.side      = o.get_side();
        t.total_qty = o.get_quantity();
        t.intended_price = o.get_price();
        t.decision_ts = o.get_decision_ts();

        {
            std::lock_guard<std::mutex> lk(map_mu_);
            by_engine_id_[t.engine_id] = t;
            by_client_id_[t.client_id] = t.engine_id;
        }

        submit_request req;
        req.engine_id     = o.get_order_id();
        req.client_id     = client_id;
        req.symbol        = o.get_symbol();
        req.endpoint      = std::string(enc.endpoint);
        req.wire_payload  = std::string(enc.wire_payload);
        req.is_cancel     = false;

        // Enqueue under short lock (orders are rare vs market ticks).
        {
            std::lock_guard<std::mutex> lk(submit_queue_mu_);
            if (!accepting_orders_.load(std::memory_order_acquire))
            {
                set_error("ExecutionBridge: live submission is quiesced");
                return;
            }
            submit_queue_.push_back(std::move(req));
        }
    }

    bool poll_fills(std::vector<fill_event>& out) override
    {
        std::lock_guard<std::mutex> lk(fills_mu_);
        if (pending_fill_head_ == pending_fills_.size()) return false;
        out.insert(out.end(),
                   std::make_move_iterator(
                       pending_fills_.begin()
                       + static_cast<std::ptrdiff_t>(pending_fill_head_)),
                   std::make_move_iterator(pending_fills_.end()));
        pending_fills_.clear();
        pending_fill_head_ = 0;
        return true;
    }

    bool supports_transactional_fill_delivery() const noexcept override
    {
        return true;
    }

    bool peek_fill(fill_event& out) override
    {
        std::lock_guard<std::mutex> lk(fills_mu_);
        if (pending_fill_head_ == pending_fills_.size())
            return false;
        out = pending_fills_[pending_fill_head_];
        return true;
    }

    bool acknowledge_fill(std::uint64_t fill_id) override
    {
        std::lock_guard<std::mutex> lk(fills_mu_);
        if (pending_fill_head_ == pending_fills_.size()
            || pending_fills_[pending_fill_head_].get_fill_id() != fill_id)
            return false;
        ++pending_fill_head_;
        if (pending_fill_head_ == pending_fills_.size())
        {
            pending_fills_.clear();
            pending_fill_head_ = 0;
        }
        return true;
    }

    bool cancel_order(uint64_t engine_order_id) override
    {
        clear_error();
        if (!d_.write_safety_readiness.permits_private_exchange_writes())
        {
            set_error(
                "ExecutionBridge: write safety readiness is not validated");
            return false;
        }
        if (!accepting_orders_.load(std::memory_order_acquire)) return false;
        std::string exchange_id, symbol, client_id;
        {
            std::lock_guard<std::mutex> lk(map_mu_);
            auto it = by_engine_id_.find(engine_order_id);
            if (it == by_engine_id_.end() || it->second.terminal_observed)
                return false;
            exchange_id = it->second.exchange_id;
            symbol      = it->second.symbol;
            client_id   = it->second.client_id;
        }

        if (!d_.encoder || !d_.order_tx) return false;

        submit_request req;
        req.engine_id           = engine_order_id;
        req.client_id           = client_id;
        req.symbol              = symbol;
        req.is_cancel           = true;
        req.cancel_exchange_id  = exchange_id;

        {
            std::lock_guard<std::mutex> lk(submit_queue_mu_);
            if (!accepting_orders_.load(std::memory_order_acquire)) return false;
            submit_queue_.push_back(std::move(req));
        }

        return true;   // Enqueued. Actual result is async.
    }

    bool poll_status(std::vector<status_event>& out)
    {
        std::lock_guard<std::mutex> lk(status_mu_);
        if (pending_status_.empty()) return false;
        out.insert(out.end(),
                   std::make_move_iterator(pending_status_.begin()),
                   std::make_move_iterator(pending_status_.end()));
        pending_status_.clear();
        return true;
    }

    // Capability hooks (IExecutionAdapter + IAsyncSubmitSupport).
    // Declared here so they are part of the concrete public surface.
    bool supports_async_submit() const override { return true; }
    const std::string& last_error() const override
    {
        // Note: lock is taken but we return a reference to the member.
        // Callers (dashboard) copy the string immediately. This matches
        // the pre-existing pattern in BinanceExecutor.
        static thread_local std::string cached;
        std::lock_guard<std::mutex> lk(error_mu_);
        cached = last_error_;
        return cached;
    }
    IAsyncSubmitSupport* get_async_support() override { return this; }

    // New: drain results from async submits/cancels.
    // Engine should call this after submit_order (and periodically).
    // Mirrors the poll_fills pattern used for incoming fills.
    // Also implements IAsyncSubmitSupport.
    bool poll_submit_results(std::vector<submit_result>& out) override
    {
        std::lock_guard<std::mutex> lk(submit_results_mu_);
        if (pending_submit_results_.empty()) return false;
        out.insert(out.end(),
                   std::make_move_iterator(pending_submit_results_.begin()),
                   std::make_move_iterator(pending_submit_results_.end()));
        pending_submit_results_.clear();
        return true;
    }

    bool poll_lifecycle_event(venue_lifecycle_event& out) noexcept override
    {
        const auto read = lifecycle_read_.load(std::memory_order_relaxed);
        const auto write = lifecycle_write_.load(std::memory_order_acquire);
        if (read >= write) return false;
        out = lifecycle_events_[read & (lifecycle_capacity - 1U)];
        lifecycle_read_.store(read + 1U, std::memory_order_release);
        return true;
    }

    // Test helper: synchronously drain and process any pending outbound
    // submissions (for unit tests that need deterministic immediate results
    // without relying on thread scheduling). Safe to call from test thread.
    void drain_outbound_for_test();

private:
    struct tracked_order
    {
        uint64_t    engine_id     = 0;
        std::string client_id;
        std::string exchange_id;
        std::string symbol;
        order_side  side           = order_side::buy;
        double      total_qty      = 0.0;
        double      cumulative_qty = 0.0;
        double      intended_price = 0.0;
        std::chrono::system_clock::time_point decision_ts{};
        bool        terminal_observed = false;
    };

    struct execution_dedupe_slot
    {
        bool occupied = false;
        std::uint64_t engine_id = 0;
        bounded_event_text<64> symbol{};
        order_side side = order_side::buy;
        bounded_event_text<96> venue_execution_id{};
        bounded_event_text<24> commission_asset{};
        double last_fill_qty = 0.0;
        double last_fill_price = 0.0;
        double cumulative_qty = 0.0;
        double commission = 0.0;
        std::chrono::system_clock::time_point ts{};
        bool has_cumulative_qty = false;
    };

    enum class execution_probe_code : std::uint8_t
    {
        insert,
        duplicate,
        conflict,
        capacity_exhausted
    };

    struct execution_probe
    {
        execution_probe_code code = execution_probe_code::capacity_exhausted;
        std::size_t slot = 0;
    };

    [[nodiscard]] execution_probe probe_execution_id(
        std::uint64_t engine_id, const parsed_exec& msg) const noexcept
    {
        if (execution_dedupe_.empty()
            || msg.venue_execution_id.empty()
            || msg.venue_execution_id.size() > 96U
            || msg.symbol.empty()
            || msg.symbol.size() > 64U
            || msg.commission_asset.empty()
            || msg.commission_asset.size() > 24U)
            return {};

        std::uint64_t hash = 1469598103934665603ULL;
        for (const unsigned char c : msg.symbol)
        {
            hash ^= c;
            hash *= 1099511628211ULL;
        }
        for (const unsigned char c : msg.venue_execution_id)
        {
            hash ^= c;
            hash *= 1099511628211ULL;
        }

        const std::size_t capacity = execution_dedupe_.size();
        std::size_t index = static_cast<std::size_t>(hash % capacity);
        for (std::size_t probe = 0; probe < capacity; ++probe)
        {
            const auto& slot = execution_dedupe_[index];
            if (!slot.occupied)
                return {execution_probe_code::insert, index};
            if (slot.symbol.view() == msg.symbol
                && slot.venue_execution_id.view() == msg.venue_execution_id)
            {
                const bool exact =
                    slot.engine_id == engine_id
                    && slot.side == msg.side
                    && slot.last_fill_qty == msg.last_fill_qty
                    && slot.last_fill_price == msg.last_fill_price
                    && slot.commission == msg.commission
                    && slot.commission_asset.view() == msg.commission_asset
                    && slot.has_cumulative_qty == msg.has_cumulative_qty
                    && (!msg.has_cumulative_qty
                        || slot.cumulative_qty == msg.cumulative_qty)
                    && slot.ts == msg.ts;
                return {exact ? execution_probe_code::duplicate
                              : execution_probe_code::conflict,
                        index};
            }
            index = (index + 1U) % capacity;
        }
        return {execution_probe_code::capacity_exhausted, 0};
    }

    void commit_execution_id(std::size_t index,
                             std::uint64_t engine_id,
                             const parsed_exec& msg) noexcept
    {
        auto& slot = execution_dedupe_[index];
        slot.engine_id = engine_id;
        (void)slot.symbol.assign(msg.symbol);
        slot.side = msg.side;
        (void)slot.venue_execution_id.assign(msg.venue_execution_id);
        (void)slot.commission_asset.assign(msg.commission_asset);
        slot.last_fill_qty = msg.last_fill_qty;
        slot.last_fill_price = msg.last_fill_price;
        slot.cumulative_qty = msg.cumulative_qty;
        slot.commission = msg.commission;
        slot.ts = msg.ts;
        slot.has_cumulative_qty = msg.has_cumulative_qty;
        slot.occupied = true;
    }

    void handle_message(std::string_view raw)
    {
        if (!d_.parser) return;

        parsed_funding_update funding;
        const auto funding_result =
            d_.parser->parse_funding_update(raw, funding);
        if (funding_result != funding_parse_result::not_funding)
        {
            bool accepted = false;
            if (funding_result == funding_parse_result::valid
                && d_.funding_update_handler)
            {
                try { accepted = d_.funding_update_handler(funding); }
                catch (...) { accepted = false; }
            }
            if (!accepted && d_.funding_failure_handler)
            {
                try { d_.funding_failure_handler(); }
                catch (...) {}
            }
            return;
        }

        parsed_exec msg;
        if (!d_.parser->parse(raw, msg))
        {
            // Not an order-lifecycle event; might be a server-pushed
            // position/balance snapshot. Spot's parser short-circuits
            // here (default returns false); futures recognizes
            // ACCOUNT_UPDATE.
            if (d_.position_snapshot_handler)
            {
                parsed_position_snapshot snap;
                if (d_.parser->parse_position_snapshot(raw, snap))
                    d_.position_snapshot_handler(snap);
            }
            return;
        }

        const bool fill_kind = msg.k == parsed_exec::kind::partial_fill
            || msg.k == parsed_exec::kind::full_fill;
        const bool lifecycle_kind = msg.k == parsed_exec::kind::ack
            || msg.k == parsed_exec::kind::canceled
            || msg.k == parsed_exec::kind::rejected
            || msg.k == parsed_exec::kind::expired;
        if (msg.k == parsed_exec::kind::invalid
            || !std::isfinite(msg.last_fill_qty)
            || !std::isfinite(msg.last_fill_price)
            || !std::isfinite(msg.cumulative_qty)
            || !std::isfinite(msg.commission)
            || msg.last_fill_qty < 0.0
            || msg.last_fill_price < 0.0
            || msg.cumulative_qty < 0.0
            || (lifecycle_kind
                && msg.ts.time_since_epoch().count() == 0)
            || (fill_kind && (!(msg.last_fill_qty > 0.0)
                              || !(msg.last_fill_price > 0.0)
                              || msg.venue_execution_id.empty()
                              || msg.commission_asset.empty()
                              || msg.ts.time_since_epoch().count() == 0)))
        {
            fail_terminal_ingress(
                "ExecutionBridge: malformed execution report", msg.symbol);
            return;
        }

        uint64_t engine_id = 0;
        const char* invalid_tracking = nullptr;
        bool duplicate_execution = false;
        bool fill_enqueued = false;
        {
            bool unknown = false;
            {
                std::lock_guard<std::mutex> lk(map_mu_);
                auto cit = by_client_id_.find(msg.client_order_id);
                if (cit == by_client_id_.end())
                    unknown = true;
                else
                    engine_id = cit->second;
            }
            if (unknown)
            {
                // Bracket synthesis is an economic-fill-only seam.  ACK,
                // cancel, reject, expire and informational reports must never
                // acquire an engine order id or enter the fill pipeline.
                // Their canonical lifecycle ingress is handled separately.
                if (!fill_kind)
                {
                    if (lifecycle_kind)
                    {
                        fail_terminal_ingress(
                            "ExecutionBridge: lifecycle references unknown order",
                            msg.symbol);
                    }
                    return;
                }
                // Unknown client_id but we may still recognize the
                // exchange_order_id as a venue-managed bracket leg.
                // Defer to the engine-supplied handler.
                dispatch_unknown_fill(msg);
                return;
            }
            std::lock_guard<std::mutex> lk(map_mu_);
            auto cit = by_client_id_.find(msg.client_order_id);
            if (cit == by_client_id_.end()) return;
            engine_id = cit->second;
            auto eit = by_engine_id_.find(engine_id);
            if (eit == by_engine_id_.end()) return;

            if (msg.symbol != eit->second.symbol
                || msg.side != eit->second.side)
            {
                invalid_tracking =
                    "ExecutionBridge: execution identity differs from submitted order";
            }
            else if (!msg.exchange_order_id.empty()
                     && !eit->second.exchange_id.empty()
                     && msg.exchange_order_id != eit->second.exchange_id)
            {
                invalid_tracking =
                    "ExecutionBridge: exchange order identity changed";
            }

            if (fill_kind && !invalid_tracking)
            {
                const auto execution = probe_execution_id(engine_id, msg);
                if (execution.code == execution_probe_code::duplicate)
                {
                    duplicate_execution = true;
                }
                else if (execution.code == execution_probe_code::conflict)
                {
                    invalid_tracking =
                        "ExecutionBridge: native execution id replay changed economic fields";
                }
                else if (execution.code
                         == execution_probe_code::capacity_exhausted)
                {
                    invalid_tracking =
                        "ExecutionBridge: native execution id registry exhausted or identity oversized";
                }

                const double next_cumulative = msg.has_cumulative_qty
                    ? msg.cumulative_qty
                    : eit->second.cumulative_qty + msg.last_fill_qty;
                const double scale = std::max(
                    {1.0, std::abs(next_cumulative),
                     std::abs(eit->second.cumulative_qty),
                     std::abs(msg.last_fill_qty)});
                const double next_scale = std::nextafter(
                    scale, std::numeric_limits<double>::infinity());
                const double ulp = next_scale - scale;
                const double arithmetic_tolerance =
                    std::isfinite(ulp) ? 2.0 * ulp : 0.0;
                if (!duplicate_execution && !invalid_tracking
                    && (!std::isfinite(next_cumulative)
                    || next_cumulative > eit->second.total_qty
                    || next_cumulative <= eit->second.cumulative_qty
                    || std::abs((next_cumulative - eit->second.cumulative_qty)
                                - msg.last_fill_qty)
                        > arithmetic_tolerance))
                {
                    invalid_tracking =
                        "ExecutionBridge: fill cumulative quantity is inconsistent";
                }
                else if (!duplicate_execution && !invalid_tracking
                         && msg.k == parsed_exec::kind::full_fill
                         && next_cumulative != eit->second.total_qty)
                {
                    invalid_tracking =
                        "ExecutionBridge: terminal fill does not complete order";
                }
                else if (!duplicate_execution && !invalid_tracking)
                {
                    if (msg.k == parsed_exec::kind::partial_fill
                        && next_cumulative == eit->second.total_qty)
                        msg.k = parsed_exec::kind::full_fill;

                    const double remaining = std::max(
                        0.0, eit->second.total_qty - next_cumulative);
                    std::lock_guard<std::mutex> fill_lock(fills_mu_);
                    const auto pending_fill_count =
                        pending_fills_.size() - pending_fill_head_;
                    if (pending_fill_count >= d_.fill_ingress_capacity)
                    {
                        invalid_tracking =
                            "ExecutionBridge: economic fill ingress capacity exhausted";
                    }
                    else if (next_fill_id_ == 0
                             || next_fill_id_
                                == std::numeric_limits<std::uint64_t>::max())
                    {
                        invalid_tracking =
                            "ExecutionBridge: local fill correlation id exhausted";
                    }
                    else
                    {
                        const auto fill_id = next_fill_id_;
                        fill_event fe(
                            msg.ts, msg.symbol, engine_id, msg.side,
                            msg.last_fill_qty, msg.last_fill_price,
                            msg.commission, remaining, fill_id);
                        fe.set_source(fill_source::exchange);
                        const bool identity_stamped =
                            fe.set_venue_execution_id(msg.venue_execution_id)
                            && fe.set_commission_currency(msg.commission_asset);
                        if (!identity_stamped)
                        {
                            invalid_tracking =
                                "ExecutionBridge: execution identity exceeds event capacity";
                        }
                        else
                        {
                            // ACKed prefix slots are reclaimed in-place. This
                            // compaction is bounded and cannot grow the vector;
                            // it keeps the configured ingress capacity a hard
                            // limit even when producer and consumer interleave.
                            if (pending_fill_head_ != 0
                                && pending_fills_.size()
                                   >= d_.fill_ingress_capacity)
                            {
                                std::move(
                                    pending_fills_.begin()
                                        + static_cast<std::ptrdiff_t>(
                                            pending_fill_head_),
                                    pending_fills_.end(),
                                    pending_fills_.begin());
                                pending_fills_.erase(
                                    pending_fills_.begin()
                                        + static_cast<std::ptrdiff_t>(
                                            pending_fill_count),
                                    pending_fills_.end());
                                pending_fill_head_ = 0;
                            }
                            fe.set_cumulative_filled_qty(
                                next_cumulative,
                                msg.has_cumulative_qty
                                    ? fill_cumulative_source::venue_reported
                                    : fill_cumulative_source::engine_accumulated);
                            fill_provenance provenance;
                            provenance.model = fill_execution_model::venue_reported;
                            provenance.reason =
                                fill_execution_reason::venue_execution_report;
                            provenance.intended_price = eit->second.intended_price;
                            provenance.reference_price = msg.last_fill_price;
                            provenance.reference_timestamp = msg.ts;
                            if (msg.ts > eit->second.decision_ts)
                            {
                                provenance.modeled_latency =
                                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                                        msg.ts - eit->second.decision_ts);
                            }
                            fe.set_provenance(provenance);
                            pending_fills_.push_back(std::move(fe));

                            // Commit bridge cursor and exact replay identity
                            // only after the bounded handoff succeeded.
                            ++next_fill_id_;
                            eit->second.cumulative_qty = next_cumulative;
                            if (msg.k == parsed_exec::kind::full_fill)
                                eit->second.terminal_observed = true;
                            if (eit->second.exchange_id.empty())
                                eit->second.exchange_id = msg.exchange_order_id;
                            commit_execution_id(execution.slot, engine_id, msg);
                            fill_enqueued = true;
                        }
                    }
                }
            }
            else if (!invalid_tracking
                     && !msg.exchange_order_id.empty()
                     && eit->second.exchange_id.empty())
            {
                // ACK/REST identity is advisory correlation state. Keep the
                // mapping as a tombstone across terminal lifecycle reports so
                // a legitimate late fill or reconnect replay remains known.
                eit->second.exchange_id = msg.exchange_order_id;
            }
            if (lifecycle_kind && !invalid_tracking)
            {
                venue_lifecycle_event event;
                event.engine_order_id = engine_id;
                event.exchange_ts = msg.ts;
                switch (msg.k)
                {
                case parsed_exec::kind::ack:
                    event.transition = venue_order_transition::acknowledged;
                    break;
                case parsed_exec::kind::canceled:
                    event.transition = venue_order_transition::canceled;
                    break;
                case parsed_exec::kind::rejected:
                    event.transition = venue_order_transition::rejected;
                    break;
                case parsed_exec::kind::expired:
                    event.transition = venue_order_transition::expired;
                    break;
                default:
                    break;
                }
                const auto write = lifecycle_write_.load(
                    std::memory_order_relaxed);
                const auto read = lifecycle_read_.load(
                    std::memory_order_acquire);
                if (write - read >= lifecycle_capacity)
                {
                    invalid_tracking =
                        "ExecutionBridge: lifecycle ingress capacity exhausted";
                }
                else
                {
                    lifecycle_events_[write & (lifecycle_capacity - 1U)] = event;
                    lifecycle_write_.store(
                        write + 1U, std::memory_order_release);
                    if (msg.k == parsed_exec::kind::canceled
                        || msg.k == parsed_exec::kind::rejected
                        || msg.k == parsed_exec::kind::expired)
                    {
                        eit->second.terminal_observed = true;
                    }
                }
            }
        }

        if (invalid_tracking)
        {
            fail_terminal_ingress(invalid_tracking, msg.symbol);
            return;
        }

        (void)duplicate_execution;
        (void)fill_enqueued;
    }

    void handle_status(IFillTransport::lifecycle st, std::string_view note)
    {
        std::lock_guard<std::mutex> lk(status_mu_);
        pending_status_.push_back({st, std::string(note)});
    }

    void fail_terminal_ingress(std::string_view reason,
                               std::string_view symbol)
    {
        if (ingress_failure_latched_.exchange(
                true, std::memory_order_acq_rel))
            return;
        {
            std::lock_guard<std::mutex> admission_lock(venue_admission_mu_);
            close_terminal_admission();
        }
        set_error(std::string(reason));
        handle_status(IFillTransport::lifecycle::error, reason);

        submit_result fatal;
        fatal.symbol = std::string(symbol);
        fatal.error = std::string(reason);
        fatal.op = submit_result::operation::submit;
        fatal.ok = false;
        fatal.fatal = true;
        std::lock_guard<std::mutex> lk(submit_results_mu_);
        pending_submit_results_.push_back(std::move(fatal));
    }

    void dispatch_unknown_fill(const parsed_exec& msg)
    {
        if (msg.k != parsed_exec::kind::partial_fill
            && msg.k != parsed_exec::kind::full_fill)
            return;
        if (!msg.has_cumulative_qty)
        {
            fail_terminal_ingress(
                "ExecutionBridge: venue bracket fill lacks authoritative cumulative quantity",
                msg.symbol);
            return;
        }

        unknown_fill_handler handler;
        {
            std::lock_guard<std::mutex> lk(handler_mu_);
            handler = unknown_fill_handler_;
        }
        if (!handler) return;
        if (msg.exchange_order_id.empty()) return;

        std::uint64_t fill_id;
        {
            std::lock_guard<std::mutex> lk(fills_mu_);
            fill_id = next_fill_id_++;
        }

        auto sr = handler(msg, fill_id);
        if (!sr) return;

        auto& fill = sr->fill;
        if (fill.get_symbol() != msg.symbol
            || fill.get_side() != msg.side
            || fill.get_filled_quantity() != msg.last_fill_qty
            || fill.get_fill_price() != msg.last_fill_price
            || fill.get_commission() != msg.commission
            || !fill.set_venue_execution_id(msg.venue_execution_id)
            || !fill.set_commission_currency(msg.commission_asset))
        {
            fail_terminal_ingress(
                "ExecutionBridge: synthesized venue bracket fill changed execution identity",
                msg.symbol);
            return;
        }
        fill.set_source(fill_source::exchange);
        fill.set_cumulative_filled_qty(
            msg.cumulative_qty, fill_cumulative_source::venue_reported);

        // Record meta first so the engine can register in order_meta_
        // before processing the fill. Both queues use their own mutex
        // - the meta is shorter-lived (drained before fills each tick).
        {
            std::lock_guard<std::mutex> lk(synth_mu_);
            pending_synth_meta_.push_back(synth_meta{
                sr->fill.get_order_id(),
                sr->opener_order_id,
                sr->strategy_name});
        }
        std::lock_guard<std::mutex> lk(fills_mu_);
        pending_fills_.push_back(std::move(sr->fill));
    }

    static std::string make_client_id(uint64_t engine_order_id)
    {
        return "tt-" + std::to_string(engine_order_id);
    }

    void set_error(std::string msg)
    {
        std::lock_guard<std::mutex> lk(error_mu_);
        last_error_ = std::move(msg);
    }

    void clear_error()
    {
        std::lock_guard<std::mutex> lk(error_mu_);
        last_error_.clear();
    }

    deps d_;

    mutable std::mutex map_mu_;
    std::unordered_map<uint64_t, tracked_order> by_engine_id_;
    std::unordered_map<std::string, uint64_t>   by_client_id_;
    std::vector<execution_dedupe_slot> execution_dedupe_;

    std::mutex fills_mu_;
    std::vector<fill_event> pending_fills_;
    std::size_t pending_fill_head_ = 0;
    uint64_t next_fill_id_ = 1;

    std::mutex status_mu_;
    std::vector<status_event> pending_status_;

    // One private user-data reader produces and the engine loop consumes.
    // Lifecycle loss is unsafe, so overflow is terminal and never overwrites.
    static constexpr std::size_t lifecycle_capacity = 4096;
    static_assert((lifecycle_capacity & (lifecycle_capacity - 1U)) == 0U);
    std::array<venue_lifecycle_event, lifecycle_capacity> lifecycle_events_{};
    alignas(64) std::atomic<std::size_t> lifecycle_write_{0};
    alignas(64) std::atomic<std::size_t> lifecycle_read_{0};

    mutable std::mutex error_mu_;
    std::string last_error_;

    mutable std::mutex handler_mu_;
    unknown_fill_handler unknown_fill_handler_;

    std::mutex synth_mu_;
    std::vector<synth_meta> pending_synth_meta_;

    // --- Async order submission support (Phase 1) ---
    struct submit_request
    {
        uint64_t    engine_id = 0;
        std::string client_id;
        std::string symbol;
        std::string endpoint;
        std::string wire_payload;
        bool        is_cancel = false;
        std::string cancel_exchange_id;
    };

    // Simple mutex-protected queue for outbound (orders are low-frequency vs. market ticks,
    // so a short critical section on enqueue is acceptable and avoids cross-layer dep).
    std::mutex                  submit_queue_mu_;
    std::deque<submit_request>  submit_queue_;

    // Guards the final accepting_orders_ check and the corresponding venue
    // mutation, making the IOrderTransport quiesce admission contract exact.
    std::mutex venue_admission_mu_;

    std::mutex submit_results_mu_;
    std::vector<submit_result> pending_submit_results_;

    std::thread transport_thread_;
    std::atomic<bool> transport_running_{false};
    std::atomic<bool> accepting_orders_{false};
    std::atomic<bool> ingress_failure_latched_{false};

    void transport_loop();
    void process_one_submit(const submit_request& req);
    void close_terminal_admission();
};


// ================== Async transport implementations (clean) ==================

inline void ExecutionBridge::transport_loop()
{
    submit_request req;
    while (transport_running_.load(std::memory_order_acquire))
    {
        bool did_work = false;
        {
            std::lock_guard<std::mutex> lk(submit_queue_mu_);
            if (!submit_queue_.empty()) {
                req = std::move(submit_queue_.front());
                submit_queue_.pop_front();
                did_work = true;
            }
        }
        if (did_work) {
            try {
                process_one_submit(req);
            } catch (const std::exception& e) {
                // An exception from a venue mutation cannot prove whether
                // bytes reached the exchange. Close admission before
                // publishing the result so no already-queued mutation can
                // overtake the engine's terminal halt reaction.
                {
                    std::lock_guard<std::mutex> admission_lock(
                        venue_admission_mu_);
                    close_terminal_admission();
                }
                submit_result sr;
                sr.engine_id = req.engine_id;
                sr.symbol = req.symbol;
                sr.error = std::string("exception: ") + e.what();
                sr.op = req.is_cancel
                    ? submit_result::operation::cancel
                    : submit_result::operation::submit;
                sr.ok = false;
                sr.uncertain = true;
                sr.fatal = true;
                {
                    std::lock_guard<std::mutex> lk(submit_results_mu_);
                    pending_submit_results_.push_back(std::move(sr));
                }
            } catch (...) {
                {
                    std::lock_guard<std::mutex> admission_lock(
                        venue_admission_mu_);
                    close_terminal_admission();
                }
                submit_result sr;
                sr.engine_id = req.engine_id;
                sr.symbol = req.symbol;
                sr.error = "unknown exception from order transport";
                sr.op = req.is_cancel
                    ? submit_result::operation::cancel
                    : submit_result::operation::submit;
                sr.ok = false;
                sr.uncertain = true;
                sr.fatal = true;
                {
                    std::lock_guard<std::mutex> lk(submit_results_mu_);
                    pending_submit_results_.push_back(std::move(sr));
                }
            }
        } else {
#ifdef __x86_64__
            _mm_pause();
#else
            std::this_thread::yield();
#endif
        }
    }
}

inline void ExecutionBridge::close_terminal_admission()
{
    accepting_orders_.store(false, std::memory_order_release);
    std::lock_guard<std::mutex> queue_lock(submit_queue_mu_);
    submit_queue_.clear();
}

inline void ExecutionBridge::process_one_submit(const submit_request& req)
{
    if (!d_.order_tx || !d_.encoder
        || !d_.write_safety_readiness.permits_private_exchange_writes()
        || !accepting_orders_.load(std::memory_order_acquire)) return;

    if (req.is_cancel)
    {
        std::string symbol = req.symbol;
        std::string exchange_id = req.cancel_exchange_id;
        std::string client_id = req.client_id;

        {
            std::lock_guard<std::mutex> lk(map_mu_);
            auto it = by_engine_id_.find(req.engine_id);
            if (it != by_engine_id_.end())
            {
                if (!it->second.symbol.empty()) symbol = it->second.symbol;
                if (!it->second.exchange_id.empty()) exchange_id = it->second.exchange_id;
                if (!it->second.client_id.empty()) client_id = it->second.client_id;
            }
        }

        if (d_.order_rate_limiter
            && !d_.order_rate_limiter->acquire_interruptibly(
                transport_running_, 1.0))
            return;

        auto enc = d_.encoder->encode_cancel(symbol, exchange_id, client_id);
        IOrderTransport::result res;
        {
            std::lock_guard<std::mutex> admission_lock(venue_admission_mu_);
            if (!d_.write_safety_readiness.permits_private_exchange_writes()
                || !accepting_orders_.load(std::memory_order_acquire)) return;
            res = d_.order_tx->cancel(enc.endpoint, enc.wire_payload);
            if (res.uncertain || res.fatal) close_terminal_admission();
        }

        if (!res.ok) {
            set_error("cancel failed: " + res.error);
        }
        // A REST cancel response is command acknowledgement, not proof that
        // the order reached a terminal economic state.  Preserve correlation
        // as a tombstone until authoritative user-data reconciliation; a fill
        // already in flight must still resolve to the original engine order.

        submit_result sr;
        sr.engine_id = req.engine_id;
        sr.symbol = symbol;
        sr.exchange_order_id = res.exchange_order_id;
        sr.error = res.ok ? "" : res.error;
        sr.op = submit_result::operation::cancel;
        sr.ok = res.ok;
        sr.uncertain = res.uncertain;
        sr.fatal = res.fatal;
        {
            std::lock_guard<std::mutex> lk(submit_results_mu_);
            pending_submit_results_.push_back(std::move(sr));
        }
        return;
    }

    if (d_.order_rate_limiter
        && !d_.order_rate_limiter->acquire_interruptibly(
            transport_running_, 1.0))
        return;

    IOrderTransport::result res;
    {
        std::lock_guard<std::mutex> admission_lock(venue_admission_mu_);
        if (!d_.write_safety_readiness.permits_private_exchange_writes()
            || !accepting_orders_.load(std::memory_order_acquire)) return;
        res = d_.order_tx->submit(req.endpoint, req.wire_payload);
        if (res.uncertain || res.fatal) close_terminal_admission();
    }

    if (!res.ok && !res.uncertain && !res.fatal) {
        set_error("submit failed: " + res.error);
        std::lock_guard<std::mutex> lk(map_mu_);
        auto it = by_engine_id_.find(req.engine_id);
        if (it != by_engine_id_.end())
            it->second.terminal_observed = true;
    }

    if (res.ok && !res.exchange_order_id.empty())
    {
        std::lock_guard<std::mutex> lk(map_mu_);
        auto it = by_engine_id_.find(req.engine_id);
        if (it != by_engine_id_.end() && it->second.exchange_id.empty())
            it->second.exchange_id = res.exchange_order_id;
    }

    submit_result sr;
    sr.engine_id = req.engine_id;
    sr.symbol = req.symbol;
    sr.exchange_order_id = res.exchange_order_id;
    sr.error = res.ok ? "" : res.error;
    sr.op = submit_result::operation::submit;
    sr.ok = res.ok;
    sr.uncertain = res.uncertain;
    sr.fatal = res.fatal;
    {
        std::lock_guard<std::mutex> lk(submit_results_mu_);
        pending_submit_results_.push_back(std::move(sr));
    }
}

inline void ExecutionBridge::drain_outbound_for_test()
{
    // Process any queued work right now on the calling thread.
    // Used only in tests. Locks the queue briefly.
    submit_request req;
    for (;;) {
        {
            std::lock_guard<std::mutex> lk(submit_queue_mu_);
            if (submit_queue_.empty()) break;
            req = std::move(submit_queue_.front());
            submit_queue_.pop_front();
        }
        try {
            process_one_submit(req);
        } catch (...) {}
    }
}
