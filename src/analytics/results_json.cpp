#include "results_json.h"

#include "analytics.h"
#include "utils/json_emit.h"

#include <chrono>

namespace truetest::analytics_json {

using truetest::json_emit::Json;

namespace {

long long ts_ms(std::chrono::system_clock::time_point tp)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(tp.time_since_epoch()).count();
}

void write_curve(Json& j, const std::vector<equity_point>& curve)
{
    // [[ts_ms, equity], ...] — compact pair form the chart consumes directly.
    j.arr();
    for (const auto& p : curve)
    {
        j.arr().inum(ts_ms(p.timestamp)).num(p.equity).endarr();
    }
    j.endarr();
}

void write_breakdown(Json& j, const std::unordered_map<std::string, sub_analytics>& m)
{
    j.obj();
    for (const auto& [k, sa] : m)
    {
        j.key(k.c_str()).obj()
            .kv("total_pnl", sa.total_pnl)
            .kv("trade_count", sa.trade_count)
        .kv("win_count", sa.win_count)
        .kv("win_rate", sa.win_rate())
        .kv("profit_factor", sa.profit_factor())
        .kv("profit_factor_valid", sa.profit_factor_valid())
        .kv("profit_factor_unbounded", sa.profit_factor_unbounded())
        .kv("profit_factor_reason", sa.profit_factor_reason())
        .kv("total_win", sa.total_win)
        .kv("total_loss", sa.total_loss)
        .endobj();
    }
    j.endobj();
}

} // namespace

