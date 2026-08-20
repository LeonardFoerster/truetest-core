// R1 property and scenario suite (P01-P10, S01-S15).
//
// The generator is seeded explicitly and never touches std::random_device or
// a system clock, so a failure reproduces from the printed case index alone.

#include <gtest/gtest.h>

#include "helpers/mm_test_harness.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <initializer_list>
#include <random>
#include <vector>

using namespace truetest::mm;
using namespace truetest::mm::test;

namespace
{

constexpr std::uint64_t property_seed = 0x5EED12026ULL;
constexpr int property_cases = 400;

struct generated_case
{
    mm_config config;
    market_snapshot market;
    inventory_snapshot inventory;
    strategy_context context;
};

class case_generator
{
public:
    explicit case_generator(std::uint64_t seed) : rng_(seed) {}

    generated_case next()
    {
        generated_case gc;

        instrument_spec spec;
        spec.tick_size = pick({0.01, 0.05, 0.10, 0.50, 1.00});
        spec.lot_size = pick({0.0001, 0.001, 0.01});
        spec.maker_rate = uniform(0.0, 0.0005);
        mm_instrument ins{};
        EXPECT_EQ(make_mm_instrument(spec, ins), instrument_status::ok);

        auto& cfg = gc.config;
        cfg = default_config();
        cfg.levels = static_cast<unsigned>(int_uniform(1, static_cast<int>(max_quote_levels)));
        cfg.fair_value.microprice_weight = uniform(0.0, 1.0);
        cfg.inventory.hard_limit_base =
            atoms_from_decimal("1.0") * int_uniform(1, 20);
        cfg.inventory.soft_limit_ratio = uniform(0.0, 0.6);
        cfg.inventory.reducing_bias_ratio =
            std::min(1.0, cfg.inventory.soft_limit_ratio + uniform(0.05, 0.35));
        cfg.inventory.reservation_skew_bps_at_hard_limit = uniform(0.0, 40.0);
        cfg.inventory.soft_limit_skew_boost = uniform(0.0, 1.5);
        cfg.inventory.size_skew_strength = uniform(0.0, 1.5);
        cfg.inventory.min_size_multiplier = uniform(0.0, 0.4);
        cfg.inventory.max_size_multiplier = uniform(1.2, 3.0);
        cfg.inventory.reducing_size_factor = uniform(0.0, 1.0);
        cfg.spread.min_half_spread_bps = uniform(0.0, 4.0);
        cfg.spread.max_half_spread_bps =
            cfg.spread.min_half_spread_bps + uniform(1.0, 60.0);
        cfg.spread.fee_buffer_bps = uniform(0.0, 3.0);
        cfg.spread.volatility_multiplier = uniform(0.0, 3.0);
        cfg.spread.toxicity_multiplier = uniform(0.0, 3.0);
        cfg.spread.latency_buffer_bps = uniform(0.0, 2.0);
        cfg.spread.latency_multiplier = uniform(0.0, 2.0);
        cfg.quotes.base_size = ins.lot_atoms * int_uniform(1, 5000);
        cfg.quotes.level_spacing_bps = uniform(0.5, 10.0);
        cfg.quotes.post_only = (int_uniform(0, 3) != 0);
        cfg.safety.max_market_data_age_ms = int_uniform(10, 2000);

        // Keep the generator inside the validated space rather than papering
        // over rejects: an invalid draw here is a generator bug.
        const auto status = validate(cfg);
        EXPECT_TRUE(status.ok) << status.message;

        auto& m = gc.market;
        m = default_market();
        const std::int64_t ticks = int_uniform(1000, 10'000'000);
        m.best_bid = Price(ticks * ins.tick_raw);
        m.best_ask = Price((ticks + int_uniform(1, 40)) * ins.tick_raw);
        m.best_bid_qty = ins.lot_atoms * int_uniform(1, 100000);
        m.best_ask_qty = ins.lot_atoms * int_uniform(1, 100000);
        m.short_horizon_volatility_bps = uniform(0.0, 25.0);
        m.toxicity_risk_bps = uniform(0.0, 15.0);
        m.latency_risk_bps = uniform(0.0, 10.0);
        m.short_flow_signal = uniform(-1.0, 1.0);
        m.sequence_valid = true;
        m.snapshot_id = static_cast<std::uint64_t>(int_uniform(1, 1'000'000));

        auto& inv = gc.inventory;
        inv = flat_inventory();
        const auto hard = cfg.inventory.hard_limit_base;
        inv.signed_base_position = static_cast<qty_atoms>(
            std::llround(uniform(-1.4, 1.4) * static_cast<double>(hard)));
        inv.worst_case_position_if_all_buys_fill = inv.signed_base_position
            + static_cast<qty_atoms>(std::llround(uniform(0.0, 0.5)
                                                  * static_cast<double>(hard)));
        inv.worst_case_position_if_all_sells_fill = inv.signed_base_position
            - static_cast<qty_atoms>(std::llround(uniform(0.0, 0.5)
                                                  * static_cast<double>(hard)));
        inv.authoritative = true;

        gc.context = default_context();
        gc.context.instrument = ins;
        gc.context.decision_time_ns = m.receive_time_ns
            + int_uniform(0, static_cast<int>(cfg.safety.max_market_data_age_ms))
                * 1'000'000LL;
        return gc;
    }

private:
    double uniform(double lo, double hi)
    {
        return std::uniform_real_distribution<double>(lo, hi)(rng_);
    }
    int int_uniform(int lo, int hi)
    {
        return std::uniform_int_distribution<int>(lo, hi)(rng_);
    }
    double pick(std::initializer_list<double> values)
    {
        const auto idx = static_cast<std::size_t>(
            int_uniform(0, static_cast<int>(values.size()) - 1));
        return *(values.begin() + static_cast<std::ptrdiff_t>(idx));
    }

