// Forensic trade-lifecycle audit — engine/analytics/portfolio half.
// docs/todos/11-F-forensic-lifecycle-audit.md
//
//   F-05a  equity is allowed to go and stay negative with no bankruptcy stop
//   F-06   exit-intent lifecycle is invisible in the report
//   F-07a  --instrument specs that bind to no symbol are silently inert
//   F-09b  total trades counts closing fill legs, not closed round trips
//   F-10   two parallel strategy registries, one of them stale

#include "helpers/backtest_defect_helpers.h"

#include "analytics/report_generator.h"
#include "strategy/strategy_factory.h"
#include "strategy/strategy_registry.h"

#include <algorithm>
#include <filesystem>
#include <fstream>

using namespace bt_defect;

namespace {

fill_event fill_at(int64_t ms, const std::string& sym, std::uint64_t oid,
                   order_side side, double qty, double px)
{
    return fill_event(t_at(static_cast<int>(ms)), sym, oid, side, qty, px,
                      /*commission=*/0.0, /*remaining=*/0.0, /*fill_id=*/oid);
}

}  // namespace

// ── F-05a: no bankruptcy stop ──────────────────────────────────────────────

TEST(ForensicEngine, F05a_PortfolioLatchesBankruptcyAndNeverUnlatches)
{
    portfolio p;
    EXPECT_FALSE(p.is_bankrupt());

    p.observe_marked_equity(5000.0);
    EXPECT_FALSE(p.is_bankrupt());

    p.observe_marked_equity(-101678.92);
    ASSERT_TRUE(p.is_bankrupt());
    EXPECT_NEAR(p.bankrupt_equity(), -101678.92, 1e-9);

    // A later favourable mark does not resurrect a wiped account.
    p.observe_marked_equity(250.0);
    EXPECT_TRUE(p.is_bankrupt());
    EXPECT_NEAR(p.bankrupt_equity(), -101678.92, 1e-9);
}

TEST(ForensicEngine, F05a_UnusableMarkIsNotABankruptcy)
{
    portfolio p;
    // A missing mark surfaces as NaN upstream. That is a data-quality
    // condition, and must never be read as a wiped account.
    p.observe_marked_equity(std::numeric_limits<double>::quiet_NaN());
    EXPECT_FALSE(p.is_bankrupt());
}

TEST(ForensicEngine, F05a_ResetClearsTheLatchForTheNextMcTrial)
{
    portfolio p;
    p.observe_marked_equity(-1.0);
    ASSERT_TRUE(p.is_bankrupt());
    p.reset();
    EXPECT_FALSE(p.is_bankrupt());
    EXPECT_EQ(p.bankrupt_equity(), 0.0);
}

TEST(ForensicEngine, F05a_ReportIsMarkedInvalidInsteadOfShowingASharpeRatio)
{
    Analytics a(10000.0);
    a.mark_bankrupt(-101678.92);

    const auto r = a.generate_report();
    ASSERT_TRUE(r.bankrupt);
    EXPECT_NEAR(r.bankrupt_equity, -101678.92, 1e-9);

    const auto text = tt::render_report(r);
    EXPECT_NE(text.find("RUN INVALID"), std::string::npos)
        << "a wiped account must not print a report that reads as valid";
    EXPECT_NE(text.find("ACCOUNT BANKRUPT"), std::string::npos);
}

TEST(ForensicEngine, F05a_HealthyRunCarriesNoInvalidityBanner)
{
    Analytics a(10000.0);
    const auto text = tt::render_report(a.generate_report());
    EXPECT_EQ(text.find("RUN INVALID"), std::string::npos);
}

// ── F-06: exit-intent lifecycle is invisible ───────────────────────────────

TEST(ForensicEngine, F06_ExitIntentLifecycleReachesTheReport)
{
    Analytics a(10000.0);
    Analytics::exit_lifecycle_counts c;
    c.registered = 5;
    c.armed = 4;              // opener 8369 orphaned — the audit's trace
    c.cancelled = 1;
    c.evicted = 2;
    c.slippage_disarms = 3;
    c.flatten_requests = 3;
    a.fold_lifecycle_counters(false, 0.0, c);

    const auto r = a.generate_report();
    EXPECT_EQ(r.exit_intents_registered, 5u);
    EXPECT_EQ(r.exit_intents_armed, 4u);
    EXPECT_EQ(r.exit_intents_cancelled, 1u);
    EXPECT_EQ(r.exit_intents_evicted, 2u);
    EXPECT_EQ(r.exit_slippage_disarms, 3u);

    const auto text = tt::render_report(r);
    EXPECT_NE(text.find("exit intents reg/armed"), std::string::npos);
    EXPECT_NE(text.find("EVICTED"), std::string::npos)
        << "a leaked intent must be visible without inspecting the container";
    EXPECT_NE(text.find("brackets refused (slippage)"), std::string::npos);
}

