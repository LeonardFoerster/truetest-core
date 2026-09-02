// Forensic trade-lifecycle audit — frozen-surface (Workstream B) regressions.
// docs/todos/11-F-forensic-lifecycle-audit.md
//
//   F-04   position sizing is blind to the only execution cost that exists
//   F-05b  no leverage or cash admission rule for spot
//   F-07b  --symbol never binds to a symbol-less bar source
//   F-08   order timestamps precede the information that produced them
//   F-10b  --strategy help is a hardcoded list, not the registry

#include "helpers/backtest_defect_helpers.h"

#include "data/market_series.h"
#include "risk/risk_manager.h"
#include "strategy/apply_execution_cost_params.h"
#include "strategy/strategy_registry.h"

#include <algorithm>

using namespace bt_defect;

namespace {

// Minimal strategy exposing the sizing knobs apply_execution_cost_params
// writes into, so the injected values are observable.
struct SlipProbeStrategy : IStrategy
{
    double entry_slip = -1.0;
    double exit_slip = -1.0;
    double entry_fee = -1.0;
    double exit_fee = -1.0;
    double fixed_fee = -1.0;

    std::optional<order_event> on_market(const market_event&) override
    {
        return std::nullopt;
    }
    std::vector<param_def> get_param_schema() const override
    {
        return {
            {"entry_slip_bps", 0.0, 0.0, 1000.0, ""},
            {"exit_slip_bps", 0.0, 0.0, 1000.0, ""},
            {"entry_fee_rate", 0.0, 0.0, 1.0, ""},
            {"exit_fee_rate", 0.0, 0.0, 1.0, ""},
            {"fixed_fee_per_leg", 0.0, 0.0, 1e6, ""},
        };
    }
    void set_param(const std::string& name, double value) override
    {
        if (name == "entry_slip_bps") entry_slip = value;
        else if (name == "exit_slip_bps") exit_slip = value;
        else if (name == "entry_fee_rate") entry_fee = value;
        else if (name == "exit_fee_rate") exit_fee = value;
        else if (name == "fixed_fee_per_leg") fixed_fee = value;
    }
};

risk_snapshot equity_snapshot(double equity, double mark, double gross_exposure)
{
    risk_snapshot s;
    s.equity = equity;
    s.ledger_authoritative = true;
    s.instrument.mark_price = mark;
    s.instrument.mark_state = mark_quality::valid;
    s.instrument.mark_age_ms = 0;
    s.instrument.exposure_tracked = true;
    s.portfolio.gross_exposure = gross_exposure;
    return s;
}

order_event buy(const std::string& sym, double qty, double px)
{
    order_event o(t_at(0), sym, order_type::market, order_side::buy, qty, px);
    o.set_order_id(1);
    return o;
}

}  // namespace

// ── F-04: sizing blind to execution cost ───────────────────────────────────

TEST(ForensicWorkstreamB, F04_SlippageComesFromTheExecutionModelNotTheDeadFlag)
{
    SlipProbeStrategy s;
    // --bar-spread-bps is documented "DEPRECATED, no effect" and defaults to
    // zero, so this used to inject nothing at all and sizing saw no cost.
    apply_execution_cost_params(s, /*maker=*/0.0, /*taker=*/0.0,
                                /*bar_spread_bps=*/0.0, /*fee_model=*/{},
                                /*fee_value=*/0.0, /*mm_half_spread_pct=*/0.002);

    EXPECT_NEAR(s.entry_slip, 20.0, 1e-9)
        << "--mm-spread-pct 0.002 is a 20 bps half spread, and a market entry "
           "crosses exactly that";
    EXPECT_NEAR(s.exit_slip, 20.0, 1e-9);
}

TEST(ForensicWorkstreamB, F04_DeadFlagAloneNoLongerDrivesSizing)
{
    SlipProbeStrategy s;
    apply_execution_cost_params(s, 0.0, 0.0, /*bar_spread_bps=*/0.0, {}, 0.0,
                                /*mm_half_spread_pct=*/0.0);
    EXPECT_LT(s.entry_slip, 0.0) << "nothing to inject when no model is given";
}