    std::mt19937_64 rng_;
};

// Shared invariant battery applied to every generated decision.
void check_universal_invariants(const generated_case& gc, const quote_decision& d,
                                int index)
{
    const auto& ins = gc.context.instrument;

    std::int64_t highest_bid = std::numeric_limits<std::int64_t>::min();
    std::int64_t lowest_ask = std::numeric_limits<std::int64_t>::max();

    for (std::size_t i = 0; i < d.intents.size(); ++i)
    {
        const auto& q = d.intents[i];

        // P02 / P10
        ASSERT_GT(q.quantity, 0) << "case " << index;
        ASSERT_TRUE(is_lot_valid(q.quantity, ins.lot_atoms)) << "case " << index;
        ASSERT_TRUE(is_tick_valid(q.price, ins.tick_raw)) << "case " << index;
        ASSERT_GT(q.price.raw(), 0) << "case " << index;
        ASSERT_LT(q.level, gc.config.levels) << "case " << index;
        ASSERT_EQ(q.post_only, gc.config.quotes.post_only) << "case " << index;

        if (q.side == order_side::buy)
        {
            highest_bid = std::max(highest_bid, q.price.raw());
            if (gc.config.quotes.post_only)
                ASSERT_LT(q.price.raw(), gc.market.best_ask.raw()) << "case " << index;
        }
        else
        {
            lowest_ask = std::min(lowest_ask, q.price.raw());
            if (gc.config.quotes.post_only)
                ASSERT_GT(q.price.raw(), gc.market.best_bid.raw()) << "case " << index;
        }
    }

    // P01
    if (highest_bid != std::numeric_limits<std::int64_t>::min()
        && lowest_ask != std::numeric_limits<std::int64_t>::max())
        ASSERT_LT(highest_bid, lowest_ask) << "case " << index;

    // P03: the worst case after every emitted intent fills must stay inside
    // the hard limit on both sides.
    const qty_atoms hard = gc.config.inventory.hard_limit_base;
    const qty_atoms worst_long = std::max(gc.inventory.signed_base_position,
                                          gc.inventory.worst_case_position_if_all_buys_fill);
    const qty_atoms worst_short = std::min(gc.inventory.signed_base_position,
                                           gc.inventory.worst_case_position_if_all_sells_fill);
    const qty_atoms bid_total = total_side_qty(d, order_side::buy);
    const qty_atoms ask_total = total_side_qty(d, order_side::sell);

    if (worst_long >= hard)
        ASSERT_EQ(bid_total, 0) << "case " << index;
    else
        ASSERT_LE(worst_long + bid_total, hard) << "case " << index;

    if (worst_short <= -hard)
        ASSERT_EQ(ask_total, 0) << "case " << index;
    else
        ASSERT_GE(worst_short - ask_total, -hard) << "case " << index;

    // Ladder monotonicity per side.
    for (unsigned level = 1; level < gc.config.levels; ++level)
    {
        const auto* prev_bid = find_intent(d, order_side::buy, level - 1);
        const auto* bid = find_intent(d, order_side::buy, level);
        if (prev_bid != nullptr && bid != nullptr)
            ASSERT_LT(bid->price.raw(), prev_bid->price.raw()) << "case " << index;

        const auto* prev_ask = find_intent(d, order_side::sell, level - 1);
        const auto* ask = find_intent(d, order_side::sell, level);
        if (prev_ask != nullptr && ask != nullptr)
            ASSERT_GT(ask->price.raw(), prev_ask->price.raw()) << "case " << index;
    }

    if (d.state == mm_state::paused)
    {
        ASSERT_TRUE(d.intents.empty()) << "case " << index;
        ASSERT_TRUE(d.cancel_resting_quotes) << "case " << index;
    }
    ASSERT_FALSE(d.reasons.empty()) << "case " << index;
}

} // namespace