// ── F-07a: --instrument specs that bind to nothing ─────────────────────────

TEST(ForensicEngine, F07a_InstrumentSpecMatchingNoSymbolFailsClosed)
{
    SilenceOutput silence;
    auto dh = std::make_shared<data_handler>();
    load_bars(*dh, 10, 100.0);           // symbol "BTCUSDT"

    engine_config cfg;
    cfg.mode = engine_mode::backtest;
    cfg.show_progress = false;
    cfg.initial_balance = 10000;
    instrument_spec spec;
    spec.symbol = "ETHUSDT";             // not in the loaded series
    spec.tick_size = 0.01;
    spec.lot_size = 1.0;
    spec.min_notional = 1e8;
    cfg.instrument_overrides["ETHUSDT"] = spec;

    auto strat = std::make_shared<OneShotMarketBuy>();
    auto eng = std::make_unique<engine>(dh, nullptr, strat, std::move(cfg));

    // Pre-fix this ran to completion with byte-identical output: every
    // filter in the spec was inert and nothing said so.
    EXPECT_THROW(eng->run(), std::runtime_error);
}

TEST(ForensicEngine, F07a_UnboundSymbolColumnIsNamedInTheError)
{
    SilenceOutput silence;
    auto dh = std::make_shared<data_handler>();
    // Symbol-less source: every bar carries "" (the Binance CSV shape).
    for (int i = 0; i < 5; ++i) {
        const long long ts = 1704067200000LL + static_cast<long long>(i) * 60000LL;
        dh->load_into_queue(std::to_string(ts), "", 100.0, 100.5, 99.5, 100.0, 1000);
    }

    engine_config cfg;
    cfg.mode = engine_mode::backtest;
    cfg.show_progress = false;
    cfg.initial_balance = 10000;
    instrument_spec spec;
    spec.symbol = "BTCUSDT";
    spec.tick_size = 0.01;
    cfg.instrument_overrides["BTCUSDT"] = spec;

    auto strat = std::make_shared<OneShotMarketBuy>();
    auto eng = std::make_unique<engine>(dh, nullptr, strat, std::move(cfg));
    try {
        eng->run();
        FAIL() << "an --instrument spec that binds to nothing must not run silently";
    } catch (const std::runtime_error& e) {
        const std::string msg = e.what();
        EXPECT_NE(msg.find("BTCUSDT"), std::string::npos);
        EXPECT_NE(msg.find("--symbol"), std::string::npos)
            << "the message must point at the actual cause; got: " << msg;
    }
}

TEST(ForensicEngine, F07a_MatchingInstrumentSpecStillRuns)
{
    SilenceOutput silence;
    auto dh = std::make_shared<data_handler>();
    load_bars(*dh, 10, 100.0);

    engine_config cfg;
    cfg.mode = engine_mode::backtest;
    cfg.show_progress = false;
    cfg.initial_balance = 10000;
    instrument_spec spec;
    spec.symbol = "BTCUSDT";
    spec.tick_size = 0.01;
    cfg.instrument_overrides["BTCUSDT"] = spec;

    auto strat = std::make_shared<OneShotMarketBuy>();
    auto eng = std::make_unique<engine>(dh, nullptr, strat, std::move(cfg));
    EXPECT_NO_THROW(eng->run());
}

// ── F-09b: trades counted per closing leg ──────────────────────────────────

TEST(ForensicEngine, F09b_MultiLegExitOfOneLotIsOneTrade)
{
    Analytics a(10000.0);
    auto mkt = std::make_shared<market_event>(t_at(0), "X", 100, 100, 100, 100.0);
    a.on_event(mkt);

    auto entry = std::make_shared<fill_event>(fill_at(1, "X", 1, order_side::buy, 4.0, 100.0));
    a.on_event(entry);

    // One exit walking four book levels — one trade that filled in four
    // pieces, not four trades.
    for (int i = 0; i < 4; ++i)
    {
        auto leg = std::make_shared<fill_event>(
            fill_at(2 + i, "X", 2, order_side::sell, 1.0, 110.0 + i));
        a.on_event(leg);
    }

    const auto r = a.generate_report();
    EXPECT_EQ(r.total_trades, 1u) << "a round trip is one trade";
    EXPECT_EQ(r.closing_fill_legs, 4u) << "leg count stays visible separately";
    EXPECT_EQ(r.winning_trades, 1u);
    EXPECT_NEAR(r.win_rate, 100.0, 1e-9);
    // 1*10 + 1*11 + 1*12 + 1*13
    EXPECT_NEAR(r.realized_pnl, 46.0, 1e-9);
    EXPECT_NEAR(r.largest_winner, 46.0, 1e-9)
        << "the largest winner is the round trip, not its biggest leg";
}

