#include "deterministic_lifecycle_sink.h"

#include "reproducibility/canonical_json.h"
#include "reproducibility/run_manifest.h"
#include "reproducibility/sha256.h"

#include <chrono>
#include <stdexcept>

namespace {

template <std::size_t N>
std::string_view text_view(const std::array<char, N>& value) noexcept
{
    std::size_t size = 0;
    while (size < value.size() && value[size] != '\0')
        ++size;
    return {value.data(), size};
}

std::int64_t timestamp_ns(
    std::chrono::system_clock::time_point timestamp) noexcept
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        timestamp.time_since_epoch()).count();
}

} // namespace

DeterministicLifecycleSink::DeterministicLifecycleSink(std::size_t capacity)
{
    if (capacity == 0)
        throw std::invalid_argument(
            "deterministic lifecycle capacity must be positive");
    records_.reserve(capacity);
}

DeterministicLifecycleSink::Record*
DeterministicLifecycleSink::append(std::string_view kind) noexcept
{
    if (records_.size() == records_.capacity())
    {
        overflowed_ = true;
        return nullptr;
    }
    records_.emplace_back();
    Record& record = records_.back();
    record.sequence = static_cast<std::uint64_t>(records_.size() - 1U);
    if (!copy_text(record.kind, kind))
        return nullptr;
    return &record;
}

void DeterministicLifecycleSink::record_order_submitted(
    const order_event& order, const char* initial_status)
{
    const auto populate = [&](Record* record) noexcept {
        if (!record)
            return;
        record->order_id = order.get_order_id();
        record->opener_order_id = order.get_opener_order_id();
        record->timestamp_ns = timestamp_ns(order.get_decision_ts());
        record->quantity = order.get_quantity();
        record->price = order.get_price();
        (void)copy_text(record->symbol, order.get_symbol());
        (void)copy_text(record->strategy, order.get_strategy_name());
        (void)copy_text(record->status_or_source,
                        initial_status ? std::string_view(initial_status)
                                       : std::string_view{});
    };
    populate(append("intent_create"));
    const std::string_view status = initial_status
        ? std::string_view(initial_status) : std::string_view{};
    if (status != "rejected")
        populate(append("submit"));
}

void DeterministicLifecycleSink::record_status_transition(
    std::uint64_t order_id, order_status old_status, order_status new_status,
    const char* reason)
{
    const auto populate = [&](Record* record) noexcept {
        if (!record)
            return;
        record->order_id = order_id;
        record->old_status = old_status;
        record->new_status = new_status;
        (void)copy_text(record->reason,
                        reason ? std::string_view(reason)
                               : std::string_view{});
    };
    if (new_status == order_status::open)
    {
        populate(append("acknowledgement"));
        populate(append("working_open"));
        return;
    }
    const std::string_view kind = new_status == order_status::cancelled
        ? std::string_view("cancelled")
        : new_status == order_status::expired
        ? std::string_view("expire")
        : new_status == order_status::unknown
        ? std::string_view("unknown")
        : std::string_view("status_transition");
    populate(append(kind));
}

void DeterministicLifecycleSink::record_fill(
    const fill_event& fill, std::uint64_t opener_order_id,
    const char* strategy_name, const char* source)
{
    Record* record = append(fill.is_partial() ? "partial_fill" : "fill");
    if (!record)
        return;
    record->order_id = fill.get_order_id();
    record->opener_order_id = opener_order_id;
    record->fill_id = fill.get_fill_id();
    record->timestamp_ns = timestamp_ns(fill.get_timestamp());
    record->quantity = fill.get_filled_quantity();
    record->price = fill.get_fill_price();
    record->commission = fill.get_commission();
    record->remaining_quantity = fill.get_remaining_qty();
    (void)copy_text(record->symbol, fill.get_symbol());
    (void)copy_text(record->strategy,
                    strategy_name ? std::string_view(strategy_name)
                                  : std::string_view{});
    (void)copy_text(record->status_or_source,
                    source ? std::string_view(source) : std::string_view{});
}