TEST(MMStrategyProperty, P01ToP03AndP10_UniversalInvariants)
{
    case_generator gen(property_seed);
    for (int i = 0; i < property_cases; ++i)
    {
        const auto gc = gen.next();
        auto strat = make_strategy(gc.config);
        const auto d = strat.evaluate(gc.market, gc.inventory, gc.context).decision;
        check_universal_invariants(gc, d, i);
    }
}

TEST(MMStrategyProperty, P04AndP05_InventoryNeverGrowsTheIncreasingSide)
{
    case_generator gen(property_seed + 1);
    for (int i = 0; i < property_cases; ++i)
    {
        auto gc = gen.next();
        auto strat = make_strategy(gc.config);

        // Same everything except the position, and with enough headroom that
        // the hard-limit cap is not what does the shrinking.
        auto neutral_inv = flat_inventory();
        const auto neutral = strat.evaluate(gc.market, neutral_inv, gc.context).decision;

        const auto hard = gc.config.inventory.hard_limit_base;
        const auto long_inv = inventory_at(hard / 4);
        const auto short_inv = inventory_at(-hard / 4);

        const auto lng = strat.evaluate(gc.market, long_inv, gc.context).decision;
        const auto shrt = strat.evaluate(gc.market, short_inv, gc.context).decision;

        ASSERT_LE(lng.bid_size, neutral.bid_size) << "case " << i;
        ASSERT_GE(lng.ask_size, neutral.ask_size) << "case " << i;
        ASSERT_LE(shrt.ask_size, neutral.ask_size) << "case " << i;
        ASSERT_GE(shrt.bid_size, neutral.bid_size) << "case " << i;

        // The reservation price must move the same way.
        ASSERT_LE(lng.reservation_price.raw(), neutral.reservation_price.raw()) << "case " << i;
        ASSERT_GE(shrt.reservation_price.raw(), neutral.reservation_price.raw()) << "case " << i;
    }
}

TEST(MMStrategyProperty, P06AndP07_RiskInputsNeverNarrowTheSpread)
{
    case_generator gen(property_seed + 2);
    for (int i = 0; i < property_cases; ++i)
    {
        const auto gc = gen.next();
        auto strat = make_strategy(gc.config);

        auto base = gc.market;
        const auto base_d = strat.evaluate(base, gc.inventory, gc.context).decision;

        auto toxic = base;
        toxic.toxicity_risk_bps = base.toxicity_risk_bps + 7.5;
        const auto toxic_d = strat.evaluate(toxic, gc.inventory, gc.context).decision;

        auto volatile_market = base;
        volatile_market.short_horizon_volatility_bps = base.short_horizon_volatility_bps + 7.5;
        const auto vol_d = strat.evaluate(volatile_market, gc.inventory, gc.context).decision;

        ASSERT_GE(toxic_d.target_half_spread_bps, base_d.target_half_spread_bps) << "case " << i;
        ASSERT_GE(vol_d.target_half_spread_bps, base_d.target_half_spread_bps) << "case " << i;
    }
}

TEST(MMStrategyProperty, P08_IdenticalInputsProduceIdenticalOutput)
{
    case_generator gen(property_seed + 3);
    for (int i = 0; i < property_cases; ++i)
    {
        const auto gc = gen.next();
        auto a = make_strategy(gc.config);
        auto b = make_strategy(gc.config);

        const auto da = a.evaluate(gc.market, gc.inventory, gc.context).decision;
        const auto db = b.evaluate(gc.market, gc.inventory, gc.context).decision;
        const auto da_again = a.evaluate(gc.market, gc.inventory, gc.context).decision;

        ASSERT_EQ(decision_hash(da), decision_hash(db)) << "case " << i;
        ASSERT_EQ(decision_hash(da), decision_hash(da_again)) << "case " << i;
        ASSERT_EQ(a.strategy_config_hash(), b.strategy_config_hash()) << "case " << i;
    }
}