TEST(ForensicEngine, F09b_SingleLegRoundTripsAreUnchanged)
{
    Analytics a(10000.0);
    auto mkt = std::make_shared<market_event>(t_at(0), "X", 100, 100, 100, 100.0);
    a.on_event(mkt);
    auto bf = std::make_shared<fill_event>(fill_at(1, "X", 1, order_side::buy, 10, 100.0));
    a.on_event(bf);
    auto sf = std::make_shared<fill_event>(fill_at(2, "X", 2, order_side::sell, 10, 110.0));
    a.on_event(sf);

    const auto r = a.generate_report();
    EXPECT_EQ(r.total_trades, 1u);
    EXPECT_EQ(r.closing_fill_legs, 1u);
    EXPECT_NEAR(r.realized_pnl, 100.0, 1e-9);
}

TEST(ForensicEngine, F09b_PartialScaleOutRealizesPnlBeforeTheTripCloses)
{
    Analytics a(10000.0);
    auto mkt = std::make_shared<market_event>(t_at(0), "X", 100, 100, 100, 100.0);
    a.on_event(mkt);
    auto bf = std::make_shared<fill_event>(fill_at(1, "X", 1, order_side::buy, 10, 100.0));
    a.on_event(bf);
    auto tp1 = std::make_shared<fill_event>(fill_at(2, "X", 2, order_side::sell, 4, 110.0));
    a.on_event(tp1);

    auto mid = a.generate_report();
    EXPECT_EQ(mid.total_trades, 0u) << "the round trip is not closed yet";
    EXPECT_NEAR(mid.realized_pnl, 40.0, 1e-9)
        << "realized cash must stay exact across a partial exit";

    auto tp2 = std::make_shared<fill_event>(fill_at(3, "X", 3, order_side::sell, 6, 120.0));
    a.on_event(tp2);

    auto done = a.generate_report();
    EXPECT_EQ(done.total_trades, 1u);
    EXPECT_EQ(done.closing_fill_legs, 2u);
    EXPECT_NEAR(done.realized_pnl, 160.0, 1e-9);
}

// ── F-10: two parallel strategy registries ─────────────────────────────────

TEST(ForensicEngine, F10_FactoryAndRegistryAgreeOnWhatExists)
{
    auto factory = StrategyFactory::available();
    auto registry = StrategyRegistry::instance().available();
    std::sort(factory.begin(), factory.end());
    std::sort(registry.begin(), registry.end());
    EXPECT_EQ(factory, registry)
        << "one registry, one list — a second hardcoded table drifts";
}

TEST(ForensicEngine, F10_FactoryExposesEveryRegisteredStrategy)
{
    const auto names = StrategyFactory::available();
    EXPECT_NE(std::find(names.begin(), names.end(), "ema-rsi-atr-pullback"),
              names.end())
        << "the audited strategy was constructible but absent from available()";
}

TEST(ForensicEngine, F10_UnknownStrategyThrowsInsteadOfSilentlyFallingBack)
{
    // Pre-fix any unrecognised name returned mean-reversion, so a typo
    // silently ran a different strategy than the one that was asked for.
    EXPECT_THROW(StrategyFactory::create("no-such-strategy"), std::runtime_error);
    EXPECT_FALSE(StrategyFactory::has("no-such-strategy"));
}

TEST(ForensicEngine, F10_FactoryStillBuildsARegisteredStrategy)
{
    auto s = StrategyFactory::create("sma");
    ASSERT_NE(s, nullptr);
    EXPECT_TRUE(StrategyFactory::has("sma"));
}

// ── F-02: a rejected bar-delayed order silently deadlocks the strategy ─────
//
// route() parks a bar-delayed order and returns `pending`, so finalize_route
// takes the NON-rejected path and arms the exit intent. The rejection itself
// lands one bar later inside drain_due -> process(), which has no
// finalize_route and told nobody. Traced on BTCUSDT: order 8369 rejected at
// bar 410 (rule=drawdown), then 1,713,190 silent bars and a complete
// analytics report with a Sharpe ratio for a run that had stopped trading.