std::string report_to_json(const AnalyticsReport& r)
{
    std::string out;
    out.reserve(16384);
    Json j(out);

    j.obj();
    j.kv("schema_version", static_cast<long long>(report_schema_version));

    // ---- headline / returns ----
    j.kv("initial_equity", r.initial_equity)
        .kv("final_equity", r.final_equity)
        .kv("gross_realized_pnl", r.gross_realized_pnl)
        .kv("realized_pnl", r.realized_pnl)
        .kv("funding_pnl", r.funding_pnl)
        .kv("unrealized_pnl", r.unrealized_pnl)
        .kv("total_commission", r.total_commission)
        .kv("reconciliation_residual", r.reconciliation_residual)
        .kv("accounting_reconciled", r.accounting_reconciled)
        .kv("accounting_reconciliation_reason",
            r.accounting_reconciliation_reason)
        .kv("valuation_complete", r.valuation_complete)
        .kv("valuation_reason", r.valuation_reason)
        .kv("portfolio_time_series_valid", r.portfolio_time_series_valid)
        .kv("portfolio_time_series_reason", r.portfolio_time_series_reason)
        .kv("ambiguous_portfolio_mark_sequences_rejected",
            r.ambiguous_portfolio_mark_sequences_rejected)
        .kv("periods_per_year", r.periods_per_year)
        .kv("return_observation_basis", r.return_observation_basis)
        .kv("equity_curve_sample_stride", r.equity_curve_sample_stride)
        .kv("benchmark_equity_curve_sample_stride",
            r.benchmark_equity_curve_sample_stride)
        .kv("benchmark_curve_observation_basis",
            r.benchmark_curve_observation_basis)
        .kv("max_drawdown_peak_equity", r.max_drawdown_peak_equity)
        .kv("max_drawdown_trough_equity", r.max_drawdown_trough_equity)
        .kv("max_drawdown_source", "full_resolution_mark_stream")
        .kv("cumulative_return", r.cumulative_return)
        .kv("annualized_return", r.annualized_return)
        .kv("annualized_return_valid", r.annualized_return_valid)
        .kv("annualized_return_reason", r.annualized_return_reason)
        .kv("annualized_return_basis", r.annualized_return_basis)
        .kv("sharpe_ratio", r.sharpe_ratio)
        .kv("sharpe_ratio_valid", r.sharpe_ratio_valid)
        .kv("sharpe_ratio_reason", r.sharpe_ratio_reason)
        .kv("sortino_ratio", r.sortino_ratio)
        .kv("sortino_ratio_valid", r.sortino_ratio_valid)
        .kv("sortino_ratio_reason", r.sortino_ratio_reason)
        .kv("max_drawdown", r.max_drawdown)
        .kv("calmar_ratio", r.calmar_ratio)
        .kv("calmar_ratio_valid", r.calmar_ratio_valid)
        .kv("calmar_ratio_reason", r.calmar_ratio_reason)
        .kv("rolling_sharpe", r.rolling_sharpe)
        .kv("rolling_max_drawdown", r.rolling_max_drawdown)
        .kv("rolling_return_count", r.rolling_return_count)
        .kv("rolling_window", r.rolling_window)
        .kv("rolling_sharpe_reason", r.rolling_sharpe_reason)
        .kv("rolling_max_drawdown_reason", r.rolling_max_drawdown_reason);

    // ---- trade stats ----
    j.kv("win_rate", r.win_rate)
        .kv("total_win", r.total_win)
        .kv("total_loss", r.total_loss)
        .kv("profit_factor", r.profit_factor)
        .kv("profit_factor_valid", r.profit_factor_valid)
        .kv("profit_factor_unbounded", r.profit_factor_unbounded)
        .kv("profit_factor_reason", r.profit_factor_reason)
        .kv("total_trades", r.total_trades)
        .kv("winning_trades", r.winning_trades)
        .kv("total_orders", r.total_orders)
        .kv("total_fills", r.total_fills)
        .kv("duplicate_fill_replays_ignored",
            r.duplicate_fill_replays_ignored)
        .kv("conflicting_fill_replays_rejected",
            r.conflicting_fill_replays_rejected)
        .kv("missing_fill_identities_rejected",
            r.missing_fill_identities_rejected)
        .kv("invalid_fill_payloads_rejected",
            r.invalid_fill_payloads_rejected)
        .kv("unreconciled_funding_events_rejected",
            r.unreconciled_funding_events_rejected)
        .kv("duplicate_funding_replays_ignored",
            r.duplicate_funding_replays_ignored)
        .kv("conflicting_funding_replays_rejected",
            r.conflicting_funding_replays_rejected)
        .kv("late_fill_events_rejected", r.late_fill_events_rejected)
        .kv("late_funding_events_rejected", r.late_funding_events_rejected)
        .kv("late_market_events_rejected", r.late_market_events_rejected)
        .kv("duplicate_market_marks_ignored",
            r.duplicate_market_marks_ignored)
        .kv("conflicting_market_marks_rejected",
            r.conflicting_market_marks_rejected)
        .kv("closing_fill_legs", r.closing_fill_legs)
        .kv("avg_win", r.avg_win)
        .kv("avg_loss", r.avg_loss)
        .kv("largest_winner", r.largest_winner)
        .kv("largest_loser", r.largest_loser)
        .kv("time_in_market_pct", r.time_in_market_pct)
        .kv("time_in_market_valid", r.time_in_market_valid)
        .kv("time_in_market_reason", r.time_in_market_reason)
        .kv("avg_holding_period_ms", r.avg_holding_period_ms);

    j.kv("bankrupt", r.bankrupt)
        .kv("bankrupt_equity", r.bankrupt_equity)
        .kv("exit_intents_registered", r.exit_intents_registered)
        .kv("exit_intents_armed", r.exit_intents_armed)
        .kv("exit_intents_cancelled", r.exit_intents_cancelled)
        .kv("exit_intents_evicted", r.exit_intents_evicted)
        .kv("exit_slippage_disarms", r.exit_slippage_disarms)
        .kv("exit_flatten_requests", r.exit_flatten_requests)
        .kv("soft_post_fill_breaches", r.soft_post_fill_breaches)
        .kv("data_rows_rejected", r.data_rows_rejected)
        .kv("fee_model", r.fee_model.empty() ? "zero" : r.fee_model)
        .kv("execution_claim_scope",
            r.contains_exploratory_execution
                ? "exploratory_synthetic"
                : "not_synthetic_execution_claim");

    // ---- execution: slippage + latency ----
    j.kv("avg_slippage", r.avg_slippage)
        .kv("avg_slippage_signed", r.avg_slippage_signed)
        .kv("avg_adverse_slippage", r.avg_adverse_slippage)
        .kv("avg_favorable_slippage", r.avg_favorable_slippage)
        .kv("adverse_slippage_count", r.adverse_slippage_count)
        .kv("favorable_slippage_count", r.favorable_slippage_count)
        .kv("avg_tick_to_trade_ns", r.avg_tick_to_trade_ns)
        .kv("min_tick_to_trade_ns", static_cast<long long>(r.min_tick_to_trade_ns))
        .kv("max_tick_to_trade_ns", static_cast<long long>(r.max_tick_to_trade_ns))
        .kv("tick_to_trade_samples", r.tick_to_trade_samples);

    // ---- benchmark ----
    j.kv("buy_and_hold_return", r.buy_and_hold_return)
        .kv("strategy_vs_benchmark", r.strategy_vs_benchmark)
        .kv("benchmark_valid", r.benchmark_valid)
        .kv("benchmark_reason", r.benchmark_reason)
        .kv("benchmark_symbol", r.benchmark_symbol)
        .kv("alpha", r.alpha)
        .kv("beta", r.beta)
        .kv("information_ratio", r.information_ratio)
        .kv("tracking_error", r.tracking_error);

    // ---- time series ----
    j.key("equity_curve");           write_curve(j, r.equity_curve);
    j.key("benchmark_equity_curve"); write_curve(j, r.benchmark_equity_curve);

    j.key("trade_returns").arr();
    for (double x : r.trade_returns) j.num(x);
    j.endarr();
    j.kv("trade_rows_kind", "physical_fill_legs")
        .kv("trade_returns_kind", "closing_fill_legs");

    // ---- per-symbol / per-strategy ----
    j.key("per_symbol");   write_breakdown(j, r.per_symbol);
    j.key("per_strategy"); write_breakdown(j, r.per_strategy);

    // ---- full trade blotter ----
    j.key("trades").arr();
    for (const auto& t : r.trades)
    {
        j.obj()
            .kv("order_id", std::to_string(t.order_id))
            .kv("fill_id", std::to_string(t.fill_id))
            .kv("venue_execution_id", t.venue_execution_id)
            .kv("side", t.side == order_side::buy ? "buy" : "sell")
            .kv("quantity", t.quantity)
            .kv("fill_price", t.fill_price)
            .kv("commission", t.commission)
            .kv("commission_currency", t.commission_currency)
            .kv("intended_price", t.intended_price)
            .kv("ts_ms", ts_ms(t.timestamp))
            .kv("reference_price", t.reference_price)
            .kv("reference_timestamp_ms", ts_ms(t.reference_timestamp))
            .kv("execution_model", fill_execution_model_name(t.execution_model))
            .kv("execution_reason", fill_execution_reason_name(t.execution_reason))
            .kv("exploratory", t.execution_is_exploratory)
            .kv("modeled_spread_bps", t.modeled_spread_bps)
            .kv("modeled_impact_bps", t.modeled_impact_bps)
            .kv("fill_probability", t.fill_probability)
            .kv("modeled_latency_ns",
                static_cast<long long>(t.modeled_latency.count()))
            .kv("pnl", t.pnl)
            .kv("symbol", t.symbol)
            .kv("strategy_name", t.strategy_name)
            .endobj();
    }
    j.endarr();

    j.key("open_positions").arr();
    for (const auto& position : r.open_positions)
    {
        j.obj()
            .kv("symbol", position.symbol)
            .kv("strategy_name", position.strategy_name)
            .kv("quantity", position.quantity)
            .kv("avg_entry", position.avg_entry)
            .kv("mark", position.mark)
            .kv("mark_valid", position.mark_valid)
            .kv("mark_source", position.mark_source)
            .kv("unrealized_pnl", position.unrealized_pnl)
            .kv("side", position.side())
            .endobj();
    }
    j.endarr();

    j.endobj();
    return out;
}

} // namespace truetest::analytics_json

std::string AnalyticsReport::to_results_json() const
{
    return truetest::analytics_json::report_to_json(*this);
}