TEST(ForensicWorkstreamB, F04_ExplicitOperatorEstimateStillWins)
{
    SlipProbeStrategy s;
    // A deliberately larger manual estimate must not be silently narrowed to
    // the model's lower bound.
    apply_execution_cost_params(s, 0.0, 0.0, /*bar_spread_bps=*/100.0, {}, 0.0,
                                /*mm_half_spread_pct=*/0.002);
    EXPECT_NEAR(s.entry_slip, 50.0, 1e-9);
}

TEST(ForensicWorkstreamB, F04_ExplicitZeroFeeDoesNotLeakDefaultRatesIntoSizing)
{
    SlipProbeStrategy s;
    apply_execution_cost_params(s, /*maker=*/0.001, /*taker=*/0.001,
                                /*bar_spread_bps=*/0.0, /*fee_model=*/"zero");
    EXPECT_LT(s.entry_fee, 0.0);
    EXPECT_LT(s.exit_fee, 0.0);
}

TEST(ForensicWorkstreamB, F04_FixedFeeDoesNotAlsoInjectTieredRates)
{
    SlipProbeStrategy s;
    apply_execution_cost_params(s, /*maker=*/0.001, /*taker=*/0.001,
                                /*bar_spread_bps=*/0.0, /*fee_model=*/"fixed",
                                /*fee_value=*/5.0);

    EXPECT_LT(s.entry_fee, 0.0);
    EXPECT_LT(s.exit_fee, 0.0);
    EXPECT_DOUBLE_EQ(s.fixed_fee, 5.0);
}

// ── F-05b: no cash or leverage rule for spot ───────────────────────────────

TEST(ForensicWorkstreamB, F05b_SpotCannotBorrow)
{
    risk_limits lim;
    lim.max_gross_leverage = 1.0;      // cash account
    RiskManager rm(lim);
    portfolio p;

    // The audit's own numbers: $186,325 of notional on $10,000 of equity.
    auto o = buy("BTCUSDT", 3.7265, 50000.0);
    risk_rule rule = risk_rule::none;
    const auto action = rm.check_order(o, p, equity_snapshot(10000.0, 50000.0, 0.0),
                                       /*open_order_count=*/0, &rule);
    EXPECT_EQ(action, risk_action::reject);
    EXPECT_EQ(rule, risk_rule::gross_leverage);
}

TEST(ForensicWorkstreamB, F05b_ExposureInsideEquityIsAccepted)
{
    risk_limits lim;
    lim.max_gross_leverage = 1.0;
    RiskManager rm(lim);
    portfolio p;

    auto o = buy("BTCUSDT", 0.1, 50000.0);   // $5,000 on $10,000
    risk_rule rule = risk_rule::none;
    EXPECT_EQ(rm.check_order(o, p, equity_snapshot(10000.0, 50000.0, 0.0), 0, &rule),
              risk_action::pass);
    EXPECT_EQ(rule, risk_rule::none);
}

TEST(ForensicWorkstreamB, F05b_DisabledByDefaultSoEmbeddersAreUnchanged)
{
    risk_limits lim;                    // max_gross_leverage defaults to 0
    RiskManager rm(lim);
    portfolio p;
    auto o = buy("BTCUSDT", 3.7265, 50000.0);
    EXPECT_EQ(rm.check_order(o, p, equity_snapshot(10000.0, 50000.0, 0.0), 0),
              risk_action::pass);
}

TEST(ForensicWorkstreamB, F05b_ReductionsStayPermittedAfterABreach)
{
    risk_limits lim;
    lim.max_gross_leverage = 1.0;
    RiskManager rm(lim);
    portfolio p;

    // Already long 3.7 BTC; the sell reduces inventory and must pass even
    // though the account is far past the cap — otherwise a breached account
    // could never de-risk.
    fill_event entry(t_at(0), "BTCUSDT", 1, order_side::buy, 3.7265, 50000.0,
                     0.0, 0.0, 1);
    p.on_fill(entry);

    order_event sell(t_at(1), "BTCUSDT", order_type::market, order_side::sell,
                     3.7265, 50000.0);
    sell.set_order_id(2);
    EXPECT_EQ(rm.check_order(sell, p, equity_snapshot(10000.0, 50000.0, 186325.0), 0),
              risk_action::pass);
}