void DeterministicLifecycleSink::record_rejection(
    const order_event& order, const char* category, const char* detail)
{
    ++total_rejections_;
    Record* record = append("reject");
    if (!record)
        return;
    record->order_id = order.get_order_id();
    record->timestamp_ns = timestamp_ns(order.get_timestamp());
    record->quantity = order.get_quantity();
    record->price = order.get_price();
    (void)copy_text(record->symbol, order.get_symbol());
    (void)copy_text(record->strategy, order.get_strategy_name());
    (void)copy_text(record->status_or_source,
                    category ? std::string_view(category) : std::string_view{});
    (void)copy_text(record->reason,
                    detail ? std::string_view(detail) : std::string_view{});
}

void DeterministicLifecycleSink::record_cancellation(
    std::uint64_t order_id, const char* symbol, const char* strategy_name,
    const char* reason)
{
    Record* record = append("cancel_request");
    if (!record)
        return;
    record->order_id = order_id;
    (void)copy_text(record->symbol,
                    symbol ? std::string_view(symbol) : std::string_view{});
    (void)copy_text(record->strategy,
                    strategy_name ? std::string_view(strategy_name)
                                  : std::string_view{});
    (void)copy_text(record->reason,
                    reason ? std::string_view(reason) : std::string_view{});
}

void DeterministicLifecycleSink::record_amendment(
    std::uint64_t order_id, const char* symbol,
    double old_price, double new_price,
    double old_quantity, double new_quantity,
    std::chrono::system_clock::time_point timestamp)
{
    Record* record = append("amend");
    if (!record)
        return;
    record->order_id = order_id;
    record->timestamp_ns = timestamp_ns(timestamp);
    record->old_price = old_price;
    record->price = new_price;
    record->old_quantity = old_quantity;
    record->quantity = new_quantity;
    (void)copy_text(record->symbol,
                    symbol ? std::string_view(symbol) : std::string_view{});
}

void DeterministicLifecycleSink::record_funding(
    const funding_event& event, const char* run_tag)
{
    Record* record = append("funding");
    if (!record)
        return;
    record->timestamp_ns = timestamp_ns(event.get_timestamp());
    record->quantity = event.get_qty_change();
    record->commission = event.get_cash_delta();
    (void)copy_text(record->symbol, event.get_symbol());
    (void)copy_text(record->status_or_source, event.get_reason());
    (void)copy_text(record->details,
                    run_tag ? std::string_view(run_tag) : std::string_view{});
}

void DeterministicLifecycleSink::record_event(
    const char* event_type, const char* symbol, const char* strategy_name,
    std::uint64_t order_id, const char* severity, const char* message,
    const char* details_json)
{
    Record* record = append(event_type ? std::string_view(event_type)
                                       : std::string_view("unknown"));
    if (!record)
        return;
    record->order_id = order_id;
    (void)copy_text(record->symbol,
                    symbol ? std::string_view(symbol) : std::string_view{});
    (void)copy_text(record->strategy,
                    strategy_name ? std::string_view(strategy_name)
                                  : std::string_view{});
    (void)copy_text(record->status_or_source,
                    severity ? std::string_view(severity) : std::string_view{});
    (void)copy_text(record->reason,
                    message ? std::string_view(message) : std::string_view{});
    (void)copy_text(record->details,
                    details_json ? std::string_view(details_json)
                                 : std::string_view{});
}

