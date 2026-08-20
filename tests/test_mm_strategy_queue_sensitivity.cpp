// R1 queue-sensitivity and markout harness.
//
// Drives the inventory-aware market-making strategy over a deterministic
// synthetic L2 path under each of the repository's three cancel-queue
// assumptions (Front / Uniform / Back, src/execution/queue_model.h) and
// reports PnL, fill ratio, time to fill, inventory distribution, spread
// capture, fees, markouts and peak inventory utilisation for each.
//
// The queue and fill models are NOT modified here: FrontCancelModel,
// UniformCancelModel and BackCancelModel are used exactly as the execution
// layer defines them.
//
// The printed table is transcribed into
// check-ups/2026-08-20-r1-queue-sensitivity.md. The assertions in this
// file are invariant checks, not profitability checks: a strategy that only
// makes money under Front is not evidence of an edge.

#include <gtest/gtest.h>

#include "helpers/mm_test_harness.h"

#include "execution/queue_model.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <random>
#include <string>
#include <vector>

using namespace truetest::mm;
using namespace truetest::mm::test;

namespace
{

constexpr int sim_steps = 20'000;          // 1 ms per step -> 20 s of tape
constexpr std::int64_t step_ns = 1'000'000; // 1 ms
constexpr std::uint64_t sim_seed = 0xC0FFEE'2026ULL;
constexpr double maker_fee_bps = 1.0;

// Markout horizons in steps (1 step == 1 ms).
constexpr int markout_steps[] = {10, 100, 1000, 5000};
constexpr const char* markout_labels[] = {"10ms", "100ms", "1s", "5s"};

// ── deterministic synthetic tape ────────────────────────────────────────────
// Generated once and reused by every queue model, so the three runs differ
// only in the cancel assumption.
struct tape_step
{
    std::int64_t bid_ticks = 0;
    std::int64_t spread_ticks = 5;
    double bid_level_size = 0.0;  // base units resting at the best bid
    double ask_level_size = 0.0;
    double sell_aggressor = 0.0;  // base units hitting the bid this step
    double buy_aggressor = 0.0;   // base units lifting the ask this step
    double bid_cancels = 0.0;
    double ask_cancels = 0.0;
    double volatility_bps = 0.0;
    double toxicity_bps = 0.0;
};

std::vector<tape_step> build_tape()
{
    std::vector<tape_step> tape;
    tape.reserve(sim_steps);

    std::mt19937_64 rng(sim_seed);
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    std::normal_distribution<double> walk(0.0, 3.0);

    std::int64_t bid_ticks = 600'000; // 60000.00 with a 0.10 tick
    double drift = 0.0;

    for (int i = 0; i < sim_steps; ++i)
    {
        // Slowly varying drift so the path has trends as well as noise: a
        // pure random walk would understate adverse selection.
        if (i % 500 == 0)
            drift = (unit(rng) - 0.5) * 0.8;

        bid_ticks += static_cast<std::int64_t>(std::llround(walk(rng) + drift));
        bid_ticks = std::max<std::int64_t>(500'000, bid_ticks);

        tape_step s;
        s.bid_ticks = bid_ticks;
        // 110-200 ticks on a 60000 price is a 1.8-3.3 bps spread. A maker
        // paying 1 bp cannot compete inside a tighter book than that, so a
        // narrower synthetic tape would only ever measure "no fills".
        s.spread_ticks = 110 + static_cast<std::int64_t>(unit(rng) * 90.0);
        s.bid_level_size = 0.5 + unit(rng) * 6.0;
        s.ask_level_size = 0.5 + unit(rng) * 6.0;
        s.sell_aggressor = unit(rng) < 0.30 ? unit(rng) * 1.2 : 0.0;
        s.buy_aggressor = unit(rng) < 0.30 ? unit(rng) * 1.2 : 0.0;
        s.bid_cancels = unit(rng) * 0.9;
        s.ask_cancels = unit(rng) * 0.9;
        s.volatility_bps = 0.4 + std::fabs(walk(rng)) * 0.4;
        s.toxicity_bps = 0.1 + unit(rng) * 0.6;
        tape.push_back(s);
    }
    return tape;
}

// ── simulation state ────────────────────────────────────────────────────────
struct resting_quote
{
    order_side side = order_side::buy;
    Price price{};
    qty_atoms remaining = 0;
    double queue_ahead = 0.0;
    int placed_step = 0;
};

struct fill_record
{
    order_side side = order_side::buy;
    Price price{};
    qty_atoms quantity = 0;
    int step = 0;
    int wait_steps = 0;
};

struct run_metrics
{
    std::string queue_model;
    double gross_pnl = 0.0;
    double fees = 0.0;
    double net_pnl = 0.0;
    std::size_t quotes_placed = 0;
    std::size_t quotes_filled = 0;
    double fill_ratio = 0.0;
    double mean_time_to_fill_ms = 0.0;
    double median_abs_utilisation = 0.0;
    double max_abs_utilisation = 0.0;
    double spread_capture_bps = 0.0;
    double markout_bps[4] = {0.0, 0.0, 0.0, 0.0};
    std::size_t markout_samples[4] = {0, 0, 0, 0};
    std::size_t hard_limit_breaches = 0;
    std::size_t invalid_quotes = 0;
};

// Sign convention, defined exactly once and tested in
// MarkoutSignConventionIsMirroredForBuysAndSells below:
//   markout_bps = side_sign * (mid_at_horizon - fill_price) / fill_price * 1e4
//   side_sign = +1 for a buy fill, -1 for a sell fill
// Positive means the fill was on the right side of where the mid went.
double markout_bps(order_side side, Price fill_price, double mid_at_horizon)
{
    const double px = fill_price.to_double();
    if (px <= 0.0)
        return 0.0;
    const double sign = (side == order_side::buy) ? 1.0 : -1.0;
    return sign * (mid_at_horizon - px) / px * 10000.0;
}

double mid_at(const std::vector<tape_step>& tape, int step, std::int64_t tick_raw)
{
    const auto& s = tape[static_cast<std::size_t>(step)];
    const double bid = static_cast<double>(s.bid_ticks * tick_raw) / Price::SCALE;
    const double ask = static_cast<double>((s.bid_ticks + s.spread_ticks) * tick_raw)
        / Price::SCALE;
    return 0.5 * (bid + ask);
}

run_metrics run_simulation(const std::vector<tape_step>& tape, IQueueModel& queue,
                           const std::string& label, const mm_config& cfg)
{
    run_metrics m;
    m.queue_model = label;

    auto strat = make_strategy(cfg);
    auto ctx = default_context();
    const auto tick_raw = ctx.instrument.tick_raw;
    const auto lot = ctx.instrument.lot_atoms;

    qty_atoms position = 0;
    double cash = 0.0;
    std::vector<resting_quote> resting;
    resting_quote_state resting_state{};
    std::vector<fill_record> fills;
    std::vector<double> utilisations;
    resting.reserve(2 * max_quote_levels);
    utilisations.reserve(static_cast<std::size_t>(sim_steps));

    const double hard = static_cast<double>(cfg.inventory.hard_limit_base);

    for (int step = 0; step < sim_steps; ++step)
    {
        const auto& s = tape[static_cast<std::size_t>(step)];
        const std::int64_t bid_raw = s.bid_ticks * tick_raw;
        const std::int64_t ask_raw = (s.bid_ticks + s.spread_ticks) * tick_raw;

        // ── advance resting quotes against this step's flow ────────────────
        for (auto& q : resting)
        {
            if (q.remaining <= 0)
                continue;

            const bool touched = (q.side == order_side::buy)
                ? (bid_raw <= q.price.raw())
                : (ask_raw >= q.price.raw());
            if (!touched)
                continue;

            const double level_total = (q.side == order_side::buy) ? s.bid_level_size
                                                                   : s.ask_level_size;
            const double cancels = (q.side == order_side::buy) ? s.bid_cancels
                                                               : s.ask_cancels;
            q.queue_ahead = queue.update_on_cancels(q.queue_ahead, level_total, cancels);

            double aggressor = (q.side == order_side::buy) ? s.sell_aggressor
                                                           : s.buy_aggressor;
            if (aggressor <= 0.0)
                continue;

            const double consumed = std::min(q.queue_ahead, aggressor);
            q.queue_ahead -= consumed;
            aggressor -= consumed;
            if (aggressor <= 0.0)
                continue;

            const auto fillable = lot_floor(
                static_cast<qty_atoms>(aggressor * static_cast<double>(atoms_per_unit)), lot);
            const qty_atoms filled = std::min(q.remaining, fillable);
            if (filled <= 0)
                continue;

            q.remaining -= filled;
            const double notional =
                q.price.to_double() * static_cast<double>(filled) / static_cast<double>(atoms_per_unit);
            if (q.side == order_side::buy)
            {
                position += filled;
                cash -= notional;
            }
            else
            {
                position -= filled;
                cash += notional;
            }
            m.fees += notional * maker_fee_bps / 10000.0;
            fills.push_back({q.side, q.price, filled, step, step - q.placed_step});
        }

        // ── strategy decision on the fresh book ────────────────────────────
        market_snapshot market;
        market.event_time_ns = base_event_time_ns + step * step_ns;
        market.receive_time_ns = market.event_time_ns + 100'000;
        market.best_bid = Price(bid_raw);
        market.best_ask = Price(ask_raw);
        market.best_bid_qty =
            static_cast<qty_atoms>(s.bid_level_size * static_cast<double>(atoms_per_unit));
        market.best_ask_qty =
            static_cast<qty_atoms>(s.ask_level_size * static_cast<double>(atoms_per_unit));
        market.short_horizon_volatility_bps = s.volatility_bps;
        market.toxicity_risk_bps = s.toxicity_bps;
        market.latency_risk_bps = 0.2;
        market.sequence_valid = true;
        market.snapshot_id = static_cast<std::uint64_t>(step + 1);
        ctx.decision_time_ns = market.receive_time_ns + 50'000;

        // The execution layer owns the resting quotes, so it is what tells the
        // strategy about them. Without this the churn guard can never engage
        // and every quote would live exactly one step.
        ctx.resting = resting_state;

        qty_atoms pending_buy = 0;
        qty_atoms pending_sell = 0;
        for (const auto& q : resting)
        {
            if (q.remaining <= 0)
                continue;
            (q.side == order_side::buy ? pending_buy : pending_sell) += q.remaining;
        }

        inventory_snapshot inv;
        inv.signed_base_position = position;
        inv.hard_limit = 0;
        inv.worst_case_position_if_all_buys_fill = position + pending_buy;
        inv.worst_case_position_if_all_sells_fill = position - pending_sell;
        inv.authoritative = true;

        const double utilisation =
            std::fabs(static_cast<double>(position)) / hard;
        utilisations.push_back(utilisation);
        m.max_abs_utilisation = std::max(m.max_abs_utilisation, utilisation);
        if (std::llabs(position) > cfg.inventory.hard_limit_base)
            ++m.hard_limit_breaches;

        const auto d = strat.evaluate(market, inv, ctx).decision;

        // ── cancel / replace ───────────────────────────────────────────────
        if (d.state == mm_state::paused || d.requote)
        {
            resting.clear();
            resting_state = resting_quote_state{};
        }

        if (d.requote)
        {
            for (std::size_t i = 0; i < d.intents.size(); ++i)
            {
                const auto& q = d.intents[i];
                if (q.quantity <= 0 || !is_tick_valid(q.price, tick_raw)
                    || !is_lot_valid(q.quantity, lot))
                {
                    ++m.invalid_quotes;
                    continue;
                }
                // Queue ahead at submit: the size already resting at our price,
                // or zero when we improve on the touch.
                double ahead = 0.0;
                if (q.side == order_side::buy && q.price.raw() == bid_raw)
                    ahead = s.bid_level_size;
                else if (q.side == order_side::sell && q.price.raw() == ask_raw)
                    ahead = s.ask_level_size;
                resting.push_back({q.side, q.price, q.quantity, ahead, step});
                ++m.quotes_placed;

                if (q.level == 0)
                {
                    resting_state.has_quotes = true;
                    resting_state.placed_time_ns = ctx.decision_time_ns;
                    if (q.side == order_side::buy)
                        resting_state.bid_price = q.price;
                    else
                        resting_state.ask_price = q.price;
                }
            }
        }
    }

    // ── aggregate ─────────────────────────────────────────────────────────
    const double final_mid = mid_at(tape, sim_steps - 1, tick_raw);
    m.gross_pnl = cash + final_mid * static_cast<double>(position)
        / static_cast<double>(atoms_per_unit);
    m.net_pnl = m.gross_pnl - m.fees;
    m.quotes_filled = fills.size();
    m.fill_ratio = m.quotes_placed == 0
        ? 0.0
        : static_cast<double>(m.quotes_filled) / static_cast<double>(m.quotes_placed);

    if (!fills.empty())
    {
        double wait_sum = 0.0;
        double capture_sum = 0.0;
        for (const auto& f : fills)
        {
            wait_sum += static_cast<double>(f.wait_steps);
            capture_sum += markout_bps(f.side, f.price, mid_at(tape, f.step, tick_raw));
        }
        m.mean_time_to_fill_ms = wait_sum / static_cast<double>(fills.size());
        m.spread_capture_bps = capture_sum / static_cast<double>(fills.size());

        for (int h = 0; h < 4; ++h)
        {
            double sum = 0.0;
            std::size_t n = 0;
            for (const auto& f : fills)
            {
                const int target = f.step + markout_steps[h];
                if (target >= sim_steps)
                    continue;
                sum += markout_bps(f.side, f.price, mid_at(tape, target, tick_raw));
                ++n;
            }
            m.markout_samples[h] = n;
            m.markout_bps[h] = n == 0 ? 0.0 : sum / static_cast<double>(n);
        }
    }

    std::sort(utilisations.begin(), utilisations.end());
    if (!utilisations.empty())
        m.median_abs_utilisation = utilisations[utilisations.size() / 2];

    return m;
}

mm_config sensitivity_config()
{
    auto cfg = default_config();
    cfg.levels = 2;
    // Tuned to the synthetic tape, not to its P&L: the half spread has to
    // land near the book's own half spread or the quotes never reach the
    // touch and every queue model reports zero fills. The reference config's
    // 6 bps floor is far outside a 1.8-3.3 bps book.
    cfg.spread.min_half_spread_bps = 0.3;
    cfg.spread.max_half_spread_bps = 6.0;
    cfg.spread.fee_buffer_bps = 0.0;
    cfg.spread.maker_fee_multiplier = 1.0;   // 1 bp maker fee from the instrument
    cfg.spread.volatility_multiplier = 0.2;
    cfg.spread.toxicity_multiplier = 0.2;
    cfg.spread.latency_multiplier = 0.5;
    cfg.quotes.level_spacing_bps = 0.5;
    cfg.quotes.base_size = atoms_from_decimal("0.05");
    // Quotes must survive longer than one step to have a queue position at
    // all; the churn guard is what gives them a lifetime.
    cfg.quotes.minimum_refresh_ticks = 8;
    cfg.quotes.minimum_quote_lifetime_ms = 25;
    cfg.inventory.hard_limit_base = atoms_from_decimal("0.60");
    cfg.inventory.reservation_skew_bps_at_hard_limit = 1.2;
    return cfg;
}

void print_report(const std::vector<run_metrics>& runs)
{
    std::printf("\n=== R1 queue sensitivity (%d steps, seed 0x%llX) ===\n", sim_steps,
                static_cast<unsigned long long>(sim_seed));
    std::printf("%-8s %12s %10s %12s %10s %12s %10s %10s\n", "queue", "net_pnl",
                "fees", "fills/quotes", "fill_rt", "ttf_ms", "med|u|", "max|u|");
    for (const auto& r : runs)
        std::printf("%-8s %12.4f %10.4f %6zu/%-6zu %10.4f %12.2f %10.3f %10.3f\n",
                    r.queue_model.c_str(), r.net_pnl, r.fees, r.quotes_filled,
                    r.quotes_placed, r.fill_ratio, r.mean_time_to_fill_ms,
                    r.median_abs_utilisation, r.max_abs_utilisation);

    std::printf("\n%-8s %12s", "queue", "capture_bps");
    for (const char* label : markout_labels)
        std::printf(" %10s", label);
    std::printf("\n");
    for (const auto& r : runs)
    {
        std::printf("%-8s %12.4f", r.queue_model.c_str(), r.spread_capture_bps);
        for (int h = 0; h < 4; ++h)
            std::printf(" %10.4f", r.markout_bps[h]);
        std::printf("\n");
    }
    std::printf("\n");
}

} // namespace

TEST(MMStrategyQueueSensitivity, MarkoutSignConventionIsMirroredForBuysAndSells)
{
    const Price fill = price_from_decimal("60000.00");

    // Mid rose after the fill: good for a buy, bad for a sell.
    EXPECT_NEAR(markout_bps(order_side::buy, fill, 60006.0), 1.0, 1e-9);
    EXPECT_NEAR(markout_bps(order_side::sell, fill, 60006.0), -1.0, 1e-9);

    // Mid fell after the fill: bad for a buy, good for a sell.
    EXPECT_NEAR(markout_bps(order_side::buy, fill, 59994.0), -1.0, 1e-9);
    EXPECT_NEAR(markout_bps(order_side::sell, fill, 59994.0), 1.0, 1e-9);

    EXPECT_DOUBLE_EQ(markout_bps(order_side::buy, fill, 60000.0), 0.0);
}

TEST(MMStrategyQueueSensitivity, InvariantsHoldUnderEveryQueueAssumption)
{
    const auto tape = build_tape();
    const auto cfg = sensitivity_config();

    FrontCancelModel front;
    UniformCancelModel uniform;
    BackCancelModel back;

    std::vector<run_metrics> runs;
    runs.push_back(run_simulation(tape, front, "Front", cfg));
    runs.push_back(run_simulation(tape, uniform, "Uniform", cfg));
    runs.push_back(run_simulation(tape, back, "Back", cfg));

    print_report(runs);

    for (const auto& r : runs)
    {
        SCOPED_TRACE(r.queue_model);
        // The safety invariants must hold regardless of how optimistic the
        // queue assumption is.
        EXPECT_EQ(r.hard_limit_breaches, 0u);
        EXPECT_EQ(r.invalid_quotes, 0u);
        EXPECT_LE(r.max_abs_utilisation, 1.0 + 1e-9);
        EXPECT_GT(r.quotes_placed, 0u);
        for (int h = 0; h < 4; ++h)
            EXPECT_GT(r.markout_samples[h], 0u) << markout_labels[h];
    }

    // Front is by construction the most optimistic cancel assumption, so it
    // can never fill less than Back on the same tape.
    EXPECT_GE(runs[0].quotes_filled, runs[2].quotes_filled);

    // Fixed inputs, fixed seed: rerunning must reproduce the run exactly.
    FrontCancelModel front_again;
    const auto repeat = run_simulation(tape, front_again, "Front", cfg);
    EXPECT_EQ(repeat.quotes_filled, runs[0].quotes_filled);
    EXPECT_DOUBLE_EQ(repeat.net_pnl, runs[0].net_pnl);
}
