#pragma once

#include "execution_adapter.h"
#include "order_transport.h"
#include "fill_transport.h"
#include "order_encoder.h"
#include "fill_parser.h"
#include "private_execution_ingress.h"
#include "rate_limiter.h"
#include "async_support.h"
#include "../core/event.h"

#include <chrono>
#include <algorithm>
#include <array>
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

        // A parser has recognized a private execution envelope but cannot
        // prove its required structure, identity, numeric fields, or time.
        // This runs once on the private reader thread after the bridge has
        // closed order admission.  It must synchronously latch the provider/
        // engine halt path; it must not enqueue a REST submit result because
        // that queue has a different producer and ordering contract.
        std::function<void()> execution_failure_handler;

        // When supplied, this is the sole private-account handoff.  The
        // private reader parses and publishes a POD record only; all identity
        // resolution and lifecycle mutation run later on the engine thread.
        IPrivateExecutionIngress* execution_ingress = nullptr;
        std::string private_account_symbol;
        bool require_execution_ingress = false;

        bool start_transport_thread = true;
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

    private_execution_resolution
    resolve_private_execution(private_execution_record& record) override;

    bool commit_private_execution(
        const private_execution_reservation& reservation) override;

    bool rollback_private_execution(
        const private_execution_reservation& reservation) noexcept override;

    bool acknowledge_private_terminal(std::uint64_t sequence) override;

    bool check_private_lifecycle_deadline() override;

    bool has_unresolved_private_lifecycle() const override;

    void fail_private_execution_admission() noexcept override
    {
        fail_malformed_execution();
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
        // `d_` is deliberately the first member and is therefore destroyed
        // last.  A fill transport may outlive the bridge through a provider
        // reference, or it may be released while `d_` is torn down.  In both
        // cases its close/destructor is allowed to publish a final status.
        // Detach our [this] callbacks only after close() has joined its
        // reader, while all bridge callback targets still exist.
        close();
    }

    explicit ExecutionBridge(deps d)
        : d_(std::move(d))
    {
        attach_fill_callbacks();
    }

    bool open()
    {
        // A malformed known private-execution frame is terminal for this
        // bridge instance.  A later open() must not silently resurrect order
        // admission after its provider has begun a halt/reconcile path.
        if (malformed_execution_latched_.load(std::memory_order_acquire))
        {
            set_error("ExecutionBridge: malformed private execution envelope");
            return false;
        }
        // A live private bridge without both the tri-state parser and its
        // terminal failure sink could accept orders while silently losing the
        // venue's only authoritative lifecycle feed.  Refuse configuration
        // before opening either transport.
        if (!d_.order_tx || !d_.fill_tx || !d_.parser
            || !d_.execution_failure_handler
            || (d_.require_execution_ingress && !d_.execution_ingress)
            || (!d_.execution_ingress
                && (static_cast<bool>(d_.funding_update_handler)
                    != static_cast<bool>(d_.funding_failure_handler))))
        {
            set_error("ExecutionBridge: missing execution safety dependency");
            return false;
        }
        // close() detaches callbacks only after the reader has joined.  A
        // later explicit reopen must install the bridge-owned sinks before it
        // creates another private session.
        attach_fill_callbacks();
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

        // fill_tx->open() may synchronously deliver a private frame.  Publish
        // admission only if no such callback latched a terminal parse fault.
        bool malformed_during_open = false;
        {
            std::lock_guard<std::mutex> admission_lock(venue_admission_mu_);
            if (malformed_execution_latched_.load(std::memory_order_acquire))
                malformed_during_open = true;
            else
                accepting_orders_.store(true, std::memory_order_release);
        }
        if (malformed_during_open)
        {
            // Do not join/close a transport while holding
            // venue_admission_mu_: its reader may be finalizing the same
            // malformed-frame latch and waiting for that lock.
            d_.fill_tx->close();
            d_.order_tx->close();
            set_error("ExecutionBridge: malformed private execution envelope");
            return false;
        }
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
        // Concrete transports accept callback clearing only after close has
        // proved the private reader joined.  This prevents their later
        // destructor/reopen status publication from calling into a destroyed
        // bridge while retaining the no-clear-while-live invariant.
        detach_fill_callbacks_after_join();
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

        bool duplicate_identity = false;
        {
            std::lock_guard<std::mutex> lk(map_mu_);
            // A reused engine/client identity can make an otherwise valid
            // venue fill bind to a different intent.  Reject it before the
            // request enters the transport queue; overwriting either index is
            // never a recoverable condition for a live ledger.
            duplicate_identity = by_engine_id_.contains(t.engine_id)
                || by_client_id_.contains(t.client_id);
            if (!duplicate_identity)
            {
                by_engine_id_.emplace(t.engine_id, t);
                by_client_id_.emplace(t.client_id, t.engine_id);
            }
        }
        if (duplicate_identity)
        {
            fail_malformed_execution();
            return;
        }

        submit_request req;
        req.engine_id     = o.get_order_id();
        req.client_id     = client_id;
        req.symbol        = o.get_symbol();
        req.endpoint      = std::string(enc.endpoint);
        req.wire_payload  = std::string(enc.wire_payload);
        req.is_cancel     = false;

        // Enqueue under short lock (orders are rare vs market ticks).  If
        // shutdown wins after identity registration but before enqueue, remove
        // only the still-active unsent mapping; never erase a private record
        // that has already advanced it meanwhile.
        bool queued = false;
        {
            std::lock_guard<std::mutex> lk(submit_queue_mu_);
            if (accepting_orders_.load(std::memory_order_acquire))
            {
                submit_queue_.push_back(std::move(req));
                queued = true;
            }
        }
        if (!queued)
        {
            std::lock_guard<std::mutex> lk(map_mu_);
            auto it = by_engine_id_.find(t.engine_id);
            if (it != by_engine_id_.end()
                && it->second.state == tracked_order::lifecycle::active)
            {
                by_client_id_.erase(it->second.client_id);
                by_engine_id_.erase(it);
            }
            set_error("ExecutionBridge: live submission is quiesced");
        }
    }

    bool poll_fills(std::vector<fill_event>& out) override
    {
        std::lock_guard<std::mutex> lk(fills_mu_);
        if (pending_fills_.empty()) return false;
        out.insert(out.end(),
                   std::make_move_iterator(pending_fills_.begin()),
                   std::make_move_iterator(pending_fills_.end()));
        pending_fills_.clear();
        return true;
    }

    bool cancel_order(uint64_t engine_order_id) override
    {
        if (!accepting_orders_.load(std::memory_order_acquire)) return false;
        if (!d_.encoder || !d_.order_tx) return false;
        std::string exchange_id, symbol, client_id;
        {
            std::lock_guard<std::mutex> lk(map_mu_);
            auto it = by_engine_id_.find(engine_order_id);
            if (it == by_engine_id_.end()) return false;
            // A REST cancellation acknowledgement is not venue lifecycle
            // truth.  Mark the request before it leaves the engine so a
            // second local cancel cannot race the one authoritative private
            // terminal we still require afterwards.
            if (it->second.state != tracked_order::lifecycle::active)
                return false;
            exchange_id = it->second.exchange_id;
            symbol      = it->second.symbol;
            client_id   = it->second.client_id;
            it->second.state = tracked_order::lifecycle::cancel_requested;
        }

        submit_request req;
        req.engine_id           = engine_order_id;
        req.client_id           = client_id;
        req.symbol              = symbol;
        req.is_cancel           = true;
        req.cancel_exchange_id  = exchange_id;

        {
            std::lock_guard<std::mutex> lk(submit_queue_mu_);
            if (!accepting_orders_.load(std::memory_order_acquire))
            {
                std::lock_guard<std::mutex> map_lock(map_mu_);
                auto it = by_engine_id_.find(engine_order_id);
                if (it != by_engine_id_.end()
                    && it->second.state
                        == tracked_order::lifecycle::cancel_requested)
                    it->second.state = tracked_order::lifecycle::active;
                return false;
            }
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

    // Test helper: synchronously drain and process any pending outbound
    // submissions (for unit tests that need deterministic immediate results
    // without relying on thread scheduling). Safe to call from test thread.
    void drain_outbound_for_test();

private:
    struct tracked_order
    {
        enum class lifecycle : std::uint8_t
        {
            active,
            // A definitive REST submit response is still not authoritative
            // enough to erase a live private identity. Keep it resolvable
            // until private truth or reconciliation proves the venue state.
            rest_submit_failed,
            cancel_requested,
            rest_cancel_acked,
            private_terminal_enqueued,
        };

        static constexpr std::size_t execution_history_capacity = 32;

        uint64_t    engine_id     = 0;
        std::string client_id;
        std::string exchange_id;
        std::string symbol;
        order_side  side           = order_side::buy;
        double      total_qty      = 0.0;
        double      cumulative_qty = 0.0;
        lifecycle   state          = lifecycle::active;
        std::chrono::steady_clock::time_point cancel_confirmation_deadline{};
        std::uint64_t terminal_sequence = 0;
        std::uint16_t terminal_slot = std::numeric_limits<std::uint16_t>::max();
        private_execution_record terminal_record{};
        std::array<private_execution_record, execution_history_capacity>
            execution_history{};
        std::uint8_t execution_history_size = 0;

        // The engine owns accounting.  Resolution may reserve capacity and
        // prove the next transition, but it must not make that transition
        // durable until the engine explicitly commits this exact source
        // record.  One pending transition per tracked order is sufficient:
        // the provider ingress is FIFO and the engine consumes it serially.
        struct pending_private_commit
        {
            bool active = false;
            bool append_execution_history = false;
            bool terminal = false;
            bool bind_exchange_id = false;
            std::uint64_t sequence = 0;
            double next_cumulative_qty = 0.0;
            std::uint16_t terminal_slot =
                std::numeric_limits<std::uint16_t>::max();
            private_execution_record record{};
        } pending{};
    };

    // Retain fixed, immutable proof for every retired terminal for the life of
    // the bridge process.  This is intentionally not a cyclic cache: once the
    // proof budget is exhausted the bridge closes admission rather than allow
    // an old duplicate/conflict to lose its identity evidence.  Operators must
    // restart/reconcile before the session can continue beyond this budget.
    static constexpr std::size_t terminal_tombstone_capacity = 4096;
    static constexpr auto private_cancel_confirmation_deadline =
        std::chrono::seconds{30};

    struct terminal_tombstone
    {
        enum class state : std::uint8_t { empty, reserved, committed };
        state status = state::empty;
        std::uint64_t engine_id = 0;
        std::uint64_t sequence = 0;
        private_execution_record record{};
    };

    static bool same_text(std::string_view left,
                          std::string_view right) noexcept
    {
        return left == right;
    }

    static bool same_number(double left, double right) noexcept
    {
        // Parser output is already canonical decimal-to-double conversion.
        // Replays must be exact, not epsilon-close: an altered economic field
        // is contradictory source truth rather than numeric noise.
        return left == right;
    }

    static bool same_order_identity(const private_execution_record& left,
                                    const private_execution_record& right) noexcept;
    static bool same_execution_fingerprint(const private_execution_record& left,
                                           const private_execution_record& right) noexcept;
    static bool same_terminal_replay(const private_execution_record& left,
                                     const private_execution_record& right) noexcept;

    bool reserve_terminal_tombstone_locked(
        tracked_order& tracked,
        const private_execution_record& record,
        std::uint16_t& slot) noexcept;
    void rollback_terminal_tombstone_locked(std::uint16_t slot,
                                            std::uint64_t sequence) noexcept;
    terminal_tombstone* find_tombstone_by_sequence_locked(
        std::uint64_t sequence) noexcept;
    const terminal_tombstone* find_related_tombstone_locked(
        const private_execution_record& record) const noexcept;
    bool exchange_id_available_locked(std::string_view exchange_id,
                                      std::uint64_t engine_id) const noexcept;

    static bool make_private_ingress_record(
        const parsed_exec& msg, private_execution_record& record) noexcept
    {
        using record_kind = private_execution_record::kind;
        switch (msg.k)
        {
        case parsed_exec::kind::ack:          record.k = record_kind::ack; break;
        case parsed_exec::kind::partial_fill: record.k = record_kind::partial_fill; break;
        case parsed_exec::kind::full_fill:    record.k = record_kind::full_fill; break;
        case parsed_exec::kind::canceled:     record.k = record_kind::canceled; break;
        case parsed_exec::kind::rejected:     record.k = record_kind::rejected; break;
        case parsed_exec::kind::expired:      record.k = record_kind::expired; break;
        case parsed_exec::kind::other:        return false;
        }

        // Private lifecycle ordering must rest on source-provided time.  A
        // locally fabricated timestamp can turn an absent/invalid venue field
        // into apparently authoritative order, so reject it at admission.
        if (msg.ts.time_since_epoch().count() == 0) return false;
        const auto ts = msg.ts;
        record.event_time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            ts.time_since_epoch()).count();
        record.last_fill_qty = msg.last_fill_qty;
        record.last_fill_price = msg.last_fill_price;
        record.cumulative_qty = msg.cumulative_qty;
        record.commission = msg.commission;
        record.side = msg.side;
        record.cumulative_reported = msg.has_cumulative_qty;
        record.lifecycle_only = msg.lifecycle_only;

        return private_execution_record::copy_text(
                   record.symbol, record.symbol_size, msg.symbol)
            && private_execution_record::copy_optional_text(
                   record.client_order_id, record.client_order_id_size,
                   msg.client_order_id)
            && private_execution_record::copy_optional_text(
                   record.exchange_order_id, record.exchange_order_id_size,
                   msg.exchange_order_id)
            && private_execution_record::copy_optional_text(
                   record.execution_id, record.execution_id_size,
                   msg.execution_id)
            && private_execution_record::copy_optional_text(
                   record.commission_asset, record.commission_asset_size,
                   msg.commission_asset)
            && private_execution_record::copy_optional_text(
                   record.error, record.error_size, msg.error)
            && (record.client_order_id_size != 0
                || record.exchange_order_id_size != 0);
    }

    void handle_message(std::string_view raw)
    {
        if (!d_.parser)
        {
            // `open()` refuses this configuration, but retain a defensive
            // fail-closed guard in case a future construction path violates
            // that invariant.
            fail_malformed_execution();
            return;
        }

        parsed_funding_update funding;
        const auto funding_result =
            d_.parser->parse_funding_update(raw, funding);
        if (funding_result != funding_parse_result::not_funding)
        {
            if (d_.execution_ingress)
            {
                bool accepted = false;
                if (funding_result == funding_parse_result::valid
                    && !d_.private_account_symbol.empty())
                {
                    private_execution_record record;
                    record.k = private_execution_record::kind::funding;
                    record.event_time_ms = funding.event_time_ms;
                    record.cash_delta = funding.cash_delta;
                    accepted = private_execution_record::copy_text(
                        record.symbol, record.symbol_size,
                        d_.private_account_symbol)
                        && d_.execution_ingress->try_publish(record);
                }
                if (!accepted)
                {
                    d_.execution_ingress->latch_failure();
                    fail_malformed_execution();
                }
                return;
            }

            bool accepted = false;
            if (funding_result == funding_parse_result::valid
                && d_.funding_update_handler)
            {
                try { accepted = d_.funding_update_handler(funding); }
                catch (...) { accepted = false; }
            }
            if (!accepted)
            {
                // A malformed or unrouteable funding update is just as
                // authoritative as a malformed execution envelope.  Close
                // admission synchronously before any provider/engine callback
                // so a queued REST mutation cannot outrun the halt path.
                fail_malformed_execution();
                if (d_.funding_failure_handler)
                {
                    try { d_.funding_failure_handler(); }
                    catch (...) {}
                }
            }
            return;
        }

        parsed_exec msg;
        execution_parse_result execution_result =
            execution_parse_result::malformed;
        try
        {
            execution_result = d_.parser->parse(raw, msg);
        }
        catch (...)
        {
            fail_malformed_execution();
            return;
        }

        if (execution_result == execution_parse_result::malformed)
        {
            fail_malformed_execution();
            return;
        }

        if (execution_result == execution_parse_result::unrelated)
        {
            if (d_.execution_ingress)
            {
                // The unified FIFO is the sole source-order boundary for
                // private account truth.  Do not preserve the legacy
                // diagnostic snapshot escape hatch here: only a parser-
                // proven exact control can be ignored.  Everything else is
                // an unmodelled authenticated state transition and must
                // synchronously close order admission before it can outrun
                // the engine's reconciliation halt.
                if (!d_.parser->is_harmless_private_control(raw))
                {
                    d_.execution_ingress->latch_failure();
                    fail_malformed_execution();
                }
                return;
            }

            // Not an order-lifecycle event; might be a server-pushed
            // position/balance snapshot. Spot's parser short-circuits
            // here (default returns false); futures recognizes
            // ACCOUNT_UPDATE.
            if (d_.position_snapshot_handler)
            {
                try
                {
                    parsed_position_snapshot snap;
                    if (d_.parser->parse_position_snapshot(raw, snap))
                        d_.position_snapshot_handler(snap);
                }
                catch (...)
                {
                    // Snapshot processing is still private venue truth.  Do
                    // not let an allocation/handler failure bypass the same
                    // terminal admission latch used for malformed execution.
                    fail_malformed_execution();
                }
            }
            return;
        }

        if (d_.execution_ingress)
        {
            private_execution_record record;
            if (!make_private_ingress_record(msg, record)
                || !d_.execution_ingress->try_publish(record))
            {
                d_.execution_ingress->latch_failure();
                fail_malformed_execution();
            }
            return;
        }

        uint64_t engine_id = 0;
        double total_qty = 0.0;
        double tracked_cumulative = 0.0;
        bool conflicting_identity = false;
        bool unknown_client = false;
        {
            std::lock_guard<std::mutex> lk(map_mu_);
            auto cit = by_client_id_.find(msg.client_order_id);
            if (cit == by_client_id_.end())
            {
                unknown_client = true;
                // Do not let an unrecognised client ID hide a known exchange
                // ID. This is an explicit contradictory-ID condition, not a
                // venue-managed bracket fallback.
                if (!msg.exchange_order_id.empty())
                {
                    for (const auto& [other_engine_id, other] : by_engine_id_)
                    {
                        (void)other_engine_id;
                        if (other.exchange_id == msg.exchange_order_id)
                        {
                            conflicting_identity = true;
                            break;
                        }
                    }
                }
            }
            else
                engine_id = cit->second;
        }

        if (conflicting_identity)
        {
            fail_malformed_execution();
            return;
        }

        if (unknown_client)
        {
            // Unknown client_id may still be a venue-managed bracket leg.
            // Only actual economic fills are eligible for that path. A
            // cancel/reject/expiry is deliberately left for Slice 3's typed
            // lifecycle ingress and must never become a synthetic fill.
            if (msg.k == parsed_exec::kind::partial_fill ||
                msg.k == parsed_exec::kind::full_fill)
                dispatch_unknown_fill(msg);
            return;
        }

        {
            std::lock_guard<std::mutex> lk(map_mu_);
            auto cit = by_client_id_.find(msg.client_order_id);
            if (cit == by_client_id_.end()) return;
            engine_id = cit->second;
            auto eit = by_engine_id_.find(engine_id);
            if (eit == by_engine_id_.end()) return;

            if (!msg.exchange_order_id.empty())
            {
                for (const auto& [other_engine_id, other] : by_engine_id_)
                {
                    if (other_engine_id != engine_id
                        && other.exchange_id == msg.exchange_order_id)
                    {
                        conflicting_identity = true;
                        break;
                    }
                }
                if (!conflicting_identity && eit->second.exchange_id.empty())
                {
                    eit->second.exchange_id = msg.exchange_order_id;
                }
                else if (!conflicting_identity
                         && eit->second.exchange_id != msg.exchange_order_id)
                {
                    conflicting_identity = true;
                }
            }

            if (msg.symbol != eit->second.symbol || msg.side != eit->second.side)
                conflicting_identity = true;

            if (!conflicting_identity)
            {
                if (msg.k == parsed_exec::kind::partial_fill ||
                    msg.k == parsed_exec::kind::full_fill)
                {
                    eit->second.cumulative_qty += msg.last_fill_qty;
                }

                total_qty = eit->second.total_qty;
                tracked_cumulative = eit->second.cumulative_qty;

                if (msg.k == parsed_exec::kind::full_fill   ||
                    msg.k == parsed_exec::kind::canceled    ||
                    msg.k == parsed_exec::kind::rejected    ||
                    msg.k == parsed_exec::kind::expired)
                {
                    by_client_id_.erase(msg.client_order_id);
                    by_engine_id_.erase(engine_id);
                }
            }
        }

        if (conflicting_identity)
        {
            fail_malformed_execution();
            return;
        }

        if (msg.k != parsed_exec::kind::partial_fill &&
            msg.k != parsed_exec::kind::full_fill)
            return;

        auto ts = (msg.ts.time_since_epoch().count() != 0)
                    ? msg.ts
                    : std::chrono::system_clock::now();

        double remaining = 0.0;
        if (msg.k == parsed_exec::kind::partial_fill && total_qty > 0.0)
            remaining = std::max(0.0, total_qty - tracked_cumulative);

        uint64_t fill_id;
        {
            std::lock_guard<std::mutex> lk(fills_mu_);
            fill_id = next_fill_id_++;

            fill_event fe(
                ts,
                msg.symbol,
                engine_id,
                msg.side,
                msg.last_fill_qty,
                msg.last_fill_price,
                msg.commission,
                remaining,
                fill_id
            );
            fe.set_source(fill_source::exchange);
            pending_fills_.push_back(std::move(fe));
        }
    }

    void handle_status(IFillTransport::lifecycle st, std::string_view note)
    {
        std::lock_guard<std::mutex> lk(status_mu_);
        pending_status_.push_back({st, std::string(note)});
    }

    void dispatch_unknown_fill(const parsed_exec& msg)
    {
        unknown_fill_handler handler;
        try
        {
            std::lock_guard<std::mutex> lk(handler_mu_);
            handler = unknown_fill_handler_;
        }
        catch (...)
        {
            fail_malformed_execution();
            return;
        }
        // An economic fill that cannot be tied either to an engine order or
        // to a registered venue bracket is account divergence, not benign
        // telemetry.  Leaving it unbooked would let the local ledger keep
        // trading against a state the venue has disproved.
        if (!handler || msg.exchange_order_id.empty())
        {
            fail_malformed_execution();
            return;
        }

        std::uint64_t fill_id;
        {
            std::lock_guard<std::mutex> lk(fills_mu_);
            fill_id = next_fill_id_++;
        }

        std::optional<synth_result> sr;
        try
        {
            sr = handler(msg, fill_id);
        }
        catch (...)
        {
            // The resolver may allocate while mapping a venue-managed bracket
            // leg.  An exception here cannot be allowed to escape the private
            // reader and leave local order admission live after losing an
            // economic venue event.
            fail_malformed_execution();
            return;
        }
        if (!sr)
        {
            fail_malformed_execution();
            return;
        }

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

    void fail_malformed_execution() noexcept
    {
        bool expected = false;
        if (!malformed_execution_latched_.compare_exchange_strong(
                expected, true, std::memory_order_acq_rel,
                std::memory_order_acquire))
            return;

        // Close the bridge before notifying the provider.  This makes every
        // concurrent or subsequent submit fail locally even if the provider
        // callback has not yet reached the engine thread.
        {
            std::lock_guard<std::mutex> admission_lock(venue_admission_mu_);
            close_terminal_admission();
        }
        if (d_.execution_ingress)
            d_.execution_ingress->latch_failure();
        if (d_.execution_failure_handler)
        {
            try { d_.execution_failure_handler(); }
            catch (...) {}
        }
        // Diagnostics are strictly secondary to closing admission and
        // notifying the halt owner.  This path can be entered while handling
        // an allocation failure from a parser, so it must not terminate the
        // process before the safety callback has run.
        try { set_error("ExecutionBridge: malformed private execution envelope"); }
        catch (...) {}
    }

    void attach_fill_callbacks()
    {
        std::lock_guard<std::mutex> lock(fill_callback_attachment_mu_);
        if (!d_.fill_tx || fill_callbacks_attached_)
            return;

        d_.fill_tx->set_on_message([this](std::string_view raw) {
            handle_message(raw);
        });
        d_.fill_tx->set_on_status([this](IFillTransport::lifecycle st,
                                         std::string_view note) {
            handle_status(st, note);
        });
        fill_callbacks_attached_ = true;
    }

    void detach_fill_callbacks_after_join() noexcept
    {
        std::shared_ptr<IFillTransport> fill_tx;
        {
            std::lock_guard<std::mutex> lock(fill_callback_attachment_mu_);
            if (!fill_callbacks_attached_)
                return;
            fill_callbacks_attached_ = false;
            fill_tx = d_.fill_tx;
        }
        if (!fill_tx)
            return;

        // A concrete private transport must not invoke a callback after its
        // close() returned (the reader is joined).  Clearing the callbacks at
        // that point is consequently safe and makes a later transport
        // destructor harmless even when the provider owns another reference.
        try { fill_tx->set_on_message({}); }
        catch (...) {}
        try { fill_tx->set_on_status({}); }
        catch (...) {}
        // Providers install the fatal route before bridge open.  It may close
        // over provider/bridge lifetime state just like the delivery sinks,
        // so retire it after the reader join as well.  Any explicit reopen
        // must re-arm its provider-local terminal route first.
        try { fill_tx->set_fatal_disconnect_callback({}); }
        catch (...) {}
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
    std::array<terminal_tombstone, terminal_tombstone_capacity>
        terminal_tombstones_{};

    std::mutex fills_mu_;
    std::vector<fill_event> pending_fills_;
    uint64_t next_fill_id_ = 1;

    std::mutex status_mu_;
    std::vector<status_event> pending_status_;

    mutable std::mutex error_mu_;
    std::string last_error_;

    mutable std::mutex handler_mu_;
    unknown_fill_handler unknown_fill_handler_;

    // Guards only attachment/detachment.  Private callbacks themselves never
    // take this lock; transport close joins the reader before detachment.
    std::mutex fill_callback_attachment_mu_;
    bool fill_callbacks_attached_ = false;

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
    std::atomic<bool> malformed_execution_latched_{false};

    void transport_loop();
    void process_one_submit(const submit_request& req);
    void close_terminal_admission();
};


inline bool ExecutionBridge::same_order_identity(
    const private_execution_record& left,
    const private_execution_record& right) noexcept
{
    if (!same_text(left.symbol_view(), right.symbol_view())
        || left.side != right.side)
        return false;

    const bool same_client = left.client_order_id_size != 0
        && right.client_order_id_size != 0
        && same_text(left.client_order_id_view(), right.client_order_id_view());
    const bool same_exchange = left.exchange_order_id_size != 0
        && right.exchange_order_id_size != 0
        && same_text(left.exchange_order_id_view(), right.exchange_order_id_view());
    if (!same_client && !same_exchange) return false;

    if (left.client_order_id_size != 0 && right.client_order_id_size != 0
        && !same_client)
        return false;
    if (left.exchange_order_id_size != 0 && right.exchange_order_id_size != 0
        && !same_exchange)
        return false;
    return true;
}

inline bool ExecutionBridge::same_execution_fingerprint(
    const private_execution_record& left,
    const private_execution_record& right) noexcept
{
    return same_order_identity(left, right)
        && left.k == right.k
        && left.lifecycle_only == right.lifecycle_only
        && left.cumulative_reported == right.cumulative_reported
        && same_number(left.cumulative_qty, right.cumulative_qty)
        && same_number(left.last_fill_qty, right.last_fill_qty)
        && same_number(left.last_fill_price, right.last_fill_price)
        && same_number(left.commission, right.commission)
        && same_text(left.execution_id_view(), right.execution_id_view())
        && same_text(left.commission_asset_view(), right.commission_asset_view());
}

inline bool ExecutionBridge::same_terminal_replay(
    const private_execution_record& left,
    const private_execution_record& right) noexcept
{
    if (!same_order_identity(left, right)) return false;

    // UTA-style order-channel full status is a non-economic confirmation of
    // a previously booked economic full.  It has no execution fingerprint but
    // must exactly corroborate terminal kind + cumulative truth.
    if (left.k == private_execution_record::kind::full_fill
        && right.k == private_execution_record::kind::full_fill
        && right.lifecycle_only)
    {
        return left.cumulative_reported == right.cumulative_reported
            && same_number(left.cumulative_qty, right.cumulative_qty);
    }
    if (left.k == private_execution_record::kind::full_fill
        && left.lifecycle_only
        && right.k == private_execution_record::kind::full_fill)
        return false;

    if (left.k != right.k
        || left.cumulative_reported != right.cumulative_reported
        || !same_number(left.cumulative_qty, right.cumulative_qty))
        return false;
    if (left.is_economic_fill() || right.is_economic_fill())
        return same_execution_fingerprint(left, right);
    return true;
}

inline bool ExecutionBridge::reserve_terminal_tombstone_locked(
    tracked_order& tracked, const private_execution_record& record,
    std::uint16_t& slot) noexcept
{
    if (tracked.terminal_slot != std::numeric_limits<std::uint16_t>::max()
        || record.sequence == 0)
        return false;

    for (std::size_t i = 0; i < terminal_tombstones_.size(); ++i)
    {
        auto& tombstone = terminal_tombstones_[i];
        if (tombstone.status != terminal_tombstone::state::empty) continue;
        tombstone.status = terminal_tombstone::state::reserved;
        tombstone.engine_id = tracked.engine_id;
        tombstone.sequence = record.sequence;
        tombstone.record = record;
        slot = static_cast<std::uint16_t>(i);
        return true;
    }
    return false;
}

inline void ExecutionBridge::rollback_terminal_tombstone_locked(
    std::uint16_t slot, std::uint64_t sequence) noexcept
{
    if (slot == std::numeric_limits<std::uint16_t>::max()
        || slot >= terminal_tombstones_.size())
        return;
    auto& tombstone = terminal_tombstones_[slot];
    if (tombstone.status != terminal_tombstone::state::reserved
        || tombstone.sequence != sequence)
        return;
    tombstone = terminal_tombstone{};
}

inline ExecutionBridge::terminal_tombstone*
ExecutionBridge::find_tombstone_by_sequence_locked(
    std::uint64_t sequence) noexcept
{
    for (auto& tombstone : terminal_tombstones_)
    {
        if (tombstone.status != terminal_tombstone::state::empty
            && tombstone.sequence == sequence)
            return &tombstone;
    }
    return nullptr;
}

inline const ExecutionBridge::terminal_tombstone*
ExecutionBridge::find_related_tombstone_locked(
    const private_execution_record& record) const noexcept
{
    for (const auto& tombstone : terminal_tombstones_)
    {
        if (tombstone.status == terminal_tombstone::state::empty) continue;
        const auto& prior = tombstone.record;
        const bool client_match = record.client_order_id_size != 0
            && prior.client_order_id_size != 0
            && same_text(record.client_order_id_view(),
                         prior.client_order_id_view());
        const bool exchange_match = record.exchange_order_id_size != 0
            && prior.exchange_order_id_size != 0
            && same_text(record.exchange_order_id_view(),
                         prior.exchange_order_id_view());
        if (client_match || exchange_match)
            return &tombstone;
    }
    return nullptr;
}

inline bool ExecutionBridge::exchange_id_available_locked(
    std::string_view exchange_id, std::uint64_t engine_id) const noexcept
{
    if (exchange_id.empty()) return true;
    for (const auto& [other_engine_id, tracked] : by_engine_id_)
    {
        if (other_engine_id == engine_id) continue;
        if (!tracked.exchange_id.empty()
            && same_text(tracked.exchange_id, exchange_id))
            return false;
        if (tracked.pending.active && tracked.pending.bind_exchange_id
            && tracked.pending.record.exchange_order_id_size != 0
            && same_text(tracked.pending.record.exchange_order_id_view(),
                         exchange_id))
            return false;
    }
    for (const auto& tombstone : terminal_tombstones_)
    {
        if (tombstone.status == terminal_tombstone::state::empty
            || tombstone.engine_id == engine_id
            || tombstone.record.exchange_order_id_size == 0)
            continue;
        if (same_text(tombstone.record.exchange_order_id_view(), exchange_id))
            return false;
    }
    return true;
}

inline private_execution_resolution
ExecutionBridge::resolve_private_execution(private_execution_record& record)
{
    using resolution = private_execution_resolution;
    bool fatal = false;
    resolution result = resolution::fatal;

    {
        std::lock_guard<std::mutex> lock(map_mu_);
        if (!record.valid_shape()
            || record.k == private_execution_record::kind::fatal
            || record.k == private_execution_record::kind::unknown_lifecycle
            || record.k == private_execution_record::kind::funding
            || record.k == private_execution_record::kind::bracket_group_active
            || record.k == private_execution_record::kind::bracket_group_completed)
        {
            fatal = true;
        }
        else if (const auto* tombstone = find_related_tombstone_locked(record))
        {
            result = same_terminal_replay(tombstone->record, record)
                ? resolution::duplicate : resolution::fatal;
            fatal = result == resolution::fatal;
        }
        else
        {
            auto tracked_it = by_engine_id_.end();
            if (record.client_order_id_size != 0)
            {
                // std::unordered_map<std::string,...>::find would require a
                // transient allocating string on this engine-thread hot
                // boundary unless the map is rebuilt with transparent
                // lookup.  The tracked-order set is bounded by order limits;
                // retain an allocation-free exact scan here.
                for (auto it = by_engine_id_.begin();
                     it != by_engine_id_.end(); ++it)
                {
                    if (same_text(it->second.client_id,
                                  record.client_order_id_view()))
                    {
                        tracked_it = it;
                        break;
                    }
                }
            }
            if (record.exchange_order_id_size != 0)
            {
                for (auto it = by_engine_id_.begin();
                     it != by_engine_id_.end(); ++it)
                {
                    const auto& candidate = it->second;
                    const bool matches_committed = !candidate.exchange_id.empty()
                        && same_text(candidate.exchange_id,
                                     record.exchange_order_id_view());
                    const bool matches_pending = candidate.pending.active
                        && candidate.pending.bind_exchange_id
                        && candidate.pending.record.exchange_order_id_size != 0
                        && same_text(candidate.pending.record.exchange_order_id_view(),
                                     record.exchange_order_id_view());
                    if (!matches_committed && !matches_pending) continue;
                    if (tracked_it != by_engine_id_.end()
                        && tracked_it != it)
                    {
                        fatal = true;
                        break;
                    }
                    tracked_it = it;
                }
            }

            // Unrecognised authenticated execution truth cannot be treated
            // as a generic "foreign" fill.  A future typed native-bracket
            // registry may explicitly claim such records; until then the
            // bridge closes admission rather than lose their attribution.
            if (!fatal && tracked_it == by_engine_id_.end())
            {
                fatal = true;
            }
            else if (!fatal)
            {
                auto& tracked = tracked_it->second;
                if (!same_text(tracked.symbol, record.symbol_view())
                    || tracked.side != record.side
                    || (record.exchange_order_id_size != 0
                        && !tracked.exchange_id.empty()
                        && !same_text(tracked.exchange_id,
                                      record.exchange_order_id_view())))
                {
                    fatal = true;
                }
                else if (tracked.pending.active)
                {
                    // The engine has not yet committed or rolled back the
                    // preceding FIFO transition.  It may be replayed by the
                    // venue, but a distinct transition cannot overtake it.
                    result = same_execution_fingerprint(tracked.pending.record,
                                                        record)
                        ? resolution::duplicate : resolution::fatal;
                    fatal = result == resolution::fatal;
                }
                else if (tracked.state
                    == tracked_order::lifecycle::private_terminal_enqueued)
                {
                    result = same_terminal_replay(tracked.terminal_record,
                                                  record)
                        ? resolution::duplicate : resolution::fatal;
                    fatal = result == resolution::fatal;
                }
                else
                {
                    const bool bind_exchange_id = tracked.exchange_id.empty()
                        && record.exchange_order_id_size != 0;
                    if (bind_exchange_id
                        && !exchange_id_available_locked(
                            record.exchange_order_id_view(), tracked.engine_id))
                    {
                        fatal = true;
                    }

                    const auto prepare = [&](double next_cumulative,
                                             bool append_history,
                                             bool terminal) noexcept {
                        record.engine_order_id = tracked.engine_id;
                        record.remaining_qty = std::max(
                            0.0, tracked.total_qty - next_cumulative);
                        auto& pending = tracked.pending;
                        pending.active = true;
                        pending.append_execution_history = append_history;
                        pending.terminal = terminal;
                        pending.bind_exchange_id = bind_exchange_id;
                        pending.sequence = record.sequence;
                        pending.next_cumulative_qty = next_cumulative;
                        pending.record = record;
                        pending.terminal_slot =
                            std::numeric_limits<std::uint16_t>::max();
                        if (terminal
                            && !reserve_terminal_tombstone_locked(
                                tracked, record, pending.terminal_slot))
                        {
                            pending = {};
                            fatal = true;
                            return;
                        }
                        result = resolution::tracked;
                    };

                    if (!fatal && record.is_economic_fill())
                    {
                        if (record.lifecycle_only)
                        {
                            const bool matches_cumulative =
                                record.cumulative_reported
                                && same_number(record.cumulative_qty,
                                               tracked.cumulative_qty);
                            const bool terminal_truth =
                                record.k != private_execution_record::kind::full_fill
                                || same_number(record.cumulative_qty,
                                               tracked.total_qty);
                            if (!matches_cumulative || !terminal_truth)
                            {
                                fatal = true;
                            }
                            else if (record.k
                                     == private_execution_record::kind::full_fill)
                            {
                                // Zero-delta full confirmation is terminal
                                // lifecycle truth. It reserves retirement but
                                // does not advance economic cumulative state.
                                prepare(tracked.cumulative_qty,
                                        /*append_history=*/false,
                                        /*terminal=*/true);
                            }
                            else
                            {
                                result = resolution::duplicate;
                            }
                        }
                        else
                        {
                            bool duplicate_execution = false;
                            for (std::size_t i = 0;
                                 i < tracked.execution_history_size; ++i)
                            {
                                const auto& prior = tracked.execution_history[i];
                                if (!same_text(prior.execution_id_view(),
                                               record.execution_id_view()))
                                    continue;
                                result = same_execution_fingerprint(prior, record)
                                    ? resolution::duplicate : resolution::fatal;
                                duplicate_execution = true;
                                fatal = result == resolution::fatal;
                                break;
                            }

                            if (!duplicate_execution && !fatal)
                            {
                                const double next_cumulative =
                                    tracked.cumulative_qty + record.last_fill_qty;
                                if (!record.cumulative_reported
                                    || !same_number(record.cumulative_qty,
                                                    next_cumulative)
                                    || record.cumulative_qty > tracked.total_qty
                                    || tracked.execution_history_size
                                        == tracked.execution_history.size()
                                    || (record.k
                                            == private_execution_record::kind::full_fill
                                        && !same_number(record.cumulative_qty,
                                                        tracked.total_qty)))
                                {
                                    fatal = true;
                                }
                                else
                                {
                                    prepare(next_cumulative,
                                            /*append_history=*/true,
                                            /*terminal=*/record.is_terminal());
                                }
                            }
                        }
                    }
                    else if (!fatal)
                    {
                        const bool terminal = record.is_terminal();
                        if ((terminal && !record.cumulative_reported)
                            || (record.cumulative_reported
                                && !same_number(record.cumulative_qty,
                                                tracked.cumulative_qty)))
                        {
                            fatal = true;
                        }
                        else
                        {
                            prepare(tracked.cumulative_qty,
                                    /*append_history=*/false, terminal);
                        }
                    }
                }
            }
        }
    }

    if (fatal)
    {
        fail_malformed_execution();
        return resolution::fatal;
    }
    return result;
}

inline bool ExecutionBridge::commit_private_execution(
    const private_execution_reservation& reservation)
{
    if (!reservation.valid()) return false;
    std::lock_guard<std::mutex> lock(map_mu_);
    const auto tracked_it = by_engine_id_.find(reservation.engine_order_id);
    if (tracked_it == by_engine_id_.end()) return false;
    auto& tracked = tracked_it->second;
    auto& pending = tracked.pending;
    if (!pending.active || pending.sequence != reservation.source_sequence)
        return false;

    if (pending.bind_exchange_id)
    {
        if (pending.record.exchange_order_id_size == 0
            || !tracked.exchange_id.empty()
            || !exchange_id_available_locked(
                pending.record.exchange_order_id_view(), tracked.engine_id))
            return false;
        tracked.exchange_id.assign(pending.record.exchange_order_id_view());
    }
    if (pending.append_execution_history)
    {
        if (tracked.execution_history_size == tracked.execution_history.size())
            return false;
        tracked.cumulative_qty = pending.next_cumulative_qty;
        tracked.execution_history[tracked.execution_history_size++] = pending.record;
    }
    if (pending.terminal)
    {
        if (pending.terminal_slot == std::numeric_limits<std::uint16_t>::max()
            || pending.terminal_slot >= terminal_tombstones_.size())
            return false;
        const auto& tombstone = terminal_tombstones_[pending.terminal_slot];
        if (tombstone.status != terminal_tombstone::state::reserved
            || tombstone.engine_id != tracked.engine_id
            || tombstone.sequence != pending.sequence)
            return false;
        tracked.terminal_slot = pending.terminal_slot;
        tracked.terminal_sequence = pending.sequence;
        tracked.terminal_record = pending.record;
        tracked.state = tracked_order::lifecycle::private_terminal_enqueued;
    }
    pending = {};
    return true;
}

inline bool ExecutionBridge::rollback_private_execution(
    const private_execution_reservation& reservation) noexcept
{
    if (!reservation.valid()) return false;
    std::lock_guard<std::mutex> lock(map_mu_);
    const auto tracked_it = by_engine_id_.find(reservation.engine_order_id);
    if (tracked_it == by_engine_id_.end()) return false;
    auto& pending = tracked_it->second.pending;
    if (!pending.active || pending.sequence != reservation.source_sequence)
        return false;
    if (pending.terminal)
        rollback_terminal_tombstone_locked(pending.terminal_slot,
                                          pending.sequence);
    pending = {};
    return true;
}

inline bool ExecutionBridge::acknowledge_private_terminal(
    std::uint64_t sequence)
{
    std::lock_guard<std::mutex> lock(map_mu_);
    auto* tombstone = find_tombstone_by_sequence_locked(sequence);
    if (!tombstone) return false;
    if (tombstone->status == terminal_tombstone::state::committed)
        return true;
    if (tombstone->status != terminal_tombstone::state::reserved)
        return false;

    auto tracked_it = by_engine_id_.find(tombstone->engine_id);
    if (tracked_it == by_engine_id_.end()
        || tracked_it->second.state
            != tracked_order::lifecycle::private_terminal_enqueued
        || tracked_it->second.pending.active
        || tracked_it->second.terminal_sequence != sequence)
        return false;

    const auto client_id = tracked_it->second.client_id;
    by_engine_id_.erase(tracked_it);
    if (!client_id.empty()) by_client_id_.erase(client_id);
    tombstone->status = terminal_tombstone::state::committed;
    return true;
}

inline bool ExecutionBridge::check_private_lifecycle_deadline()
{
    bool expired = false;
    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(map_mu_);
        for (const auto& [_, tracked] : by_engine_id_)
        {
            if (tracked.state == tracked_order::lifecycle::rest_cancel_acked
                && tracked.cancel_confirmation_deadline !=
                    std::chrono::steady_clock::time_point{}
                && now >= tracked.cancel_confirmation_deadline)
            {
                expired = true;
                break;
            }
        }
    }
    if (expired) fail_malformed_execution();
    return !expired;
}

inline bool ExecutionBridge::has_unresolved_private_lifecycle() const
{
    std::lock_guard<std::mutex> lock(map_mu_);
    // A successful REST kill/cancel is advisory.  At shutdown every active
    // mapping still needs source-of-truth private terminal evidence (or an
    // explicit reconciled replacement) before the authoritative ledger may
    // be published.  Pending prepares are likewise unresolved until commit
    // or rollback; a compact state subset here would reopen the old
    // REST-acknowledged-but-private-silent escape hatch.
    return !by_engine_id_.empty();
}


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
        || !accepting_orders_.load(std::memory_order_acquire)) return;

    if (req.is_cancel)
    {
        std::string symbol = req.symbol;
        std::string exchange_id = req.cancel_exchange_id;
        std::string client_id = req.client_id;
        bool cancellation_still_authorized = false;

        {
            std::lock_guard<std::mutex> lk(map_mu_);
            auto it = by_engine_id_.find(req.engine_id);
            if (it == by_engine_id_.end()
                || it->second.state != tracked_order::lifecycle::cancel_requested)
                return;

            if (!it->second.symbol.empty()) symbol = it->second.symbol;
            if (!it->second.exchange_id.empty()) exchange_id = it->second.exchange_id;
            if (!it->second.client_id.empty()) client_id = it->second.client_id;
            cancellation_still_authorized = true;
        }
        if (!cancellation_still_authorized) return;

        if (d_.order_rate_limiter
            && !d_.order_rate_limiter->acquire_interruptibly(
                transport_running_, 1.0))
            return;

        auto enc = d_.encoder->encode_cancel(symbol, exchange_id, client_id);
        IOrderTransport::result res;
        {
            std::lock_guard<std::mutex> admission_lock(venue_admission_mu_);
            if (!accepting_orders_.load(std::memory_order_acquire)) return;
            res = d_.order_tx->cancel(enc.endpoint, enc.wire_payload);
            if (res.uncertain || res.fatal) close_terminal_admission();
        }

        bool cancel_response_identity_conflict = false;
        if (!res.ok) {
            set_error("cancel failed: " + res.error);
            // A definitive REST failure means the requested cancellation did
            // not transition venue state.  Keep the private identity and
            // return to active so an explicit later operator action can make
            // a new request.  Ambiguous/fatal outcomes intentionally retain
            // cancel_requested: their truth is no longer locally knowable.
            if (!res.uncertain && !res.fatal)
            {
                std::lock_guard<std::mutex> lk(map_mu_);
                const auto it = by_engine_id_.find(req.engine_id);
                if (it != by_engine_id_.end()
                    && it->second.state
                        == tracked_order::lifecycle::cancel_requested)
                {
                    it->second.state = tracked_order::lifecycle::active;
                    it->second.cancel_confirmation_deadline = {};
                }
            }
        }
        else
        {
            std::lock_guard<std::mutex> lk(map_mu_);
            auto it = by_engine_id_.find(req.engine_id);
            if (it == by_engine_id_.end())
            {
                // A private terminal may have won the race and been
                // acknowledged already.  Its immutable tombstone still
                // proves whether this delayed REST response names the same
                // venue order; anything else is an identity contradiction.
                const auto tombstone = std::find_if(
                    terminal_tombstones_.begin(), terminal_tombstones_.end(),
                    [&req](const terminal_tombstone& candidate) {
                        return candidate.status
                                != terminal_tombstone::state::empty
                            && candidate.engine_id == req.engine_id;
                    });
                if (tombstone == terminal_tombstones_.end()
                    || (!res.exchange_order_id.empty()
                        && tombstone->record.exchange_order_id_size != 0
                        && !same_text(res.exchange_order_id,
                                      tombstone->record.exchange_order_id_view())))
                {
                    res.ok = false;
                    res.uncertain = true;
                    res.fatal = true;
                    res.error = "cancel response lost private lifecycle identity";
                    cancel_response_identity_conflict = true;
                }
            }
            else if (it->second.state == tracked_order::lifecycle::cancel_requested)
            {
                if (!res.exchange_order_id.empty()
                    && !it->second.exchange_id.empty()
                    && it->second.exchange_id != res.exchange_order_id)
                {
                    res.ok = false;
                    res.uncertain = true;
                    res.fatal = true;
                    res.error = "conflicting exchange order id from cancel response";
                    cancel_response_identity_conflict = true;
                }
                else
                {
                    if (it->second.exchange_id.empty()
                        && !res.exchange_order_id.empty())
                    {
                        if (!exchange_id_available_locked(
                                res.exchange_order_id, req.engine_id))
                        {
                            res.ok = false;
                            res.uncertain = true;
                            res.fatal = true;
                            res.error = "duplicate exchange order id from cancel response";
                            cancel_response_identity_conflict = true;
                        }
                        else
                        {
                            it->second.exchange_id = res.exchange_order_id;
                        }
                    }
                    if (!cancel_response_identity_conflict)
                    {
                        it->second.state = tracked_order::lifecycle::rest_cancel_acked;
                        it->second.cancel_confirmation_deadline =
                            std::chrono::steady_clock::now()
                            + private_cancel_confirmation_deadline;
                    }
                }
            }
            else if (it->second.state != tracked_order::lifecycle::private_terminal_enqueued)
            {
                // A REST response cannot advance an order lifecycle we did
                // not request.  Keep the mapping intact for reconciliation.
                res.ok = false;
                res.uncertain = true;
                res.fatal = true;
                res.error = "unexpected cancel response lifecycle state";
                cancel_response_identity_conflict = true;
            }
        }
        if (cancel_response_identity_conflict)
            fail_malformed_execution();

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
        if (!accepting_orders_.load(std::memory_order_acquire)) return;
        res = d_.order_tx->submit(req.endpoint, req.wire_payload);
        if (res.uncertain || res.fatal) close_terminal_admission();
    }

    if (!res.ok && !res.uncertain && !res.fatal) {
        set_error("submit failed: " + res.error);
        std::lock_guard<std::mutex> lk(map_mu_);
        const auto it = by_engine_id_.find(req.engine_id);
        if (d_.execution_ingress)
        {
            // A private fill/status can have been admitted while the REST
            // call was blocked.  Preserve the identity instead of turning
            // that future source-of-truth record into an untracked drop.
            if (it != by_engine_id_.end()
                && it->second.state == tracked_order::lifecycle::active)
                it->second.state = tracked_order::lifecycle::rest_submit_failed;
        }
        else
        {
            by_engine_id_.erase(req.engine_id);
            if (!req.client_id.empty())
                by_client_id_.erase(req.client_id);
        }
    }

    bool response_identity_conflict = false;
    if (res.ok && !res.exchange_order_id.empty())
    {
        std::lock_guard<std::mutex> lk(map_mu_);
        auto it = by_engine_id_.find(req.engine_id);
        if (it != by_engine_id_.end())
        {
            if (!it->second.exchange_id.empty()
                && it->second.exchange_id != res.exchange_order_id)
            {
                response_identity_conflict = true;
            }
            else
            {
                if (!exchange_id_available_locked(res.exchange_order_id,
                                                  req.engine_id))
                    response_identity_conflict = true;
                else
                    it->second.exchange_id = res.exchange_order_id;
            }
        }
        else
        {
            // The private reader can publish and the engine can acknowledge
            // a terminal fill while the synchronous REST submit is still
            // blocked.  Do not let that map retirement erase the one chance
            // to compare the REST and private venue identities.
            const auto tombstone = std::find_if(
                terminal_tombstones_.begin(), terminal_tombstones_.end(),
                [&req](const terminal_tombstone& candidate) {
                    return candidate.status
                            != terminal_tombstone::state::empty
                        && candidate.engine_id == req.engine_id;
                });
            response_identity_conflict =
                tombstone == terminal_tombstones_.end()
                || (tombstone->record.exchange_order_id_size != 0
                    && !same_text(res.exchange_order_id,
                                  tombstone->record.exchange_order_id_view()));
        }
    }

    if (response_identity_conflict)
    {
        res.ok = false;
        res.uncertain = true;
        res.fatal = true;
        res.error = "conflicting exchange order id from order transport";
        fail_malformed_execution();
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