void DeterministicLifecycleSink::record_exit_lifecycle(
    const exit_lifecycle_record& lifecycle)
{
    Record* record = append("exit_lifecycle");
    if (!record)
        return;
    record->signal_id = lifecycle.signal_id;
    record->order_id = lifecycle.order_id;
    record->opener_order_id = lifecycle.opener_order_id;
    record->fill_id = lifecycle.fill_id;
    record->timestamp_ns = timestamp_ns(lifecycle.decision_ts);
    record->submit_timestamp_ns = timestamp_ns(lifecycle.submit_ts);
    record->eligible_timestamp_ns = timestamp_ns(lifecycle.eligible_ts);
    record->fill_timestamp_ns = timestamp_ns(lifecycle.fill_ts);
    record->quantity = lifecycle.requested_qty;
    record->commission = lifecycle.filled_qty;
    record->remaining_quantity = lifecycle.remaining_qty;
    record->exit_reason = lifecycle.reason;
    record->old_status = lifecycle.state_before;
    record->new_status = lifecycle.state_after;
    (void)copy_text(record->symbol,
                    lifecycle.symbol ? std::string_view(lifecycle.symbol)
                                     : std::string_view{});
    (void)copy_text(record->strategy,
                    lifecycle.strategy_name
                        ? std::string_view(lifecycle.strategy_name)
                        : std::string_view{});
    (void)copy_text(record->status_or_source,
                    lifecycle.phase ? std::string_view(lifecycle.phase)
                                    : std::string_view{});
    (void)copy_text(record->reason,
                    lifecycle.risk_outcome
                        ? std::string_view(lifecycle.risk_outcome)
                        : std::string_view{});
}

std::string DeterministicLifecycleSink::canonical_json_lines() const
{
    if (overflowed_)
        throw std::runtime_error(
            "deterministic lifecycle capture overflowed its startup capacity");
    std::string output;
    output.reserve(records_.size() * 256U);
    for (const auto& record : records_)
    {
        if (text_view(record.kind) == "exit_lifecycle")
        {
            output += truetest::reproducibility::serialize_canonical_json(
                truetest::reproducibility::CanonicalJsonValue::object({
                    {"decision_ts_ns", record.timestamp_ns},
                    {"eligible_ts_ns", record.eligible_timestamp_ns},
                    {"exit_reason", static_cast<std::uint64_t>(
                        record.exit_reason)},
                    {"fill_id", record.fill_id},
                    {"fill_ts_ns", record.fill_timestamp_ns},
                    {"filled_qty", record.commission},
                    {"kind", "exit_lifecycle"},
                    {"opener_order_id", record.opener_order_id},
                    {"order_id", record.order_id},
                    {"phase", text_view(record.status_or_source)},
                    {"remaining_qty", record.remaining_quantity},
                    {"requested_qty", record.quantity},
                    {"risk_outcome", text_view(record.reason)},
                    {"sequence", record.sequence},
                    {"signal_id", record.signal_id},
                    {"state_after", to_string(record.new_status)},
                    {"state_before", to_string(record.old_status)},
                    {"strategy", text_view(record.strategy)},
                    {"submit_ts_ns", record.submit_timestamp_ns},
                    {"symbol", text_view(record.symbol)},
                }));
            output.push_back('\n');
            continue;
        }
        output += truetest::reproducibility::serialize_canonical_json(
            truetest::reproducibility::CanonicalJsonValue::object({
                {"commission", record.commission},
                {"details", text_view(record.details)},
                {"fill_id", record.fill_id},
                {"kind", text_view(record.kind)},
                {"new_status", to_string(record.new_status)},
                {"old_price", record.old_price},
                {"old_quantity", record.old_quantity},
                {"old_status", to_string(record.old_status)},
                {"opener_order_id", record.opener_order_id},
                {"order_id", record.order_id},
                {"price", record.price},
                {"quantity", record.quantity},
                {"reason", text_view(record.reason)},
                {"remaining_quantity", record.remaining_quantity},
                {"sequence", record.sequence},
                {"status_or_source", text_view(record.status_or_source)},
                {"strategy", text_view(record.strategy)},
                {"symbol", text_view(record.symbol)},
                {"timestamp_ns", record.timestamp_ns},
            }));
        output.push_back('\n');
    }
    return output;
}

std::string DeterministicLifecycleSink::write_atomic_and_hash(
    const std::filesystem::path& path, bool replace_existing) const
{
    const std::string bytes = canonical_json_lines();
    truetest::reproducibility::write_text_file_atomic(
        path, bytes, replace_existing);
    return truetest::reproducibility::sha256_hex(bytes);
}
