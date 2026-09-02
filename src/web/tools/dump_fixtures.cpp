// Fixture dump tool — builds a representative dashboard_snapshot and
// AnalyticsReport, runs them through the web serializers, and writes the two
// JSON fixtures the frontend renders against offline.
//
// Standalone (no engine link): constructs the plain structs directly. Until
// the embedded server (ENABLE_WEB, step 3) can emit fixtures from a real run,
// this is the source of the engine-shaped contract samples.
//
// Build:
//   g++ -std=c++23 -I src src/web/snapshot_json.cpp src/web/report_json.cpp src/web/tools/dump_fixtures.cpp -o /tmp/dump_fixtures
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
    s.total_pnl       = 134316.42;
    s.realized_pnl    = 47312.88;
    s.unrealized_pnl  = 9893.55;
    s.equity_available = true;
    s.total_pnl_available = true;
    s.realized_pnl_available = true;
    s.unrealized_pnl_available = true;

    s.positions = {
        {.symbol = "BTCUSDT", .qty = 8.42, .avg_entry = 70180.25,
         .mark = 71248.50, .unrealized = (71248.50 - 70180.25) * 8.42,
         .mark_available = true, .unrealized_available = true},
        {.symbol = "ETHUSDT", .qty = 142.0, .avg_entry = 3798.40,
         .mark = 3842.18, .unrealized = (3842.18 - 3798.40) * 142.0,
         .mark_available = true, .unrealized_available = true},
        {.symbol = "SOLUSDT", .qty = -1200.0, .avg_entry = 182.10,
         .mark = 177.94, .unrealized = (177.94 - 182.10) * -1200.0,
         .mark_available = true, .unrealized_available = true},
    };

    s.lots = {
        {.opener_order_id = 4471, .symbol = "BTCUSDT", .strategy_name = "MOM-XBT",
         .side = 'L', .qty_open = 8.42, .entry_price = 70180.25, .age_seconds = 5280},
        {.opener_order_id = 4468, .symbol = "ETHUSDT", .strategy_name = "MR-ETH",
         .side = 'L', .qty_open = 142.0, .entry_price = 3798.40, .age_seconds = 4110},
        {.opener_order_id = 4459, .symbol = "SOLUSDT", .strategy_name = "BASIS-SOL",
         .side = 'S', .qty_open = 1200.0, .entry_price = 182.10, .age_seconds = 9230},
    };

    s.open_orders = {
        {.order_id = 90231, .symbol = "BTCUSDT", .strategy_name = "MOM-XBT",
         .side = 'B', .type = 's', .qty = 0.50, .price = 70900.00,
         .trigger_price = 71000.00, .trigger_price_available = true,
         .age_seconds = 42, .status = "working"},
        {.order_id = 90244, .symbol = "ETHUSDT", .strategy_name = "MR-ETH",
         .side = 'S', .type = 'L', .qty = 30.0, .price = 3905.00,
         .trigger_price = 0.0, .trigger_price_available = false,
         .age_seconds = 18, .status = "working"},
    };

    s.recent_fills = {
        {.ts = at(base_ms + 41 * 60000), .symbol = "BTCUSDT", .side = 'B',
         .qty = 0.18, .price = 71240.5, .fee = 2.31, .source = "exchange"},
        {.ts = at(base_ms + 40 * 60000), .symbol = "ETHUSDT", .side = 'S',
         .qty = 4.20, .price = 3842.0, .fee = 1.45, .source = "simulated"},
        {.ts = at(base_ms + 39 * 60000), .symbol = "SOLUSDT", .side = 'B',
         .qty = 60.0, .price = 177.92, .fee = 0.96, .source = "unexpected"},
    };

    s.brackets = {
        {.opener_order_id = 4471, .strategy_name = "MOM-XBT", .symbol = "BTCUSDT",
         .side = 'L', .qty = 8.42, .entry_price = 70180.25, .stop_loss = 69240.00,
         .take_profit = 72600.00, .mark = 71248.50, .venue_managed = false,
         .venue_list_id = "", .age_seconds = 5280},
        {.opener_order_id = 4468, .strategy_name = "MR-ETH", .symbol = "ETHUSDT",
         .side = 'L', .qty = 142.0, .entry_price = 3798.40, .stop_loss = 3742.00,
         .take_profit = 3930.00, .mark = 3842.18, .venue_managed = true,
         .venue_list_id = "oco-88241", .age_seconds = 4110},
        {.opener_order_id = 4459, .strategy_name = "BASIS-SOL", .symbol = "SOLUSDT",
         .side = 'S', .qty = 1200.0, .entry_price = 182.10, .stop_loss = 187.40,
         .take_profit = 171.00, .mark = 177.94, .venue_managed = false,
         .venue_list_id = "", .age_seconds = 9230},
        {.opener_order_id = 4455, .strategy_name = "MOM-XBT", .symbol = "BTCUSDT",
         .side = 'L', .qty = 4.00, .entry_price = 70910.00, .stop_loss = 70050.00,
         .take_profit = 73100.00, .mark = 71248.50, .venue_managed = false,
         .venue_list_id = "", .age_seconds = 1980},
    };

    // win_rate is a percent (0..100), matching sub_analytics::win_rate() in the engine.
    s.strategies = {
        {.name = "MOM-XBT", .pnl = 28940.55, .trade_count = 184, .win_count = 113,
         .win_rate = 61.2, .profit_factor = 2.31, .total_win = 41200,
         .total_loss = 17840, .open_lots = 2, .armed_brackets = 2},
        {.name = "MR-ETH", .pnl = 12184.20, .trade_count = 241, .win_count = 140,
         .win_rate = 58.3, .profit_factor = 1.74, .total_win = 28600,
         .total_loss = 16420, .open_lots = 1, .armed_brackets = 1},
        {.name = "BASIS-SOL", .pnl = -3420.18, .trade_count = 96, .win_count = 46,
         .win_rate = 47.6, .profit_factor = 0.91, .total_win = 14200,
         .total_loss = 15620, .open_lots = 1, .armed_brackets = 1},
        {.name = "STAT-ARB", .pnl = 9601.31, .trade_count = 312, .win_count = 208,
         .win_rate = 66.8, .profit_factor = 2.02, .total_win = 19300,
         .total_loss = 9560, .open_lots = 0, .armed_brackets = 0},
    };

    s.risk = {.halted = false,
              .daily_loss = 8240.0,
              .daily_loss_limit = 25000.0,
              .daily_loss_available = true,
              .max_drawdown_pct = 4.18,
              .max_drawdown_limit = 8.0,
              .max_drawdown_available = true,
              .exposure = 1.42e6,
              .exposure_limit = 2.0e6,
              .exposure_available = true,
              .open_orders = 11,
              .open_orders_limit = 20};

    s.perf = {.total_orders = 1320,
              .total_fills = 1290,
              .total_trades = 1284,
              .win_rate = 59.8,
              .sharpe = 2.14,
              .sortino = 3.08,
              .profit_factor = 1.92,
              .avg_markout_bps = 1.4,
              .markout_samples = 1290};

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
        s.l2.bids.push_back(
            {.price = mid - (i + 1) * 4.0, .size = bsz, .cum = cb});
        s.l2.asks.push_back(
            {.price = mid + (i + 1) * 4.0, .size = asz, .cum = ca});
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
    s.trend.equity_available = true;
    s.trend.drawdown_now_available = true;

    s.queue = {.available = true,
               .avg_bps = 2450,
               .submitted_with_queue = 28,
               .filled_after_drain = 19,
               .blocked_at_eos = 2};

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