TEST(ForensicWorkstreamB, F05b_UnusableEquityRefusesRatherThanDisablingTheCap)
{
    risk_limits lim;
    lim.max_gross_leverage = 1.0;
    RiskManager rm(lim);
    portfolio p;
    auto o = buy("BTCUSDT", 0.1, 50000.0);
    risk_rule rule = risk_rule::none;
    EXPECT_EQ(rm.check_order(o, p, equity_snapshot(0.0, 50000.0, 0.0), 0, &rule),
              risk_action::reject);
    EXPECT_EQ(rule, risk_rule::invalid_equity);
}

// ── F-07b: --symbol never binds to symbol-less data ────────────────────────

TEST(ForensicWorkstreamB, F07b_UnboundBarsTakeTheConfiguredSymbol)
{
    MarketSeries series;
    for (int i = 0; i < 5; ++i)
        series.load_into_queue(std::to_string(1704067200000LL + i * 60000LL), "",
                               100.0, 100.5, 99.5, 100.0, 1000);

    ASSERT_TRUE(series.has_unbound_symbols());
    const auto bound = series.bind_unset_symbols("BTCUSDT");
    EXPECT_EQ(bound.bars, 5u);
    EXPECT_FALSE(series.has_unbound_symbols());
    for (std::size_t i = 0; i < series.bar_count(); ++i)
        EXPECT_EQ(series.bar_symbol_at(i), "BTCUSDT");
}

TEST(ForensicWorkstreamB, F07b_AlreadyNamedRowsAreNeverRewritten)
{
    MarketSeries series;
    series.load_into_queue("1704067200000", "ETHUSDT", 100, 101, 99, 100, 1000);
    series.load_into_queue("1704067260000", "", 100, 101, 99, 100, 1000);

    const auto bound = series.bind_unset_symbols("BTCUSDT");
    EXPECT_EQ(bound.bars, 1u) << "a real multi-symbol load must not be rewritten";
    EXPECT_EQ(series.bar_symbol_at(0), "ETHUSDT");
    EXPECT_EQ(series.bar_symbol_at(1), "BTCUSDT");
}

TEST(ForensicWorkstreamB, F07b_EmptySymbolBindsNothing)
{
    MarketSeries series;
    series.load_into_queue("1704067200000", "", 100, 101, 99, 100, 1000);
    const auto bound = series.bind_unset_symbols("");
    EXPECT_EQ(bound.bars, 0u);
    EXPECT_TRUE(series.has_unbound_symbols());
}

TEST(ForensicWorkstreamB, F07b_BoundSymbolMakesAnInstrumentSpecTakeEffect)
{
    SilenceOutput silence;
    auto dh = std::make_shared<data_handler>();
    for (int i = 0; i < 10; ++i) {
        const long long ts = 1704067200000LL + static_cast<long long>(i) * 60000LL;
        dh->load_into_queue(std::to_string(ts), "", 100.0, 100.5, 99.5, 100.0, 1000);
    }
    // The audit's own proof: a spec that should reject every order on
    // min-notional produced byte-identical output because it bound to nothing.
    dh->bind_unset_symbols("BTCUSDT");

    engine_config cfg;
    cfg.mode = engine_mode::backtest;
    cfg.show_progress = false;
    cfg.initial_balance = 10000;
    instrument_spec spec;
    spec.symbol = "BTCUSDT";
    spec.min_notional = 1e8;
    cfg.instrument_overrides["BTCUSDT"] = spec;

    auto strat = std::make_shared<OneShotMarketBuy>();
    auto eng = std::make_unique<engine>(dh, nullptr, strat, std::move(cfg));
    eng->run();

    EXPECT_EQ(eng->get_analytics().snapshot().total_fills, 0u)
        << "min-notional must actually reject once --symbol is bound";
}

// ── F-08: orders stamped before the information that produced them ─────────

TEST(ForensicWorkstreamB, F08_DecisionTimestampDefaultsToTheMarketDataClock)
{
    order_event o(t_at(1000), "X", order_type::market, order_side::buy, 1.0, 100.0);
    EXPECT_FALSE(o.has_decision_ts());
    EXPECT_EQ(o.get_decision_ts(), o.get_timestamp())
        << "an unstamped order must behave exactly as before";
}

