// End-to-end coverage for bracket lifecycle inside the engine:
//   1. SL declared at entry actually fires on a bar wick (not just close).
//   2. When a strategy closes via signal in on_market without setting
//      opener_order_id, the leftover armed bracket is swept on the
//      net-flat transition - preventing phantom closes later.

#include <gtest/gtest.h>

#include "engine/engine.h"
#include "engine/engine_config.h"
#include "data/data_handler.h"
#include "orderbook/orderbook.h"
#include "market_maker/market_maker.h"
#include "exits/exit_intent.h"
#include "strategy/strategy_interface.h"

#include <array>
#include <chrono>
#include <initializer_list>
#include <iostream>
#include <sstream>

namespace {
struct SilenceCout {
    std::ostringstream sink;
    std::streambuf* orig;
    SilenceCout() : sink(), orig(std::cout.rdbuf(sink.rdbuf())) {}
    ~SilenceCout() { std::cout.rdbuf(orig); }
};

// Strategy that buys once on bar 3 with a tight bracket, then idle.
class BracketEntryStrategy : public IStrategy
{
    int call_count_ = 0;
    bool issued_ = false;
    std::optional<truetest::exits::exit_intent> pending_;
public:
    int fills_seen = 0;
    std::vector<order_side> fill_sides;

    std::optional<order_event> on_market(const market_event& mkt) override
    {
        ++call_count_;
        if (call_count_ == 3 && !issued_)
        {
            issued_ = true;
            const double entry = mkt.get_close();
            pending_ = truetest::exits::make_long_exit_intent(
                mkt.get_symbol(), entry, 1.0,
                /*sl_pct=*/0.01, /*tp_pct=*/0.01, "bracket");
            return order_event(mkt.get_timestamp(), mkt.get_symbol(),
                               order_type::market, order_side::buy,
                               1.0, entry);
        }
        return std::nullopt;
    }

    std::optional<truetest::exits::exit_intent> take_pending_exit_intent() override
    {
        auto out = std::move(pending_);
        pending_.reset();
        return out;
    }

    void on_fill(const fill_event& f, std::uint64_t) override
    {
        ++fills_seen;
        fill_sides.push_back(f.get_side());
    }

    void set_position_open(const std::string&, bool) override {}
};

// Strategy that buys on bar 3, then closes with a signal SELL on bar 5
// without setting opener_order_id. Models the legacy mean-reversion pattern.
class SignalCloseStrategy : public IStrategy
{
    int call_count_ = 0;
    bool entered_ = false;
    bool exited_ = false;
    std::optional<truetest::exits::exit_intent> pending_;
public:
    int fills_seen = 0;
    std::vector<order_side> fill_sides;

    std::optional<order_event> on_market(const market_event& mkt) override
    {
        ++call_count_;
        if (call_count_ == 3 && !entered_)
        {
            entered_ = true;
            const double entry = mkt.get_close();
            pending_ = truetest::exits::make_long_exit_intent(
                mkt.get_symbol(), entry, 1.0,
                /*sl_pct=*/0.50, /*tp_pct=*/0.50, "signal");
            return order_event(mkt.get_timestamp(), mkt.get_symbol(),
                               order_type::market, order_side::buy,
                               1.0, entry);
        }
        if (call_count_ == 5 && entered_ && !exited_)
        {
            exited_ = true;
            return order_event(mkt.get_timestamp(), mkt.get_symbol(),
                               order_type::market, order_side::sell,
                               1.0, mkt.get_close());
        }
        return std::nullopt;
    }

    std::optional<truetest::exits::exit_intent> take_pending_exit_intent() override
    {
        auto out = std::move(pending_);
        pending_.reset();
        return out;
    }

    void on_fill(const fill_event& f, std::uint64_t) override
    {
        ++fills_seen;
        fill_sides.push_back(f.get_side());
    }

    void set_position_open(const std::string&, bool) override {}
};

static auto make_data(std::initializer_list<std::array<double,4>> bars)
{
    // Each bar: open, high, low, close.
    auto dh = std::make_shared<data_handler>();
    for (const auto& b : bars)
        dh->load_into_queue("2024-01-01", "TEST", b[0], b[1], b[2], b[3], 1000);
    return dh;
}
}

