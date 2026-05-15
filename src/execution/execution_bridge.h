#pragma once

#include "execution_adapter.h"
#include "order_transport.h"
#include "fill_transport.h"
#include "order_encoder.h"
#include "fill_parser.h"
#include "rate_limiter.h"
#include "../core/event.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

class ExecutionBridge : public IExecutionAdapter
{
public:
    struct deps
    {
        std::shared_ptr<IOrderTransport> order_tx;
        std::shared_ptr<IFillTransport>  fill_tx;
        std::shared_ptr<IOrderEncoder>   encoder;
        std::shared_ptr<IFillParser>     parser;

        // Optional. If set, each submit consults it before sending and
        // blocks until a token is available — gates the venue's order-rate
        // cap (Binance spot: 50 orders / 10s). Left null, submit is ungated.
        std::shared_ptr<TokenBucketRateLimiter> order_rate_limiter;

        // Optional. Called per submit to produce the clientOrderId used for
        // exchange-side idempotency and as the bridge's internal key. When
        // null, the bridge falls back to "tt-<engine_order_id>", which is
        // only unique within a single process lifetime.
        std::function<std::string(uint64_t engine_order_id)> client_id_fn;

        // Optional. Fires when the parser declines the message via parse()
        // but accepts it via parse_position_snapshot() — i.e. server-pushed
        // position/balance state changes (futures ACCOUNT_UPDATE). Runs on
        // the fill transport's worker thread; must be thread-safe.
        std::function<void(const parsed_position_snapshot&)>
            position_snapshot_handler;
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
    // (engine_order_id → opener_order_id + strategy) metadata for the
    // engine to register in its order_meta_ on the main thread.
    // Handler runs on the fill transport's worker thread → must be
    // thread-safe. ExitManager's venue lookups are mutex-guarded; the
    // engine-side OrderIdGenerator::next() is atomic.
    struct synth_result
    {
        fill_event    fill;
        std::uint64_t opener_order_id = 0;
        std::string   strategy_name;
    };
    using unknown_fill_handler =
        std::function<std::optional<synth_result>(const parsed_exec&,
                                                  std::uint64_t fill_id)>;

    struct synth_meta
    {
        std::uint64_t engine_order_id  = 0;
        std::uint64_t opener_order_id  = 0;
        std::string   strategy_name;
    };

    void set_unknown_fill_handler(unknown_fill_handler h)
    {
        std::lock_guard<std::mutex> lk(handler_mu_);
        unknown_fill_handler_ = std::move(h);
    }

    // Engine drains this BEFORE poll_fills so order_meta_ has the
    // mapping ready when lookup_opener fires inside the fill loop.
    bool poll_synth_meta(std::vector<synth_meta>& out)
    {
        std::lock_guard<std::mutex> lk(synth_mu_);
        if (pending_synth_meta_.empty()) return false;
        out.insert(out.end(),
                   std::make_move_iterator(pending_synth_meta_.begin()),
                   std::make_move_iterator(pending_synth_meta_.end()));
        pending_synth_meta_.clear();
        return true;
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
        return true;
    }

    void close()
    {
        if (d_.fill_tx)  d_.fill_tx->close();
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

        if (d_.order_rate_limiter)
            d_.order_rate_limiter->acquire_blocking(1.0);

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

        {
            std::lock_guard<std::mutex> lk(map_mu_);
            by_engine_id_[t.engine_id] = t;
            by_client_id_[t.client_id] = t.engine_id;
        }

        auto res = d_.order_tx->submit(enc.endpoint, enc.wire_payload);
        if (!res.ok)
        {
            set_error("submit failed: " + res.error);
            std::lock_guard<std::mutex> lk(map_mu_);
            by_engine_id_.erase(t.engine_id);
            by_client_id_.erase(t.client_id);
            return;
        }

        if (!res.exchange_order_id.empty())
        {
            std::lock_guard<std::mutex> lk(map_mu_);
            auto it = by_engine_id_.find(t.engine_id);
            if (it != by_engine_id_.end())
                it->second.exchange_id = res.exchange_order_id;
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

        if (d_.order_rate_limiter)
            d_.order_rate_limiter->acquire_blocking(1.0);

        auto enc = d_.encoder->encode_cancel(symbol, exchange_id, client_id);
        auto res = d_.order_tx->cancel(enc.endpoint, enc.wire_payload);
        if (!res.ok)
        {
            set_error("cancel failed: " + res.error);
            return false;
        }
        return true;
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

    std::string last_error() const
    {
        std::lock_guard<std::mutex> lk(error_mu_);
        return last_error_;
    }

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

            if (msg.k == parsed_exec::kind::full_fill   ||
                msg.k == parsed_exec::kind::canceled    ||
                msg.k == parsed_exec::kind::rejected    ||
                msg.k == parsed_exec::kind::expired)
            {
                by_client_id_.erase(msg.client_order_id);
                by_engine_id_.erase(engine_id);
            }
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
        // — the meta is shorter-lived (drained before fills each tick).
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
};