namespace {

// Emits one market BUY per bar and refuses to emit again while it believes a
// position is open. That belief is only ever cleared by set_position_open —
// i.e. by the terminal-transition notification F-02 adds. This is the same
// optimistic entry gate the audited ema-rsi-atr-pullback strategy uses.
class GatedEntryStrategy : public IStrategy {
public:
    int orders_emitted = 0;
    int flat_notifications = 0;

    std::optional<order_event> on_market(const market_event& m) override
    {
        if (in_position_) return std::nullopt;
        in_position_ = true;
        ++orders_emitted;
        return order_event(m.get_timestamp(), m.get_symbol(),
                           order_type::market, order_side::buy, 1.0, m.get_close());
    }

    void set_position_open(const std::string&, bool open) override
    {
        if (!open) { in_position_ = false; ++flat_notifications; }
    }

private:
    bool in_position_ = false;
};

// Every order refused by the portfolio drawdown rule; soft portfolio limits
// in backtest mode turn that halt into a reject, which is exactly the
// configuration the audit traced.
engine_config always_reject_cfg(int bar_delay)
{
    engine_config cfg;
    cfg.mode = engine_mode::backtest;
    cfg.show_progress = false;
    cfg.initial_balance = 10000;
    cfg.execution_bar_delay = bar_delay;
    cfg.risk.max_drawdown = 0.0;             // 0 >= 0 — refuses from bar one
    cfg.risk_soft_portfolio_limits = true;
    return cfg;
}

}  // namespace

TEST(ForensicEngine, F02_RejectedBarDelayedOrderReturnsTheStrategyToFlat)
{
    SilenceOutput silence;
    auto dh = std::make_shared<data_handler>();
    load_bars(*dh, 30, 100.0);

    auto strat = std::make_shared<GatedEntryStrategy>();
    auto eng = std::make_unique<engine>(dh, nullptr, strat,
                                        always_reject_cfg(/*bar_delay=*/1));
    eng->run();

    // Pre-fix: exactly one order, then permanent silence. The rejection
    // happened inside drain_due -> process(), which had no finalize_route,
    // so the optimistic gate never cleared.
    EXPECT_GT(strat->orders_emitted, 1)
        << "the strategy must keep trading after a rejection, not go silent";
    EXPECT_GT(strat->flat_notifications, 0)
        << "every terminal non-fill outcome must resync the position gate";
}

TEST(ForensicEngine, F02_SameHoldsWithoutBarDelay)
{
    SilenceOutput silence;
    auto dh = std::make_shared<data_handler>();
    load_bars(*dh, 30, 100.0);

    auto strat = std::make_shared<GatedEntryStrategy>();
    auto eng = std::make_unique<engine>(dh, nullptr, strat,
                                        always_reject_cfg(/*bar_delay=*/0));
    eng->run();
    EXPECT_GT(strat->orders_emitted, 1);
}

namespace {

// Emits unconditionally so several orders are in flight at once — the only
// way to actually exhaust an open-order cap.
class AlwaysEmitStrategy : public IStrategy {
public:
    int orders_emitted = 0;
    int flat_notifications = 0;

    std::optional<order_event> on_market(const market_event& m) override
    {
        ++orders_emitted;
        return order_event(m.get_timestamp(), m.get_symbol(),
                           order_type::market, order_side::buy, 1.0, m.get_close());
    }
    void set_position_open(const std::string&, bool open) override
    {
        if (!open) ++flat_notifications;
    }
};

}  // namespace

TEST(ForensicEngine, F02_MaxOpenOrdersRejectionAlsoResyncsTheGate)
{
    SilenceOutput silence;
    auto dh = std::make_shared<data_handler>();
    load_bars(*dh, 30, 100.0);

    auto strat = std::make_shared<AlwaysEmitStrategy>();
    engine_config cfg;
    cfg.mode = engine_mode::backtest;
    cfg.show_progress = false;
    cfg.initial_balance = 10000;
    // Bar-delayed orders rest in the scheduler while holding a ledger slot,
    // so a one-order cap is exhausted from the second bar onward.
    cfg.execution_bar_delay = 3;
    cfg.risk.max_open_orders = 1;

    auto eng = std::make_unique<engine>(dh, nullptr, strat, std::move(cfg));
    eng->run();

    const auto r = eng->get_analytics().generate_report();
    EXPECT_GT(strat->orders_emitted, 1);
    EXPECT_GT(strat->flat_notifications, 0)
        << "the capacity rejection path must emit the same notification";
    EXPECT_EQ(r.exit_intents_evicted, 0u);
}