TEST(MMStrategyProperty, P09_StaleOrSequenceInvalidInputsEmitNothing)
{
    case_generator gen(property_seed + 4);
    for (int i = 0; i < property_cases; ++i)
    {
        const auto gc = gen.next();
        auto strat = make_strategy(gc.config);

        auto gapped = gc.market;
        gapped.sequence_valid = false;
        const auto gap_d = strat.evaluate(gapped, gc.inventory, gc.context).decision;
        ASSERT_EQ(gap_d.state, mm_state::paused) << "case " << i;
        ASSERT_TRUE(gap_d.intents.empty()) << "case " << i;

        auto late = gc.context;
        late.decision_time_ns = gc.market.receive_time_ns
            + (gc.config.safety.max_market_data_age_ms + 1) * 1'000'000LL;
        const auto stale_d = strat.evaluate(gc.market, gc.inventory, late).decision;
        ASSERT_EQ(stale_d.state, mm_state::paused) << "case " << i;
        ASSERT_TRUE(stale_d.intents.empty()) << "case " << i;

        auto unknown = gc.inventory;
        unknown.authoritative = false;
        const auto unknown_d = strat.evaluate(gc.market, unknown, gc.context).decision;
        ASSERT_EQ(unknown_d.state, mm_state::paused) << "case " << i;
        ASSERT_TRUE(unknown_d.intents.empty()) << "case " << i;
    }
}

// ── S01-S15 deterministic scenarios ─────────────────────────────────────────

namespace
{

struct scenario_result
{
    quote_decision decision;
    market_snapshot market;
};

scenario_result run_scenario(const mm_config& cfg, const market_snapshot& market,
                             const inventory_snapshot& inv,
                             const strategy_context& ctx)
{
    auto strat = make_strategy(cfg);
    return {strat.evaluate(market, inv, ctx).decision, market};
}

} // namespace

TEST(MMStrategyScenario, S01_CalmBalancedMarket)
{
    auto market = default_market();
    market.best_bid_qty = atoms_from_decimal("3.00");
    market.best_ask_qty = atoms_from_decimal("3.00");
    market.short_horizon_volatility_bps = 0.1;

    const auto r = run_scenario(default_config(), market, flat_inventory(), default_context());
    EXPECT_EQ(r.decision.state, mm_state::active);
    EXPECT_EQ(r.decision.reservation_price.raw(), r.decision.fair_value.raw());
    EXPECT_EQ(count_side(r.decision, order_side::buy), 1u);
    EXPECT_EQ(count_side(r.decision, order_side::sell), 1u);
}

TEST(MMStrategyScenario, S02_HighVolatilityWidensSpread)
{
    auto calm = default_market();
    calm.short_horizon_volatility_bps = 0.2;
    auto stormy = default_market();
    stormy.short_horizon_volatility_bps = 18.0;

    const auto a = run_scenario(default_config(), calm, flat_inventory(), default_context());
    const auto b = run_scenario(default_config(), stormy, flat_inventory(), default_context());
    EXPECT_GT(b.decision.target_half_spread_bps, a.decision.target_half_spread_bps);
}

TEST(MMStrategyScenario, S03_StrongBidImbalanceLiftsFairValue)
{
    auto market = default_market();
    market.best_bid_qty = atoms_from_decimal("40.00");
    market.best_ask_qty = atoms_from_decimal("0.50");

    const auto balanced = run_scenario(default_config(), default_market(), flat_inventory(),
                                       default_context());
    const auto r = run_scenario(default_config(), market, flat_inventory(), default_context());
    EXPECT_GT(r.decision.fair_value.raw(), balanced.decision.fair_value.raw());
}

TEST(MMStrategyScenario, S04_StrongAskImbalanceDepressesFairValue)
{
    auto market = default_market();
    market.best_bid_qty = atoms_from_decimal("0.50");
    market.best_ask_qty = atoms_from_decimal("40.00");

    const auto balanced = run_scenario(default_config(), default_market(), flat_inventory(),
                                       default_context());
    const auto r = run_scenario(default_config(), market, flat_inventory(), default_context());
    EXPECT_LT(r.decision.fair_value.raw(), balanced.decision.fair_value.raw());
}

