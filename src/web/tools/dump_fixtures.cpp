// Fixture dump tool — builds a representative dashboard_snapshot and
// AnalyticsReport, runs them through the web serializers, and writes the two
// JSON fixtures the frontend renders against offline.
//
// Standalone (no engine link): constructs the plain structs directly. Until
// the embedded server (ENABLE_WEB, step 3) can emit fixtures from a real run,
// this is the source of the engine-shaped contract samples.
//
// Build:
//   g++ -std=c++23 -I src src/web/snapshot_json.cpp src/web/report_json.cpp \
//       src/web/tools/dump_fixtures.cpp -o /tmp/dump_fixtures
// Run:
//   /tmp/dump_fixtures [out_dir]   (default: src/web/frontend/src/fixtures)

#include "../snapshot_json.h"
#include "../report_json.h"
#include "../../ui/dashboard_snapshot.h"
#include "../../analytics/analytics.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

using truetest::ui::dashboard_snapshot;
using sc = std::chrono::system_clock;

namespace {

sc::time_point at(long long ms)
{
    return sc::time_point(std::chrono::milliseconds(ms));
}

// 2025-01-01T00:00:00Z in ms — fixed base so fixtures are deterministic.
constexpr long long base_ms = 1735689600000LL;

dashboard_snapshot build_snapshot()
{
    dashboard_snapshot s;

    s.cash            = 642880.10;
    s.equity          = 1284316.42;
    s.initial_balance = 1150000.00;
    s.realized_pnl    = 47312.88;
    s.unrealized_pnl  = 9893.55;

    s.positions = {
        {"BTCUSDT",  8.42,  70180.25, 71248.50, (71248.50 - 70180.25) * 8.42},
        {"ETHUSDT",  142.0, 3798.40,  3842.18,  (3842.18 - 3798.40) * 142.0},
        {"SOLUSDT", -1200.0, 182.10,  177.94,   (177.94 - 182.10) * -1200.0},
    };

    s.lots = {
        {4471, "BTCUSDT", "MOM-XBT",   'L', 8.42,  70180.25, 5280},
        {4468, "ETHUSDT", "MR-ETH",    'L', 142.0, 3798.40,  4110},
        {4459, "SOLUSDT", "BASIS-SOL", 'S', 1200.0, 182.10,  9230},
    };

    s.open_orders = {
        {90231, "BTCUSDT", "MOM-XBT", 'B', 'L', 0.50, 70900.00, 42, "working"},
        {90244, "ETHUSDT", "MR-ETH",  'S', 'L', 30.0, 3905.00,  18, "working"},
    };

    s.recent_fills = {
        {at(base_ms + 41 * 60000), "BTCUSDT", 'B', 0.18, 71240.5, 2.31, "exchange"},
        {at(base_ms + 40 * 60000), "ETHUSDT", 'S', 4.20, 3842.0,  1.45, "simulated"},
        {at(base_ms + 39 * 60000), "SOLUSDT", 'B', 60.0, 177.92, 0.96, "exchange"},
    };

    s.brackets = {
        {4471, "MOM-XBT",   "BTCUSDT", 'L', 8.42,  70180.25, 69240.00, 72600.00, 71248.50, false, "",          5280},
        {4468, "MR-ETH",    "ETHUSDT", 'L', 142.0, 3798.40,  3742.00,  3930.00,  3842.18,  true,  "oco-88241", 4110},
        {4459, "BASIS-SOL", "SOLUSDT", 'S', 1200.0, 182.10,  187.40,   171.00,   177.94,   false, "",          9230},
        {4455, "MOM-XBT",   "BTCUSDT", 'L', 4.00,  70910.00, 70050.00, 73100.00, 71248.50, false, "",          1980},
    };

    // win_rate is a percent (0..100), matching sub_analytics::win_rate() in the engine.
    s.strategies = {
        {"MOM-XBT",   28940.55, 184, 113, 61.2, 2.31, 41200, 17840, 2, 2},
        {"MR-ETH",    12184.20, 241, 140, 58.3, 1.74, 28600, 16420, 1, 1},
        {"BASIS-SOL", -3420.18, 96,  46,  47.6, 0.91, 14200, 15620, 1, 1},
        {"STAT-ARB",  9601.31,  312, 208, 66.8, 2.02, 19300, 9560,  0, 0},
    };

    s.risk = {false, 8240.0, 25000.0, 4.18, 8.0, 1.42e6, 2.0e6, 11, 20};

    s.perf = {1320, 1290, 1284, 59.8, 2.14, 3.08, 1.92, 1.4, 1290};

    // L2 ladder around the BTC mid.
    s.l2.symbol = "BTCUSDT";
    s.l2.source = dashboard_snapshot::l2_source::venue;
    const double mid = 71248.5;
    double cb = 0.0, ca = 0.0;
    for (int i = 0; i < 10; ++i)
    {
        double bsz = 6.0 + i * 0.4;
        double asz = 5.6 + i * 0.5;
        cb += bsz; ca += asz;
        s.l2.bids.push_back({mid - (i + 1) * 4.0, bsz, cb});
        s.l2.asks.push_back({mid + (i + 1) * 4.0, asz, ca});
    }
    s.l2.total_bid_levels = s.l2.bids.size();
    s.l2.total_ask_levels = s.l2.asks.size();
    s.l2.best_bid = s.l2.bids.front().price;
    s.l2.best_ask = s.l2.asks.front().price;
    s.l2.mid = mid;
    s.l2.spread_bps = (s.l2.best_ask - s.l2.best_bid) / mid * 1e4;
    s.l2.microprice = mid + 0.6;
    s.l2.cum_bid_size = cb;
    s.l2.cum_ask_size = ca;
    s.l2.imbalance = (cb - ca) / (cb + ca);

    auto& h = s.health;
    h.avg_tick_to_trade_us = 412; h.min_tick_to_trade_us = 198; h.max_tick_to_trade_us = 1840;
    h.tick_to_trade_samples = 1290;
    h.events_total = 318402000; h.fills_total = 1290; h.orders_total = 1320; h.trades_total = 1284;
    h.provider_present = true; h.provider_name = "binance-ws"; h.provider_state = 2;
    h.rate_ev_per_sec = 14820;
    h.questdb.active = true; h.questdb.connected = true; h.questdb.last_flush_age_ms = 12;

    // Trend tails — a short stable-ish climb.
    double eq = 1238900.0, peak = eq;
    for (int i = 0; i < 40; ++i)
    {
        eq += std::sin(i * 0.4) * 1200 + 900;
        peak = std::max(peak, eq);
        s.trend.equity_tail.push_back(eq);
        s.trend.drawdown_tail.push_back((peak - eq) / peak * 100.0);
        s.trend.rate_tail.push_back(14820 + std::sin(i * 0.7) * 700);
    }
    s.trend.equity_now = s.equity;
    s.trend.equity_change_pct = (s.equity - s.initial_balance) / s.initial_balance * 100.0;
    s.trend.drawdown_now_pct = s.trend.drawdown_tail.back();
    s.trend.rate_now = s.trend.rate_tail.back();

    return s;
}

AnalyticsReport build_report()
{
    AnalyticsReport r;
    r.initial_equity = 1000000.0;
    r.final_equity   = 1384200.0;
    r.cumulative_return = 0.3842;
    r.annualized_return = 0.3842;
    r.sharpe_ratio = 2.14; r.sortino_ratio = 3.08; r.max_drawdown = 15.94; r.calmar_ratio = 2.41;
    r.rolling_sharpe = 2.02; r.rolling_max_drawdown = 12.1;

    r.win_rate = 59.8; r.profit_factor = 1.92;
    r.total_win = 1'920.0; r.total_loss = 1'000.0;
    r.profit_factor_valid = true;
    r.profit_factor_reason = "computed_from_gross_win_and_loss";
    r.total_trades = 1284; r.winning_trades = 768;
    r.total_orders = 1320; r.total_fills = 1290;
    r.avg_win = 540.0; r.avg_loss = -360.0;
    r.largest_winner = 8240.0; r.largest_loser = -5410.0;
    r.time_in_market_pct = 62.0; r.avg_holding_period_ms = 5280000.0;

    r.avg_slippage = 1.8; r.avg_slippage_signed = 0.4;
    r.avg_adverse_slippage = 2.6; r.avg_favorable_slippage = 1.1;
    r.adverse_slippage_count = 612; r.favorable_slippage_count = 678;
    r.avg_tick_to_trade_ns = 412000; r.min_tick_to_trade_ns = 198000; r.max_tick_to_trade_ns = 1840000;
    r.tick_to_trade_samples = 1290;

    r.buy_and_hold_return = 0.2632; r.strategy_vs_benchmark = 0.1210;
    r.alpha = 0.142; r.beta = 0.61; r.information_ratio = 0.88; r.tracking_error = 0.09;

    // Equity vs benchmark curves over 2025 (one point/day-ish, 120 pts).
    double s = 1000000.0, b = 1000000.0, peak = s;
    const long long day_ms = 86400000LL;
    for (int i = 0; i < 120; ++i)
    {
        s *= 1.0 + (std::sin(i * 0.21) * 0.006 + 0.0028);
        b *= 1.0 + (std::sin(i * 0.17) * 0.005 + 0.0019);
        peak = std::max(peak, s);
        r.equity_curve.push_back({at(base_ms + i * day_ms), s});
        r.benchmark_equity_curve.push_back({at(base_ms + i * day_ms), b});
    }
    double fs = r.final_equity / s;
    for (auto& p : r.equity_curve) p.equity *= fs;

    for (int i = 0; i < 24; ++i)
        r.trade_returns.push_back(std::sin(i * 1.3) * 900 + 110);

    r.per_symbol = {
        {"BTCUSDT",  {184200.0, 512, 320, 372000.0, 170600.0}},
        {"ETHUSDT",  {121800.0, 468, 276, 268000.0, 148000.0}},
        {"SOLUSDT",  {-22400.0, 304, 156, 96000.0,  118400.0}},
    };
    r.per_strategy = {
        {"MOM-XBT",   {198400.0, 384, 246, 396000.0, 163400.0}},
        {"MR-ETH",    {96200.0,  401, 234, 224000.0, 127800.0}},
        {"STAT-ARB",  {71200.0,  318, 195, 192000.0, 96800.0}},
        {"BASIS-SOL", {-18200.0, 181, 90,  84000.0,  102200.0}},
    };

    const char* syms[] = {"BTCUSDT", "ETHUSDT", "SOLUSDT"};
    const char* strats[] = {"MOM-XBT", "MR-ETH", "BASIS-SOL", "STAT-ARB"};
    for (int i = 0; i < 40; ++i)
    {
        trade_record t;
        t.order_id = 5000 - i;
        t.fill_id = static_cast<std::uint64_t>(i + 1);
        t.side = (i % 2 == 0) ? order_side::buy : order_side::sell;
        t.symbol = syms[i % 3];
        t.strategy_name = strats[i % 4];
        double base = (i % 3 == 0 ? 71000.0 : i % 3 == 1 ? 3840.0 : 178.0);
        t.intended_price = base;
        double slip = std::sin(i * 0.9) * 0.0003;
        t.fill_price = base * (1.0 + slip);
        t.quantity = (i % 3 == 0 ? 0.4 : i % 3 == 1 ? 12.0 : 120.0);
        t.commission = t.fill_price * t.quantity * 0.0002;
        t.pnl = std::sin(i * 1.1) * 900 + 110;
        t.timestamp = at(base_ms + i * 3600000LL);
        r.trades.push_back(t);
    }

    return r;
}

bool write_file(const std::string& path, const std::string& content)
{
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) { std::fprintf(stderr, "cannot write %s\n", path.c_str()); return false; }
    f << content << '\n';
    return static_cast<bool>(f);
}

} // namespace

int main(int argc, char** argv)
{
    std::string dir = (argc > 1) ? argv[1] : "src/web/frontend/src/fixtures";

    const std::string snap = truetest::web::snapshot_to_json(build_snapshot());
    const std::string rep  = truetest::web::report_to_json(build_report());

    bool ok = true;
    ok &= write_file(dir + "/snapshot.json", snap);
    ok &= write_file(dir + "/report.json", rep);

    std::fprintf(stderr, "snapshot.json: %zu bytes\nreport.json:   %zu bytes\n", snap.size(), rep.size());
    return ok ? 0 : 1;
}