TEST(EngineBrackets, SlFiresOnIntraBarWickEvenWhenCloseRecovers)
{
    SilenceCout quiet;
    // Bars: warmup, warmup, ENTRY (close=100, SL=99, TP=101),
    // wick bar (low=98 < SL=99 but close=100), pad.
    auto dh = make_data({
        {100, 100.5,  99.5, 100},
        {100, 100.5,  99.5, 100},
        {100, 100.5,  99.5, 100},   // entry on bar 3 close=100, SL=99
        {100, 100.5,  98.0, 100},   // wick to 98 - must trigger SL
        {100, 100.5,  99.5, 100},
        {100, 100.5,  99.5, 100},
    });
    auto ob = std::make_shared<orderbook>();
    auto strat = std::make_shared<BracketEntryStrategy>();

    MarketMaker mm;
    mm.add_orders(ob, 100.0, 10);

    engine_config cfg;
    engine eng(dh, ob, strat, std::move(cfg));
    eng.set_primary_strategy_name("bracket");
    eng.run();

    // Expect BUY (entry) + SELL (SL closer).
    ASSERT_GE(strat->fills_seen, 2)
        << "SL did not fire - bar low=98 should have crossed SL=99";
    EXPECT_EQ(strat->fill_sides[0], order_side::buy);
    EXPECT_EQ(strat->fill_sides[1], order_side::sell);
}

TEST(EngineBrackets, NetFlatSweepCancelsLeftoverBracket)
{
    SilenceCout quiet;
    // Strategy enters bar 3, closes via signal SELL bar 5. Bar 7 prints
    // a price that would have hit the (wide) SL = 50. With the sweep
    // wired in, the bracket is gone after the signal close, so no
    // phantom SELL must fire on bar 7.
    auto dh = make_data({
        {100, 100.5,  99.5, 100},
        {100, 100.5,  99.5, 100},
        {100, 100.5,  99.5, 100},   // bar 3: BUY entry, SL=50, TP=150
        {100, 100.5,  99.5, 100},
        {100, 100.5,  99.5, 100},   // bar 5: signal SELL closes lot
        {100, 100.5,  99.5, 100},
        { 49,  50.0,  48.0,  49},   // would hit SL=50 if bracket leaked
        {100, 100.5,  99.5, 100},
    });
    auto ob = std::make_shared<orderbook>();
    auto strat = std::make_shared<SignalCloseStrategy>();

    MarketMaker mm;
    mm.add_orders(ob, 100.0, 10);

    engine_config cfg;
    engine eng(dh, ob, strat, std::move(cfg));
    eng.set_primary_strategy_name("signal");
    eng.run();

    // Exactly two fills: BUY (entry) + SELL (signal close).
    // A third SELL would mean a phantom bracket fired on bar 7.
    EXPECT_EQ(strat->fills_seen, 2)
        << "Phantom bracket fire - net-flat sweep did not cancel the leftover intent";
}

// The SL closer must fill anchored at the SL level (book re-centered at
// 99 → bid 99 × 0.998), not against the next bar's open-centered book
// (≈ 100 × 0.998) — the pre-anchoring behavior deferred the fire through
// execution_bar_delay and discarded the fire price.
TEST(EngineBrackets, SlCloserFillsAnchoredAtSlLevelNotNextOpen)
{
    SilenceCout quiet;
    auto dh = make_data({
        {100, 100.5,  99.5, 100},
        {100, 100.5,  99.5, 100},
        {100, 100.5,  99.5, 100},   // entry signal on bar 3 (close=100, SL=99)
        {100, 100.5,  99.5, 100},   // entry fills on bar 4 open; bracket arms
        {100, 100.5,  98.0, 100},   // wick to 98 — SL 99 fires, anchored
        {100, 100.5,  99.5, 100},
    });
    auto strat = std::make_shared<BracketEntryStrategy>();

    engine_config cfg;
    cfg.seed = 1;
    // One deterministic level per side at ref × (1 ± 0.002).
    cfg.mm_levels_per_side  = 1;
    cfg.mm_base_spread_pct  = 0.002;
    cfg.mm_vol_spread_mult  = 0.0;

    engine eng(dh, nullptr, strat, std::move(cfg));
    eng.set_primary_strategy_name("bracket");
    eng.run();

    auto report = eng.get_analytics().generate_report();
    double buy_px = 0.0;
    double sell_px = 0.0;
    for (const auto& t : report.trades)
    {
        if (t.side == order_side::buy && buy_px == 0.0)
            buy_px = t.fill_price;
        if (t.side == order_side::sell && sell_px == 0.0)
            sell_px = t.fill_price;
    }

    ASSERT_GT(buy_px, 0.0) << "entry must fill";
    ASSERT_GT(sell_px, 0.0) << "SL closer must fill";
    // Entry-relative brackets preserve absolute |entry − SL| distance when the
    // opener fills away from the signal mid (resting ask). Signal mid=100,
    // sl_pct=1% → designed distance 1.0; actual SL = buy_fill − 1.0.
    // Book re-centered at that SL → bid = SL × 0.998.
    const double sl = buy_px - 1.0;
    EXPECT_NEAR(sell_px, sl * 0.998, 1e-3)
        << "SL closer anchored at the entry-relative SL level, not the next bar's open"
        << " (buy=" << buy_px << " sl=" << sl << " sell=" << sell_px << ")";
}
