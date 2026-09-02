#include "order_audit_sink.h"

#include <cstdio>

// Implementation of IOrderAuditSink seam (see core/docs/internal/engine-decomposition.md Phase 2 E-21 + engine-decomposition skill).
// Engine never bypasses this for recording.

#ifdef HAS_QUESTDB
QuestdbOrderAuditSink::QuestdbOrderAuditSink(std::shared_ptr<truetest::questdb::QuestdbStore> store, bool* active_flag)
    : store_(std::move(store))
    , active_flag_(active_flag)
{
}

void QuestdbOrderAuditSink::record_order_submitted(const order_event& o, const char* initial_status)
{
    if (store_ && active_flag_ && *active_flag_)
    {
        store_->record_order_submitted(o, initial_status ? std::string(initial_status) : std::string{});
    }
}

void QuestdbOrderAuditSink::record_status_transition(uint64_t order_id,
                                                     order_status old_s,
                                                     order_status new_s,
                                                     const char* reason)
{
    if (store_ && active_flag_ && *active_flag_)
    {
        store_->record_status_transition(order_id, old_s, new_s,
                                         reason ? std::string(reason) : std::string{});
    }
}

void QuestdbOrderAuditSink::record_fill(const fill_event& f,
                                        uint64_t opener_order_id,
                                        const char* strategy_name,
                                        const char* source)
{
    if (store_ && active_flag_ && *active_flag_)
    {
        store_->record_fill(f, opener_order_id,
                            strategy_name ? std::string(strategy_name) : std::string{},
                            source ? std::string(source) : std::string{});
    }
}

void QuestdbOrderAuditSink::record_rejection(const order_event& o,
                                             const char* category,
                                             const char* detail)
{
    ++total_rejections_;
    if (store_ && active_flag_ && *active_flag_)
    {
        store_->record_rejection(o,
                                 category ? std::string(category) : std::string{},
                                 detail ? std::string(detail) : std::string{});
    }
}

void QuestdbOrderAuditSink::record_cancellation(uint64_t order_id,
                                                const char* symbol,
                                                const char* strategy_name,
                                                const char* reason)
{
    if (store_ && active_flag_ && *active_flag_)
    {
        store_->record_cancellation(order_id,
                                    symbol ? std::string(symbol) : std::string{},
                                    strategy_name ? std::string(strategy_name) : std::string{},
                                    reason ? std::string(reason) : std::string{});
    }
}

void QuestdbOrderAuditSink::record_amendment(uint64_t order_id,
                                             const char* symbol,
                                             double old_price, double new_price,
                                             double old_qty, double new_qty,
                                             std::chrono::system_clock::time_point ts)
{
    if (store_ && active_flag_ && *active_flag_)
    {
        store_->record_amendment(order_id,
                                 symbol ? std::string(symbol) : std::string{},
                                 old_price, new_price, old_qty, new_qty, ts);
    }
}

void QuestdbOrderAuditSink::record_funding(const funding_event& fe, const char* run_tag)
{
    if (store_ && active_flag_ && *active_flag_)
    {
        store_->record_funding(fe, run_tag ? std::string(run_tag) : std::string{});
    }
}

void QuestdbOrderAuditSink::record_event(const char* event_type,
                                         const char* symbol,
                                         const char* strategy_name,
                                         uint64_t order_id,
                                         const char* severity,
                                         const char* message,
                                         const char* details_json)
{
    if (store_ && active_flag_ && *active_flag_)
    {
        store_->record_event(event_type ? std::string(event_type) : std::string{},
                             symbol ? std::string(symbol) : std::string{},
                             strategy_name ? std::string(strategy_name) : std::string{},
                             order_id,
                             severity ? std::string(severity) : std::string{},
                             message ? std::string(message) : std::string{},
                             details_json ? std::string(details_json) : std::string{});
    }
}

void QuestdbOrderAuditSink::record_exit_lifecycle(
    const exit_lifecycle_record& record)
{
    if (!store_ || !active_flag_ || !*active_flag_)
        return;
    char details[640];
    const int length = std::snprintf(details, sizeof(details),
        "{\"signal_id\":%llu,\"order_id\":%llu,\"opener_order_id\":%llu,\"fill_id\":%llu,"
        "\"decision_ts_ns\":%lld,\"submit_ts_ns\":%lld,\"eligible_ts_ns\":%lld,\"fill_ts_ns\":%lld,"
        "\"requested_qty\":%.12g,\"filled_qty\":%.12g,\"remaining_qty\":%.12g,"
        "\"exit_reason\":%u,\"state_before\":\"%s\",\"state_after\":\"%s\",\"risk_outcome\":\"%s\"}",
        static_cast<unsigned long long>(record.signal_id),
        static_cast<unsigned long long>(record.order_id),
        static_cast<unsigned long long>(record.opener_order_id),
        static_cast<unsigned long long>(record.fill_id),
        static_cast<long long>(record.decision_ts.time_since_epoch().count()),
        static_cast<long long>(record.submit_ts.time_since_epoch().count()),
        static_cast<long long>(record.eligible_ts.time_since_epoch().count()),
        static_cast<long long>(record.fill_ts.time_since_epoch().count()),
        record.requested_qty, record.filled_qty, record.remaining_qty,
        static_cast<unsigned>(record.reason), to_string(record.state_before),
        to_string(record.state_after),
        record.risk_outcome ? record.risk_outcome : "");
    const char* encoded = length >= 0
            && static_cast<std::size_t>(length) < sizeof(details)
        ? details : "{\"encoding_error\":\"bounded exit lifecycle overflow\"}";
    record_event("exit_lifecycle", record.symbol, record.strategy_name,
                 record.order_id, record.phase,
                 "exit lifecycle transition", encoded);
}

std::size_t QuestdbOrderAuditSink::total_rejections() const
{
    return total_rejections_;
}

const char* QuestdbOrderAuditSink::run_tag() const
{
    if (store_ && active_flag_ && *active_flag_)
        return store_->run_tag().c_str();
    return "";
}

IOrderAuditSink::Health QuestdbOrderAuditSink::health() const
{
    if (store_ && active_flag_ && *active_flag_)
    {
        auto sh = store_->health();
        return { sh.connected, sh.pending_lines, sh.dropped_lines, sh.fallback_lines };
    }
    return {};
}

void QuestdbOrderAuditSink::tick()
{
    if (store_ && active_flag_ && *active_flag_) store_->tick();
}

void QuestdbOrderAuditSink::flush()
{
    if (store_ && active_flag_ && *active_flag_) store_->flush();
}

void QuestdbOrderAuditSink::finalize_run(double final_equity,
                                         std::size_t total_orders,
                                         std::size_t total_fills,
                                         std::size_t total_rejections_in,
                                         double max_drawdown,
                                         double sharpe,
                                         double sortino,
                                         double profit_factor,
                                         double win_rate,
                                         double calmar,
                                         std::size_t total_trades,
                                         std::size_t winning_trades)
{
    if (store_ && active_flag_ && *active_flag_)
    {
        store_->end(final_equity, total_orders, total_fills, total_rejections_in,
                    max_drawdown, sharpe, sortino, profit_factor, win_rate, calmar,
                    total_trades, winning_trades);
        store_->flush();
    }
}
#endif
