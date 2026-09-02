#pragma once

// Test-only buffered evidence writer for Plan 08. Business components run
// before any file is opened; the enabled path only appends cold test records
// and serialization happens after the final report has been produced.

#include <charconv>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace observability_evidence {

using row = std::vector<std::string>;

inline std::string number(double value)
{
    if (!std::isfinite(value)) return "UNVERIFIED_NON_FINITE";
    char buffer[128];
    const auto result =
        std::to_chars(buffer, buffer + sizeof(buffer), value, std::chars_format::general,
                      std::numeric_limits<double>::max_digits10);
    if (result.ec != std::errc{}) throw std::runtime_error("failed to format evidence number");
    return std::string(buffer, result.ptr);
}

template <typename Integer> std::string integer(Integer value)
{
    char buffer[64];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
    if (result.ec != std::errc{}) throw std::runtime_error("failed to format evidence integer");
    return std::string(buffer, result.ptr);
}

inline std::string boolean(bool value)
{
    return value ? "true" : "false";
}

inline std::string csv_field(std::string_view value)
{
    bool quote = false;
    for (const char c : value)
        quote = quote || c == ',' || c == '"' || c == '\r' || c == '\n';
    if (!quote) return std::string(value);

    std::string result;
    result.reserve(value.size() + 2);
    result.push_back('"');
    for (const char c : value) {
        if (c == '"') result.push_back('"');
        result.push_back(c);
    }
    result.push_back('"');
    return result;
}

class table
{
public:
    table(std::string filename, row header)
        : filename_(std::move(filename))
        , header_(std::move(header))
    {}

    void append(bool enabled, row values)
    {
        if (!enabled) return;
        if (values.size() != header_.size())
            throw std::runtime_error("evidence row/header width mismatch: " + filename_);
        rows_.push_back(std::move(values));
    }

    void write(const std::filesystem::path& root) const
    {
        std::ofstream out(root / filename_, std::ios::binary | std::ios::trunc);
        if (!out) throw std::runtime_error("cannot create evidence table: " + filename_);
        write_row(out, header_);
        for (const auto& values : rows_)
            write_row(out, values);
        if (!out) throw std::runtime_error("cannot finish evidence table: " + filename_);
    }

    std::size_t size() const noexcept { return rows_.size(); }

private:
    static void write_row(std::ofstream& out, const row& values)
    {
        for (std::size_t i = 0; i < values.size(); ++i) {
            if (i != 0) out.put(',');
            out << csv_field(values[i]);
        }
        out << "\r\n";
    }

    std::string filename_;
    row header_;
    std::vector<row> rows_;
};

class trace_buffer
{
public:
    explicit trace_buffer(bool enabled)
        : enabled_(enabled)
        , bars("bars.csv", {"physical_row", "accepted_index", "bar_index", "timestamp_ms", "symbol",
                            "open", "high", "low", "close", "source_provenance",
                            "index_provenance", "payload_provenance"})
        , indicators("indicators.csv", {"bar_index", "name", "before", "after",
                                        "index_provenance", "value_provenance"})
        , signals("signals.csv", {"signal_id",
                                  "bar_index",
                                  "physical_row",
                                  "timestamp_ms",
                                  "state_before",
                                  "state_after",
                                  "ema",
                                  "rsi_previous",
                                  "rsi_current",
                                  "atr",
                                  "close_above_ema",
                                  "close_below_ema",
                                  "previous_at_or_below_long",
                                  "current_above_long",
                                  "previous_at_or_above_short",
                                  "current_below_short",
                                  "decision",
                                  "identity_provenance",
                                  "source_provenance",
                                  "state_provenance",
                                  "condition_provenance",
                                  "decision_provenance"})
        , orders("orders.csv",
                 {"order_id", "signal_id", "opener_order_id", "side", "type", "intended_price",
                  "quantity", "decision_timestamp_ms", "submit_timestamp_ms",
                  "eligible_timestamp_ms", "identity_provenance", "payload_provenance",
                  "timestamp_provenance"})
        , risk("risk-decisions.csv",
               {"order_id", "check_scope", "equity", "mark", "position_qty", "gross_exposure",
                "action", "rule", "identity_provenance", "input_provenance",
                "decision_provenance", "completeness"})
        , execution("execution.csv",
                    {"order_id", "model", "book_side", "book_price", "book_quantity",
                     "intended_price", "reference_price", "fill_price", "slippage",
                     "modeled_spread_bps", "modeled_impact_bps", "depth_source",
                     "identity_provenance", "depth_provenance", "fill_provenance",
                     "slippage_provenance"})
        , fills("fills.csv",
                {"fill_id", "order_id", "opener_order_id", "side", "quantity", "fee",
                 "timestamp_ms", "remaining_quantity", "intended_price", "reference_price",
                 "fill_price", "modeled_spread_bps", "modeled_impact_bps", "model", "reason",
                 "payload_provenance", "correlation_provenance"})
        , positions("positions.csv",
                    {"fill_id", "cash_before", "cash_after", "quantity_before", "quantity_after",
                     "cost_basis_before", "cost_basis_after", "identity_provenance",
                     "state_provenance"})
        , exits("exits.csv",
                {"opener_order_id", "phase", "intent_stop", "armed_count", "trigger_reason",
                 "trigger_price", "close_order_id", "identity_provenance", "phase_provenance",
                 "intent_provenance", "state_provenance", "trigger_provenance"})
        , reconciliation("trade-reconciliation.csv",
                         {"report_row", "fill_id", "order_id", "opener_order_id", "report_pnl",
                          "report_commission", "link_status", "record_provenance",
                          "link_provenance"})
        , completeness("completeness.csv", {"link", "status", "reason", "provenance"})
    {}

    bool enabled() const noexcept { return enabled_; }

    template <typename Factory> void append(table& target, Factory&& factory)
    {
        if (!enabled_) return;
        target.append(true, std::forward<Factory>(factory)());
    }

    void write(const std::filesystem::path& root) const
    {
        if (!enabled_) return;
        std::filesystem::create_directories(root);
        bars.write(root);
        indicators.write(root);
        signals.write(root);
        orders.write(root);
        risk.write(root);
        execution.write(root);
        fills.write(root);
        positions.write(root);
        exits.write(root);
        reconciliation.write(root);
        completeness.write(root);
    }

    bool enabled_;
    table bars;
    table indicators;
    table signals;
    table orders;
    table risk;
    table execution;
    table fills;
    table positions;
    table exits;
    table reconciliation;
    table completeness;
};

inline void write_text(const std::filesystem::path& path, std::string_view content)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("cannot create evidence artifact: " + path.string());
    out << content;
    if (!out) throw std::runtime_error("cannot finish evidence artifact: " + path.string());
}

}  // namespace observability_evidence