TEST(MMStrategyScenario, S05_SuddenSpreadWideningKeepsQuotesInsideTheBook)
{
    auto market = default_market();
    market.best_bid = price_from_decimal("59900.00");
    market.best_ask = price_from_decimal("60100.00");

    const auto r = run_scenario(default_config(), market, flat_inventory(), default_context());
    EXPECT_EQ(r.decision.state, mm_state::active);
    const auto* bid = find_intent(r.decision, order_side::buy, 0);
    const auto* ask = find_intent(r.decision, order_side::sell, 0);
    ASSERT_NE(bid, nullptr);
    ASSERT_NE(ask, nullptr);
    EXPECT_GT(bid->price.raw(), market.best_bid.raw());
    EXPECT_LT(ask->price.raw(), market.best_ask.raw());
}

TEST(MMStrategyScenario, S06_PriceJumpMovesQuotesWithTheBook)
{
    auto jumped = default_market();
    jumped.best_bid = price_from_decimal("61000.00");
    jumped.best_ask = price_from_decimal("61000.50");

    const auto before = run_scenario(default_config(), default_market(), flat_inventory(),
                                     default_context());
    const auto after = run_scenario(default_config(), jumped, flat_inventory(), default_context());

    const auto* bid_before = find_intent(before.decision, order_side::buy, 0);
    const auto* bid_after = find_intent(after.decision, order_side::buy, 0);
    ASSERT_NE(bid_before, nullptr);
    ASSERT_NE(bid_after, nullptr);
    EXPECT_GT(bid_after->price.raw(), bid_before->price.raw());
}

TEST(MMStrategyScenario, S07_LiquidityVacuumWithNoDisplayedSizePauses)
{
    auto market = default_market();
    market.best_bid_qty = 0;
    market.best_ask_qty = 0;

    const auto r = run_scenario(default_config(), market, flat_inventory(), default_context());
    EXPECT_EQ(r.decision.state, mm_state::paused);
    EXPECT_TRUE(r.decision.intents.empty());
    EXPECT_TRUE(has_reason(r.decision, quote_reason::invalid_market_state));
}

TEST(MMStrategyScenario, S07b_OneSidedVacuumStillQuotesButSkewed)
{
    auto market = default_market();
    market.best_bid_qty = atoms_from_decimal("0.0001");
    market.best_ask_qty = atoms_from_decimal("50.00");

    const auto r = run_scenario(default_config(), market, flat_inventory(), default_context());
    EXPECT_EQ(r.decision.state, mm_state::active);
    // Almost no bid size: the microprice sits at the bid.
    EXPECT_LT(r.decision.fair_value.raw(),
              (market.best_bid.raw() + market.best_ask.raw()) / 2);
}

TEST(MMStrategyScenario, S08_StaleFeedPauses)
{
    const auto cfg = default_config();
    auto ctx = default_context();
    const auto market = default_market();
    ctx.decision_time_ns = market.receive_time_ns + 5'000'000'000LL;

    const auto r = run_scenario(cfg, market, flat_inventory(), ctx);
    EXPECT_EQ(r.decision.state, mm_state::paused);
    EXPECT_TRUE(has_reason(r.decision, quote_reason::stale_market_data));
}

TEST(MMStrategyScenario, S09_SequenceGapPausesUntilReconciled)
{
    auto gapped = default_market();
    gapped.sequence_valid = false;

    const auto during = run_scenario(default_config(), gapped, flat_inventory(),
                                     default_context());
    EXPECT_EQ(during.decision.state, mm_state::paused);
    EXPECT_TRUE(during.decision.intents.empty());

    auto reconciled = gapped;
    reconciled.sequence_valid = true;
    reconciled.snapshot_id = gapped.snapshot_id + 1;
    const auto after = run_scenario(default_config(), reconciled, flat_inventory(),
                                    default_context());
    EXPECT_EQ(after.decision.state, mm_state::active);
    EXPECT_FALSE(after.decision.intents.empty());
}

TEST(MMStrategyScenario, S10_NearLongHardLimit)
{
    const auto cfg = default_config();
    const auto inv = inventory_at(static_cast<qty_atoms>(
        static_cast<double>(cfg.inventory.hard_limit_base) * 0.97));

    const auto r = run_scenario(cfg, default_market(), inv, default_context());
    EXPECT_EQ(r.decision.state, mm_state::active);
    EXPECT_TRUE(has_reason(r.decision, quote_reason::inventory_reducing_bias));
    EXPECT_LT(r.decision.bid_size, r.decision.ask_size);
    EXPECT_LT(r.decision.reservation_price.raw(), r.decision.fair_value.raw());
}