TEST(ForensicEngine, F02_VenueFilterRejectionAlsoResyncsTheGate)
{
    SilenceOutput silence;
    auto dh = std::make_shared<data_handler>();
    load_bars(*dh, 30, 100.0);

    auto strat = std::make_shared<GatedEntryStrategy>();
    engine_config cfg;
    cfg.mode = engine_mode::backtest;
    cfg.show_progress = false;
    cfg.initial_balance = 10000;
    cfg.execution_bar_delay = 1;
    instrument_spec spec;
    spec.symbol = "BTCUSDT";
    spec.min_notional = 1e12;       // nothing can satisfy it
    cfg.instrument_overrides["BTCUSDT"] = spec;

    auto eng = std::make_unique<engine>(dh, nullptr, strat, std::move(cfg));
    eng->run();
    EXPECT_GT(strat->orders_emitted, 1)
        << "the venue-filter rejection path must emit the same notification";
}

TEST(ForensicEngine, F02_RejectedOrderDoesNotLeakItsExitIntent)
{
    SilenceOutput silence;
    auto dh = std::make_shared<data_handler>();
    load_bars(*dh, 30, 100.0);

    auto strat = std::make_shared<GatedEntryStrategy>();
    auto cfg = always_reject_cfg(/*bar_delay=*/1);
    // Platform floor attaches an SL/TP to every entry, so each rejected order
    // has a pending exit intent behind it — F-06's leak.
    cfg.exit_defaults.mode = truetest::exits::exit_policy_mode::floor;
    cfg.exit_defaults.sl_pct = 0.05;
    cfg.exit_defaults.tp_pct = 0.10;

    auto eng = std::make_unique<engine>(dh, nullptr, strat, std::move(cfg));
    eng->run();

    const auto r = eng->get_analytics().generate_report();
    EXPECT_GT(r.exit_intents_registered, 1u)
        << "the scenario must actually register intents to be meaningful";
    EXPECT_EQ(r.exit_intents_armed, 0u)
        << "nothing fills in this scenario, so nothing can arm";
    // Every intent this run registered was released by the terminal
    // transition of the order that owned it. (The counters are not a
    // partition in general — an intent that arms and is later cancelled is
    // counted in both — which is exactly why this asserts the armed-free
    // case rather than a sum.)
    EXPECT_EQ(r.exit_intents_cancelled, r.exit_intents_registered)
        << "a rejected order must release the intent registered for it";
    EXPECT_EQ(r.exit_intents_evicted, 0u)
        << "the capacity bound must never be the thing that stops the leak";

}

TEST(ForensicEngine, F02_EveryTerminalStatusSiteRoutesThroughTheEmitter)
{
    // Invariant guard. The plan's own warning is that patching one call site
    // reopens the hole the next time a rejection rule is added, so every
    // set_status(..., rejected|cancelled|expired) in the order pipeline must
    // live inside emit_terminal_transition — the one place allowed to write a
    // terminal status.
    const std::filesystem::path src =
        std::filesystem::path(__FILE__).parent_path().parent_path()
        / "src" / "engine" / "order_intent_processor.cpp";
    std::ifstream f(src);
    ASSERT_TRUE(f.is_open()) << "cannot read " << src;

    std::string line;
    std::string offenders;
    int lineno = 0;
    bool inside_emitter = false;
    while (std::getline(f, line))
    {
        ++lineno;
        if (line.find("void OrderIntentProcessor::emit_terminal_transition") !=
            std::string::npos)
            inside_emitter = true;
        else if (inside_emitter && line.rfind("void OrderIntentProcessor::", 0) == 0)
            inside_emitter = false;

        if (line.find("set_status(") == std::string::npos) continue;
        const bool terminal =
            line.find("order_status::rejected")  != std::string::npos ||
            line.find("order_status::cancelled") != std::string::npos ||
            line.find("order_status::expired")   != std::string::npos;
        if (!terminal || inside_emitter) continue;
        offenders += "  " + std::to_string(lineno) + ": " + line + "\n";
    }

    EXPECT_TRUE(offenders.empty())
        << "terminal status written outside emit_terminal_transition:\n" << offenders;
}