TEST(ForensicWorkstreamB, F08_DecisionTimestampDoesNotOverwriteTheBarOpen)
{
    order_event o(t_at(1000), "X", order_type::market, order_side::buy, 1.0, 100.0);
    o.set_decision_ts(t_at(1060));
    EXPECT_EQ(o.get_timestamp(), t_at(1000))
        << "the bar open is the market-data clock and must survive";
    EXPECT_EQ(o.get_decision_ts(), t_at(1060));
}

TEST(ForensicWorkstreamB, F08_BarIntervalIsInferredFromTheSeries)
{
    SilenceOutput silence;
    auto dh = std::make_shared<data_handler>();
    load_bars(*dh, 30, 100.0);   // 60s bars

    auto strat = std::make_shared<OneShotMarketBuy>();
    engine_config cfg;
    cfg.mode = engine_mode::backtest;
    cfg.show_progress = false;
    cfg.initial_balance = 10000;
    auto eng = std::make_unique<engine>(dh, nullptr, strat, std::move(cfg));
    eng->run();

    // The offset the decision clock is stamped with. An order derived from
    // bar N's close is dated one interval after bar N's open, which is when
    // that close actually existed.
    EXPECT_EQ(eng->inferred_bar_interval(), std::chrono::seconds(60));
}

TEST(ForensicWorkstreamB, F08_IrregularSeriesGetsNoInventedInterval)
{
    SilenceOutput silence;
    auto dh = std::make_shared<data_handler>();
    // Deliberately irregular gaps: no honest interval exists, so the decision
    // clock must fall back to the bar open rather than guess one.
    const long long gaps[] = {0, 7000, 61000, 900000, 1000, 480000};
    long long ts = 1704067200000LL;
    for (long long g : gaps) {
        ts += g;
        dh->load_into_queue(std::to_string(ts), "BTCUSDT",
                            100.0, 100.5, 99.5, 100.0, 1000);
    }

    auto strat = std::make_shared<OneShotMarketBuy>();
    engine_config cfg;
    cfg.mode = engine_mode::backtest;
    cfg.show_progress = false;
    cfg.initial_balance = 10000;
    auto eng = std::make_unique<engine>(dh, nullptr, strat, std::move(cfg));
    eng->run();

    EXPECT_EQ(eng->inferred_bar_interval(),
              std::chrono::system_clock::duration::zero());
}


TEST(ForensicWorkstreamB, F08_RateLimitsUseTheDecisionClock)
{
    risk_limits lim;
    lim.max_orders_per_minute = 2;
    RiskManager rm(lim);
    portfolio p;

    // Three orders whose bar-open timestamps all fall inside one minute, but
    // whose decision instants are 60s apart. The window must follow the
    // decisions, not the market-data clock.
    for (int i = 0; i < 3; ++i)
    {
        order_event o(t_at(0), "X", order_type::market, order_side::buy, 1.0, 100.0);
        o.set_order_id(static_cast<std::uint64_t>(i + 1));
        o.set_decision_ts(t_at(0) + std::chrono::seconds(61 * i));
        risk_rule rule = risk_rule::none;
        EXPECT_EQ(rm.check_order(o, p, equity_snapshot(10000.0, 100.0, 0.0), 0, &rule),
                  risk_action::pass)
            << "order " << i << " rejected as rule " << to_string(rule);
    }
}

// ── F-10b: --strategy help is a stale hardcoded list ───────────────────────

TEST(ForensicWorkstreamB, F10b_RegistryIsTheOnlySourceOfTheStrategyList)
{
    const auto names = StrategyRegistry::instance().available();
    ASSERT_FALSE(names.empty());
    // The hardcoded help string listed six strategies and omitted this one,
    // which the registry has always been able to construct.
    EXPECT_NE(std::find(names.begin(), names.end(), "ema-rsi-atr-pullback"),
              names.end());
    EXPECT_TRUE(StrategyRegistry::instance().has("ema-rsi-atr-pullback"));
}