TEST(MMStrategyScenario, S11_NearShortHardLimit)
{
    const auto cfg = default_config();
    const auto inv = inventory_at(-static_cast<qty_atoms>(
        static_cast<double>(cfg.inventory.hard_limit_base) * 0.97));

    const auto r = run_scenario(cfg, default_market(), inv, default_context());
    EXPECT_EQ(r.decision.state, mm_state::active);
    EXPECT_TRUE(has_reason(r.decision, quote_reason::inventory_reducing_bias));
    EXPECT_LT(r.decision.ask_size, r.decision.bid_size);
    EXPECT_GT(r.decision.reservation_price.raw(), r.decision.fair_value.raw());
}

TEST(MMStrategyScenario, S12_RepeatedAdverseFillsDriveTowardReducingOnly)
{
    const auto cfg = default_config();
    auto strat = make_strategy(cfg);
    const auto market = default_market();
    const auto ctx = default_context();

    // Each pass fills the whole bid; the position walks toward the limit and
    // the bid must shrink monotonically on the way.
    qty_atoms position = 0;
    qty_atoms previous_bid = std::numeric_limits<qty_atoms>::max();
    mm_state last_state = mm_state::active;

    for (int i = 0; i < 60; ++i)
    {
        const auto d = strat.evaluate(market, inventory_at(position), ctx).decision;
        last_state = d.state;
        EXPECT_LE(d.bid_size, previous_bid) << "iteration " << i;
        previous_bid = d.bid_size;

        const auto* bid = find_intent(d, order_side::buy, 0);
        if (bid == nullptr)
            break;
        position += bid->quantity;
    }

    EXPECT_EQ(last_state, mm_state::reducing_only);
}

TEST(MMStrategyScenario, S13_LatencySpikeWidensSpread)
{
    auto quiet = default_market();
    quiet.latency_risk_bps = 0.05;
    auto spike = default_market();
    spike.latency_risk_bps = 12.0;

    const auto a = run_scenario(default_config(), quiet, flat_inventory(), default_context());
    const auto b = run_scenario(default_config(), spike, flat_inventory(), default_context());
    EXPECT_GT(b.decision.target_half_spread_bps, a.decision.target_half_spread_bps);
}

TEST(MMStrategyScenario, S14_FeeIncreaseWidensSpread)
{
    auto cheap = default_config();
    cheap.spread.fee_buffer_bps = 0.5;
    auto pricey = default_config();
    pricey.spread.fee_buffer_bps = 9.0;

    const auto a = run_scenario(cheap, default_market(), flat_inventory(), default_context());
    const auto b = run_scenario(pricey, default_market(), flat_inventory(), default_context());
    EXPECT_GT(b.decision.target_half_spread_bps, a.decision.target_half_spread_bps);
}

TEST(MMStrategyScenario, S15_BurstOfL2EventsStaysInvariantSafe)
{
    const auto cfg = default_config();
    auto strat = make_strategy(cfg);
    auto ctx = default_context();

    std::mt19937_64 rng(property_seed + 15);
    std::uniform_int_distribution<int> tick_walk(-20, 20);
    std::int64_t bid_ticks = 600000;

    for (int i = 0; i < 5000; ++i)
    {
        bid_ticks = std::max<std::int64_t>(1000, bid_ticks + tick_walk(rng));
        auto market = default_market();
        market.best_bid = Price(bid_ticks * ctx.instrument.tick_raw);
        market.best_ask = Price((bid_ticks + 5) * ctx.instrument.tick_raw);
        market.snapshot_id = static_cast<std::uint64_t>(i + 1);
        market.event_time_ns = base_event_time_ns + i * 1'000'000LL;
        market.receive_time_ns = market.event_time_ns + 100'000;
        ctx.decision_time_ns = market.receive_time_ns + 50'000;

        const auto d = strat.evaluate(market, inventory_at(0), ctx).decision;
        ASSERT_EQ(d.state, mm_state::active) << "burst step " << i;
        for (std::size_t k = 0; k < d.intents.size(); ++k)
        {
            ASSERT_GT(d.intents[k].quantity, 0);
            ASSERT_TRUE(is_tick_valid(d.intents[k].price, ctx.instrument.tick_raw));
        }
    }
}
