#include "order_audit_sink.h"

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
                                        const std::string& strategy_name,
                                        const char* source)
{
    if (store_ && active_flag_ && *active_flag_)
    {
        store_->record_fill(f, opener_order_id, strategy_name,
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

void QuestdbOrderAuditSink::record_rejection(uint64_t order_id,
                                             const std::string& symbol,
                                             const char* category,
                                             const char* detail)
{
    ++total_rejections_;
    // Sparse: no corresponding overload on current QuestdbStore yet (skeleton).
    // Full data delegation will be enabled when store gains sparse variant;
    // counter is updated here per design. No store call for now to match existing API.
    (void)order_id; (void)symbol; (void)category; (void)detail;
}

void QuestdbOrderAuditSink::record_cancellation(uint64_t order_id,
                                                const std::string& symbol,
                                                const std::string& strategy_name,
                                                const char* reason)
{
    if (store_ && active_flag_ && *active_flag_)
    {
        store_->record_cancellation(order_id, symbol, strategy_name,
                                    reason ? std::string(reason) : std::string{});
    }
}

void QuestdbOrderAuditSink::record_amendment(uint64_t order_id,
                                             const std::string& symbol,
                                             double old_price, double new_price,
                                             double old_qty, double new_qty,
                                             std::chrono::system_clock::time_point ts)
{
    if (store_ && active_flag_ && *active_flag_)
    {
        store_->record_amendment(order_id, symbol, old_price, new_price, old_qty, new_qty, ts);
    }
}

void QuestdbOrderAuditSink::record_funding(const funding_event& fe, const std::string& run_tag)
{
    if (store_ && active_flag_ && *active_flag_)
    {
        store_->record_funding(fe, run_tag);
    }
}

void QuestdbOrderAuditSink::record_event(const char* event_type,
                                         const std::string& symbol,
                                         const std::string& strategy_name,
                                         uint64_t order_id,
                                         const char* severity,
                                         const char* message,
                                         const char* details_json)
{
    if (store_ && active_flag_ && *active_flag_)
    {
        store_->record_event(event_type ? std::string(event_type) : std::string{},
                             symbol,
                             strategy_name,
                             order_id,
                             severity ? std::string(severity) : std::string{},
                             message ? std::string(message) : std::string{},
                             details_json ? std::string(details_json) : std::string{});
    }
}

std::size_t QuestdbOrderAuditSink::total_rejections() const
{
    return total_rejections_;
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
#endif
