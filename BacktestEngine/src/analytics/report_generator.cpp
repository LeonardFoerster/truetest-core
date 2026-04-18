#include "report_generator.h"
#include "ascii_widgets.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <sstream>

namespace tt {

using ascii::align;

namespace {

std::string metric(const std::string& label, const std::string& value,
                   double bar_v = 0.0, double bar_max = 0.0,
                   std::size_t bar_w = 0)
{
    std::ostringstream oss;
    oss << "  " << ascii::ljust(label, 20) << "  " << ascii::rjust(value, 14);
    if (bar_w > 0 && bar_max > 0.0)
        oss << "  " << ascii::hbar(bar_v, bar_max, bar_w);
    oss << "\n";
    return oss.str();
}

std::string fmt_fixed(double v, int precision = 3)
{
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.*f", precision, v);
    return buf;
}

std::string format_timestamp(std::chrono::system_clock::time_point tp)
{
    std::time_t t = std::chrono::system_clock::to_time_t(tp);
    std::tm gm_buf{};
#if defined(_WIN32)
    if (gmtime_s(&gm_buf, &t) != 0) return "-";
#else
    if (!gmtime_r(&t, &gm_buf)) return "-";
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &gm_buf);
    return buf;
}

std::string format_latency_ns(double ns)
{
    if (ns <= 0.0 || !std::isfinite(ns)) return "-";
    char buf[32];
    if (ns < 1000.0)             std::snprintf(buf, sizeof(buf), "%.0f ns", ns);
    else if (ns < 1'000'000.0)   std::snprintf(buf, sizeof(buf), "%.2f us", ns / 1000.0);
    else if (ns < 1'000'000'000.0) std::snprintf(buf, sizeof(buf), "%.2f ms", ns / 1'000'000.0);
    else                         std::snprintf(buf, sizeof(buf), "%.2f s",  ns / 1'000'000'000.0);
    return buf;
}

std::string format_duration_ms(double ms)
{
    if (ms < 0.0 || !std::isfinite(ms)) return "-";
    long total_s = static_cast<long>(ms / 1000.0);
    long h = total_s / 3600;
    long m = (total_s % 3600) / 60;
    long s = total_s % 60;
    char buf[32];
    if (h > 0)      std::snprintf(buf, sizeof(buf), "%ldh %ldm", h, m);
    else if (m > 0) std::snprintf(buf, sizeof(buf), "%ldm %lds", m, s);
    else            std::snprintf(buf, sizeof(buf), "%lds", s);
    return buf;
}

std::string render_attribution(
    const std::string& title,
    const std::unordered_map<std::string, sub_analytics>& m,
    const report_options& o)
{
    if (m.empty()) return "";
    std::ostringstream oss;
    oss << "\n" << ascii::section_header(title, o.width) << "\n";
    std::vector<std::vector<std::string>> rows;
    rows.reserve(m.size());
    for (const auto& [key, sa] : m)
    {
        char pnl[32], wr[32], pf[32];
        std::snprintf(pnl, sizeof(pnl), "%+.2f", sa.total_pnl);
        std::snprintf(wr,  sizeof(wr),  "%.1f%%", sa.win_rate());
        std::snprintf(pf,  sizeof(pf),  "%.2f", sa.profit_factor());
        rows.push_back({key, std::to_string(sa.trade_count), pnl, wr, pf});
    }
    oss << ascii::table(
        {"name", "trades", "pnl", "win%", "pf"},
        rows,
        {align::left, align::right, align::right, align::right, align::right});
    return oss.str();
}

} // namespace

std::string render_returns_section(const AnalyticsReport& r, const report_options& o)
{
    std::ostringstream oss;
    oss << "\n" << ascii::section_header("Returns", o.width) << "\n";
    oss << metric("initial equity", ascii::fmt_money(r.initial_equity));
    oss << metric("final equity",   ascii::fmt_money(r.final_equity));

    double ret_abs = std::abs(r.cumulative_return);
    double bh_abs  = std::abs(r.buy_and_hold_return);
    double bar_max = std::max({ret_abs, bh_abs, 0.01});

    oss << metric("total return", ascii::fmt_signed_pct(r.cumulative_return),
                  ret_abs, bar_max, o.bar_width);
    oss << metric("buy & hold",   ascii::fmt_signed_pct(r.buy_and_hold_return),
                  bh_abs, bar_max, o.bar_width);
    oss << metric("vs benchmark", ascii::fmt_signed_pct(r.strategy_vs_benchmark));
    return oss.str();
}

std::string render_risk_section(const AnalyticsReport& r, const report_options& o)
{
    std::ostringstream oss;
    oss << "\n" << ascii::section_header("Risk", o.width) << "\n";
    oss << metric("sharpe ratio",   fmt_fixed(r.sharpe_ratio));
    oss << metric("sortino ratio",  fmt_fixed(r.sortino_ratio));
    oss << metric("max drawdown",   ascii::fmt_pct(r.max_drawdown / 100.0),
                  r.max_drawdown, std::max(r.max_drawdown, 1.0), o.bar_width);
    oss << metric("calmar ratio",   fmt_fixed(r.calmar_ratio));
    oss << metric("rolling sharpe", fmt_fixed(r.rolling_sharpe));
    oss << metric("rolling max dd", ascii::fmt_pct(r.rolling_max_drawdown / 100.0));
    return oss.str();
}

std::string render_trades_section(const AnalyticsReport& r, const report_options& o)
{
    std::ostringstream oss;
    oss << "\n" << ascii::section_header("Trades", o.width) << "\n";
    oss << metric("total trades", std::to_string(r.total_trades));
    oss << metric("win rate", ascii::fmt_pct(r.win_rate / 100.0, 1),
                  r.win_rate, 100.0, o.bar_width);
    oss << metric("profit factor",  fmt_fixed(r.profit_factor));
    oss << metric("avg win",        ascii::fmt_money(r.avg_win));
    oss << metric("avg loss",       ascii::fmt_money(r.avg_loss));
    oss << metric("largest win",    ascii::fmt_money(r.largest_winner));
    oss << metric("largest loss",   ascii::fmt_money(r.largest_loser));
    return oss.str();
}

std::string render_execution_section(const AnalyticsReport& r, const report_options& o)
{
    std::ostringstream oss;
    oss << "\n" << ascii::section_header("Execution Quality", o.width) << "\n";
    oss << metric("avg slippage",        fmt_fixed(r.avg_slippage, 4));
    oss << metric("avg slippage (signed)", fmt_fixed(r.avg_slippage_signed, 4));
    if (r.adverse_slippage_count > 0)
        oss << metric("avg adverse",     fmt_fixed(r.avg_adverse_slippage, 4)
                                       + "  n=" + std::to_string(r.adverse_slippage_count));
    if (r.favorable_slippage_count > 0)
        oss << metric("avg favorable",   fmt_fixed(r.avg_favorable_slippage, 4)
                                       + "  n=" + std::to_string(r.favorable_slippage_count));
    oss << metric("total orders",  std::to_string(r.total_orders));
    oss << metric("total fills",   std::to_string(r.total_fills));
    if (r.tick_to_trade_samples > 0)
    {
        oss << metric("avg tick→trade", format_latency_ns(r.avg_tick_to_trade_ns));
        oss << metric("min tick→trade", format_latency_ns(static_cast<double>(r.min_tick_to_trade_ns)));
        oss << metric("max tick→trade", format_latency_ns(static_cast<double>(r.max_tick_to_trade_ns)));
    }
    return oss.str();
}

std::string render_exposure_section(const AnalyticsReport& r, const report_options& o)
{
    std::ostringstream oss;
    oss << "\n" << ascii::section_header("Exposure", o.width) << "\n";
    oss << metric("time in market", ascii::fmt_pct(r.time_in_market_pct / 100.0, 1),
                  r.time_in_market_pct, 100.0, o.bar_width);
    oss << metric("avg holding", format_duration_ms(r.avg_holding_period_ms));
    return oss.str();
}

std::string render_distribution_section(const AnalyticsReport& r, const report_options& o)
{
    if (r.trade_returns.empty()) return "";
    std::ostringstream oss;
    oss << "\n" << ascii::section_header("Per-Trade PnL Distribution", o.width) << "\n";
    auto bins = ascii::equal_width_bins(r.trade_returns, o.distribution_bins);
    oss << ascii::horizontal_histogram(bins, o.bar_width + 6);
    return oss.str();
}

std::string render_equity_sparkline_section(const AnalyticsReport& r, const report_options& o)
{
    if (r.equity_curve.size() < 2) return "";
    std::ostringstream oss;
    oss << "\n" << ascii::section_header("Equity Curve", o.width) << "\n";

    std::vector<double> values;
    values.reserve(r.equity_curve.size());
    for (const auto& p : r.equity_curve) values.push_back(p.equity);

    oss << "  " << ascii::sparkline(values, o.sparkline_width) << "\n";

    double lo = *std::min_element(values.begin(), values.end());
    double hi = *std::max_element(values.begin(), values.end());
    char buf[160];
    std::snprintf(buf, sizeof(buf), "  min %s   max %s   points %zu\n",
                  ascii::fmt_money(lo).c_str(),
                  ascii::fmt_money(hi).c_str(),
                  values.size());
    oss << buf;
    return oss.str();
}

std::string render_benchmark_section(const AnalyticsReport& r, const report_options& o)
{
    std::ostringstream oss;
    oss << "\n" << ascii::section_header("Benchmark", o.width) << "\n";
    oss << metric("alpha",             fmt_fixed(r.alpha, 4));
    oss << metric("beta",              fmt_fixed(r.beta, 4));
    oss << metric("information ratio", fmt_fixed(r.information_ratio, 4));
    oss << metric("tracking error",    fmt_fixed(r.tracking_error, 4));
    return oss.str();
}

std::string render_per_symbol_section(const AnalyticsReport& r, const report_options& o)
{
    return render_attribution("Per-Symbol Attribution", r.per_symbol, o);
}

std::string render_per_strategy_section(const AnalyticsReport& r, const report_options& o)
{
    return render_attribution("Per-Strategy Attribution", r.per_strategy, o);
}

std::string render_worst_trades_section(const AnalyticsReport& r, const report_options& o)
{
    if (r.trades.empty() || o.worst_trades_count == 0) return "";

    std::vector<trade_record> closed;
    closed.reserve(r.trades.size());
    for (const auto& t : r.trades)
        if (t.pnl != 0.0) closed.push_back(t);
    if (closed.empty()) return "";

    std::sort(closed.begin(), closed.end(),
              [](const trade_record& a, const trade_record& b) { return a.pnl < b.pnl; });

    std::ostringstream oss;
    oss << "\n" << ascii::section_header("Worst Trades", o.width) << "\n";

    std::vector<std::vector<std::string>> rows;
    std::size_t n = std::min(o.worst_trades_count, closed.size());
    rows.reserve(n);
    for (std::size_t i = 0; i < n; ++i)
    {
        const auto& t = closed[i];
        char price[32], pnl[32];
        std::snprintf(price, sizeof(price), "%.4f", t.fill_price);
        std::snprintf(pnl,   sizeof(pnl),   "%+.2f", t.pnl);
        rows.push_back({
            format_timestamp(t.timestamp),
            t.symbol.empty() ? "-" : t.symbol,
            t.side == order_side::buy ? "BUY" : "SELL",
            price,
            pnl,
        });
    }
    oss << ascii::table(
        {"time", "symbol", "side", "price", "pnl"},
        rows,
        {align::left, align::left, align::left, align::right, align::right});
    return oss.str();
}

std::string render_report(const AnalyticsReport& r, const report_options& o)
{
    std::ostringstream oss;

    oss << "\n" << ascii::rule(o.width, "\xe2\x95\x90") << "\n";
    oss << "  " << o.title << "\n";
    oss << ascii::rule(o.width, "\xe2\x95\x90") << "\n";

    if (o.include_returns)          oss << render_returns_section(r, o);
    if (o.include_risk)             oss << render_risk_section(r, o);
    if (o.include_equity_sparkline) oss << render_equity_sparkline_section(r, o);
    if (o.include_trades)           oss << render_trades_section(r, o);
    if (o.include_distribution)     oss << render_distribution_section(r, o);
    if (o.include_execution)        oss << render_execution_section(r, o);
    if (o.include_exposure)         oss << render_exposure_section(r, o);
    if (o.include_benchmark)        oss << render_benchmark_section(r, o);
    if (o.include_per_symbol)       oss << render_per_symbol_section(r, o);
    if (o.include_per_strategy)     oss << render_per_strategy_section(r, o);
    if (o.include_worst_trades)     oss << render_worst_trades_section(r, o);

    oss << "\n";
    return oss.str();
}

} // namespace tt
