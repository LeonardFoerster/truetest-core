#pragma once

// Canonical, trace-independent serialization of the complete Analytics trade
// report. Both trace modes produce this after execution so the capture runner
// can detect semantic changes that aggregate metrics alone would miss.

#include "analytics/analytics.h"
#include "observability_evidence_writer.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace observability_evidence {

inline void append_semantic_row(std::string& output, const row& fields)
{
    for (std::size_t i = 0; i < fields.size(); ++i) {
        if (i != 0) output.push_back(',');
        output += csv_field(fields[i]);
    }
    output += "\r\n";
}

inline std::string canonical_report(const AnalyticsReport& report, double portfolio_cash,
                                    std::uint64_t entry_order_id, std::uint64_t exit_order_id,
                                    std::size_t physical_entry_row)
{
    std::string output;
    const auto scalar = [&](std::string name, std::string value) {
        append_semantic_row(output, {"summary", std::move(name), std::move(value)});
    };
    const auto time_ms = [](std::chrono::system_clock::time_point timestamp) {
        return integer(std::chrono::duration_cast<std::chrono::milliseconds>(
                           timestamp.time_since_epoch())
                           .count());
    };

    // Serialize every AnalyticsReport field, plus the selected final harness
    // state used by the reconciliation assertions. Map keys are sorted below
    // so this representation is stable across fresh processes.
    scalar("initial_equity", number(report.initial_equity));
    scalar("final_equity", number(report.final_equity));
    scalar("portfolio_cash", number(portfolio_cash));
    scalar("gross_realized_pnl", number(report.gross_realized_pnl));
    scalar("realized_pnl", number(report.realized_pnl));
    scalar("funding_pnl", number(report.funding_pnl));
    scalar("unrealized_pnl", number(report.unrealized_pnl));
    scalar("total_commission", number(report.total_commission));
    scalar("reconciliation_residual", number(report.reconciliation_residual));
    scalar("periods_per_year", integer(report.periods_per_year));
    scalar("return_observation_basis", report.return_observation_basis);
    scalar("equity_curve_sample_stride", integer(report.equity_curve_sample_stride));
    scalar("max_drawdown_peak_equity", number(report.max_drawdown_peak_equity));
    scalar("max_drawdown_trough_equity", number(report.max_drawdown_trough_equity));
    scalar("cumulative_return", number(report.cumulative_return));
    scalar("annualized_return", number(report.annualized_return));
    scalar("sharpe_ratio", number(report.sharpe_ratio));
    scalar("sortino_ratio", number(report.sortino_ratio));
    scalar("max_drawdown", number(report.max_drawdown));
    scalar("calmar_ratio", number(report.calmar_ratio));
    scalar("avg_slippage", number(report.avg_slippage));
    scalar("avg_slippage_signed", number(report.avg_slippage_signed));
    scalar("avg_adverse_slippage", number(report.avg_adverse_slippage));
    scalar("avg_favorable_slippage", number(report.avg_favorable_slippage));
    scalar("adverse_slippage_count", integer(report.adverse_slippage_count));
    scalar("favorable_slippage_count", integer(report.favorable_slippage_count));
    scalar("total_orders", integer(report.total_orders));
    scalar("total_fills", integer(report.total_fills));
    scalar("avg_tick_to_trade_ns", number(report.avg_tick_to_trade_ns));
    scalar("min_tick_to_trade_ns", integer(report.min_tick_to_trade_ns));
    scalar("max_tick_to_trade_ns", integer(report.max_tick_to_trade_ns));
    scalar("tick_to_trade_samples", integer(report.tick_to_trade_samples));
    scalar("time_in_market_pct", number(report.time_in_market_pct));
    scalar("avg_holding_period_ms", number(report.avg_holding_period_ms));
    scalar("total_trades", integer(report.total_trades));
    scalar("closing_fill_legs", integer(report.closing_fill_legs));
    scalar("winning_trades", integer(report.winning_trades));
    scalar("win_rate", number(report.win_rate));
    scalar("avg_win", number(report.avg_win));
    scalar("avg_loss", number(report.avg_loss));
    scalar("profit_factor", number(report.profit_factor));
    scalar("largest_winner", number(report.largest_winner));
    scalar("largest_loser", number(report.largest_loser));
    scalar("rolling_sharpe", number(report.rolling_sharpe));
    scalar("rolling_max_drawdown", number(report.rolling_max_drawdown));
    scalar("rolling_return_count", integer(report.rolling_return_count));
    scalar("rolling_window", integer(report.rolling_window));
    scalar("rolling_sharpe_reason", report.rolling_sharpe_reason);
    scalar("rolling_max_drawdown_reason", report.rolling_max_drawdown_reason);
    scalar("buy_and_hold_return", number(report.buy_and_hold_return));
    scalar("strategy_vs_benchmark", number(report.strategy_vs_benchmark));
    scalar("alpha", number(report.alpha));
    scalar("beta", number(report.beta));
    scalar("information_ratio", number(report.information_ratio));
    scalar("tracking_error", number(report.tracking_error));
    scalar("contains_exploratory_execution", boolean(report.contains_exploratory_execution));
    scalar("soft_post_fill_breaches", integer(report.soft_post_fill_breaches));
    scalar("data_rows_rejected", integer(report.data_rows_rejected));
    scalar("bankrupt", boolean(report.bankrupt));
    scalar("bankrupt_equity", number(report.bankrupt_equity));
    scalar("exit_intents_registered", integer(report.exit_intents_registered));
    scalar("exit_intents_armed", integer(report.exit_intents_armed));
    scalar("exit_intents_cancelled", integer(report.exit_intents_cancelled));
    scalar("exit_intents_evicted", integer(report.exit_intents_evicted));
    scalar("exit_slippage_disarms", integer(report.exit_slippage_disarms));
    scalar("exit_flatten_requests", integer(report.exit_flatten_requests));
    scalar("fee_model", report.fee_model);
    scalar("entry_order_id", integer(entry_order_id));
    scalar("exit_order_id", integer(exit_order_id));
    scalar("physical_entry_row", integer(physical_entry_row));

    for (std::size_t i = 0; i < report.equity_curve.size(); ++i) {
        const auto& point = report.equity_curve[i];
        append_semantic_row(output,
                            {"equity_curve", integer(i), time_ms(point.timestamp),
                             number(point.equity)});
    }
    for (std::size_t i = 0; i < report.trade_returns.size(); ++i)
        append_semantic_row(output,
                            {"trade_return", integer(i), number(report.trade_returns[i])});
    for (std::size_t i = 0; i < report.benchmark_equity_curve.size(); ++i) {
        const auto& point = report.benchmark_equity_curve[i];
        append_semantic_row(output,
                            {"benchmark_equity_curve", integer(i), time_ms(point.timestamp),
                             number(point.equity)});
    }

    const auto append_breakdown = [&](std::string_view kind, const auto& breakdown) {
        std::vector<std::pair<std::string, const sub_analytics*>> sorted;
        sorted.reserve(breakdown.size());
        for (const auto& [key, value] : breakdown) sorted.emplace_back(key, &value);
        std::ranges::sort(sorted, {}, &std::pair<std::string, const sub_analytics*>::first);
        for (const auto& [key, value] : sorted) {
            append_semantic_row(output,
                                {std::string(kind), key, number(value->total_pnl),
                                 integer(value->trade_count), integer(value->win_count),
                                 number(value->total_win), number(value->total_loss),
                                 number(value->win_rate()), number(value->profit_factor())});
        }
    };
    append_breakdown("per_symbol", report.per_symbol);
    append_breakdown("per_strategy", report.per_strategy);

    append_semantic_row(output, {"trade",
                                 "index",
                                 "fill_id",
                                 "order_id",
                                 "side",
                                 "quantity",
                                 "fill_price",
                                 "commission",
                                 "intended_price",
                                 "timestamp_ms",
                                 "pnl",
                                 "symbol",
                                 "strategy",
                                 "reference_price",
                                 "reference_timestamp_ms",
                                 "modeled_spread_bps",
                                 "modeled_impact_bps",
                                 "fill_probability",
                                 "modeled_latency_ns",
                                 "execution_model",
                                 "execution_reason",
                                 "execution_is_exploratory"});
    for (std::size_t i = 0; i < report.trades.size(); ++i) {
        const auto& trade = report.trades[i];
        append_semantic_row(output,
                            {"trade",
                             integer(i),
                             integer(trade.fill_id),
                             integer(trade.order_id),
                             trade.side == order_side::buy ? "buy" : "sell",
                             number(trade.quantity),
                             number(trade.fill_price),
                             number(trade.commission),
                             number(trade.intended_price),
                             time_ms(trade.timestamp),
                             number(trade.pnl),
                             trade.symbol,
                             trade.strategy_name,
                             number(trade.reference_price),
                             time_ms(trade.reference_timestamp),
                             number(trade.modeled_spread_bps),
                             number(trade.modeled_impact_bps),
                             number(trade.fill_probability),
                             integer(trade.modeled_latency.count()),
                             std::string(fill_execution_model_name(trade.execution_model)),
                             std::string(fill_execution_reason_name(trade.execution_reason)),
                             boolean(trade.execution_is_exploratory)});
    }
    for (std::size_t i = 0; i < report.open_positions.size(); ++i) {
        const auto& position = report.open_positions[i];
        append_semantic_row(output,
                            {"open_position", integer(i), position.symbol,
                             position.strategy_name, number(position.quantity),
                             number(position.avg_entry), number(position.mark),
                             number(position.unrealized_pnl), position.side()});
    }
    return output;
}

}  // namespace observability_evidence
