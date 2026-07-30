#pragma once

#include "execution_adapter.h"
#include "order_transport.h"
#include "fill_transport.h"
#include "order_encoder.h"
#include "fill_parser.h"
#include "rate_limiter.h"
#include "async_support.h"
#include "../core/event.h"

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
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
        // Revoke the handler immediately so that any in-flight or late
        // dispatch_unknown_fill on the worker cannot observe a stale
        // functor after close returns.
        clear_unknown_fill_handler();

        // Stop transport thread first so it can drain / finish I/O cleanly.
        transport_running_.store(false, std::memory_order_release);
        if (transport_thread_.joinable())
            transport_thread_.join();

        // Drop fill-tx callbacks before close so late/reused transport
        // messages cannot call into a destroyed bridge (ctor captures
        // [this]). Destructor calls close() first; this is the revoke
        // path for both explicit close and dtor teardown.
        if (d_.fill_tx)
        {
            d_.fill_tx->set_on_message({});
            d_.fill_tx->set_on_status({});
            d_.fill_tx->close();
        }
        if (d_.order_tx) d_.order_tx->close();
    }

    void submit_order(const order_event& o) override
    {
        clear_error();

        if (!d_.encoder || !d_.order_tx)
        {
            set_error("ExecutionBridge: not configured");
            return;
        }

        const std::string client_id = d_.client_id_fn
            ? d_.client_id_fn(o.get_order_id())
            : make_client_id(o.get_order_id());
        auto enc = d_.encoder->encode_submit(o, client_id);

        // Empty encode (stop types, bad clientOid, missing symbol) must not
        // POST an empty path or leave a phantom tracked order.
        if (enc.endpoint.empty())
        {
            set_error("ExecutionBridge: encoder refused submit (empty endpoint)");
            submit_result sr;
            sr.engine_id = o.get_order_id();
            sr.symbol = o.get_symbol();
            sr.error = "encoder refused submit (empty endpoint)";
            sr.op = submit_result::operation::submit;
            sr.ok = false;
            {
                std::lock_guard<std::mutex> lk(submit_results_mu_);
                pending_submit_results_.push_back(std::move(sr));
            }
            return;
        }

        tracked_order t;
        t.engine_id = o.get_order_id();
        t.client_id = client_id;
        t.symbol    = o.get_symbol();
        t.side      = o.get_side();
        t.total_qty = o.get_quantity();

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
            submit_queue_.push_back(std::move(req));
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
        std::string exchange_id, symbol, client_id;
        {
            std::lock_guard<std::mutex> lk(map_mu_);
            auto it = by_engine_id_.find(engine_order_id);
            if (it == by_engine_id_.end()) return false;
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
        uint64_t    engine_id     = 0;
        std::string client_id;
        std::string exchange_id;
        std::string symbol;
        order_side  side           = order_side::buy;
        double      total_qty      = 0.0;
        double      cumulative_qty = 0.0;
    };

    void handle_message(std::string_view raw)
    {
        if (!d_.parser) return;
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

        uint64_t engine_id = 0;
        double total_qty = 0.0;
        double tracked_cumulative = 0.0;
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

            if (!msg.exchange_order_id.empty() && eit->second.exchange_id.empty())
                eit->second.exchange_id = msg.exchange_order_id;

            if (msg.k == parsed_exec::kind::partial_fill ||
                msg.k == parsed_exec::kind::full_fill)
            {
                eit->second.cumulative_qty += msg.last_fill_qty;
            }

            total_qty = eit->second.total_qty;
            tracked_cumulative = eit->second.cumulative_qty;

            // Complete when venue says full_fill OR cumulative covers the
            // tracked total (fill-channel venues that never emit full_fill,
            // and dual-channel paths that demote order-channel filled→other).
            constexpr double k_qty_eps = 1e-12;
            const bool qty_complete =
                (total_qty > 0.0
                 && tracked_cumulative + k_qty_eps >= total_qty);

            if (msg.k == parsed_exec::kind::full_fill   ||
                msg.k == parsed_exec::kind::canceled    ||
                msg.k == parsed_exec::kind::rejected    ||
                msg.k == parsed_exec::kind::expired     ||
                qty_complete)
            {
                by_client_id_.erase(msg.client_order_id);
                by_engine_id_.erase(engine_id);
            }
        }

        if (msg.k != parsed_exec::kind::partial_fill &&
            msg.k != parsed_exec::kind::full_fill)
            return;

        // Venue-agnostic: order-status channels often emit full_fill /
        // partial_fill with last_fill_qty==0 (lifecycle only). Skip
        // zero-qty fill_event so dual-channel venues do not invent fills.
        // (Untrack for terminal/qty-complete already ran above.)
        if (msg.last_fill_qty <= 0.0)
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

    std::mutex fills_mu_;
    std::vector<fill_event> pending_fills_;
    uint64_t next_fill_id_ = 1;

    std::mutex status_mu_;
    std::vector<status_event> pending_status_;

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

    std::mutex submit_results_mu_;
    std::vector<submit_result> pending_submit_results_;

    std::thread transport_thread_;
    std::atomic<bool> transport_running_{false};

    void transport_loop();
    void process_one_submit(const submit_request& req);
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
                submit_result sr;
                sr.engine_id = req.engine_id;
                sr.symbol = req.symbol;
                sr.error = std::string("exception: ") + e.what();
                sr.op = req.is_cancel
                    ? submit_result::operation::cancel
                    : submit_result::operation::submit;
                sr.ok = false;
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

inline void ExecutionBridge::process_one_submit(const submit_request& req)
{
    if (!d_.order_tx || !d_.encoder) return;

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

        if (d_.order_rate_limiter)
            d_.order_rate_limiter->acquire_blocking(1.0);

        auto enc = d_.encoder->encode_cancel(symbol, exchange_id, client_id);
        auto res = d_.order_tx->cancel(enc.endpoint, enc.wire_payload);

        if (!res.ok) {
            set_error("cancel failed: " + res.error);
        }
        else
        {
            std::lock_guard<std::mutex> lk(map_mu_);
            by_engine_id_.erase(req.engine_id);
            if (!client_id.empty())
                by_client_id_.erase(client_id);
        }

        submit_result sr;
        sr.engine_id = req.engine_id;
        sr.symbol = symbol;
        sr.exchange_order_id = res.exchange_order_id;
        sr.error = res.ok ? "" : res.error;
        sr.op = submit_result::operation::cancel;
        sr.ok = res.ok;
        {
            std::lock_guard<std::mutex> lk(submit_results_mu_);
            pending_submit_results_.push_back(std::move(sr));
        }
        return;
    }

    if (req.endpoint.empty())
    {
        set_error("submit failed: empty endpoint");
        {
            std::lock_guard<std::mutex> lk(map_mu_);
            by_engine_id_.erase(req.engine_id);
            if (!req.client_id.empty())
                by_client_id_.erase(req.client_id);
        }
        submit_result sr;
        sr.engine_id = req.engine_id;
        sr.symbol = req.symbol;
        sr.error = "empty endpoint";
        sr.op = submit_result::operation::submit;
        sr.ok = false;
        {
            std::lock_guard<std::mutex> lk(submit_results_mu_);
            pending_submit_results_.push_back(std::move(sr));
        }
        return;
    }

    if (d_.order_rate_limiter)
        d_.order_rate_limiter->acquire_blocking(1.0);

    auto res = d_.order_tx->submit(req.endpoint, req.wire_payload);

    if (!res.ok) {
        set_error("submit failed: " + res.error);
        std::lock_guard<std::mutex> lk(map_mu_);
        by_engine_id_.erase(req.engine_id);
        if (!req.client_id.empty())
            by_client_id_.erase(req.client_id);
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
