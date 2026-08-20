// R1 unit suite for the inventory-aware market-making strategy (T01-T25).
//
// Every expectation is derived from the reference configuration in
// tests/helpers/mm_test_harness.h, never from a previously observed output.

#include <gtest/gtest.h>

#include "helpers/mm_test_harness.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <vector>

using namespace truetest::mm;
using namespace truetest::mm::test;

namespace
{

// mid/microprice/fair for the reference book, computed here independently of
// the strategy so the price tests assert against arithmetic, not memory.
struct book_math
{
    double mid;
    double microprice;
    double fair;
};

book_math compute_book_math(const market_snapshot& m, const mm_config& cfg)
{
    const double bid = static_cast<double>(m.best_bid.raw());
    const double ask = static_cast<double>(m.best_ask.raw());
    const double bq = static_cast<double>(m.best_bid_qty);
    const double aq = static_cast<double>(m.best_ask_qty);
    const double mid = 0.5 * (bid + ask);
    const double micro = (ask * bq + bid * aq) / (bq + aq);
    const double fair = mid + cfg.fair_value.microprice_weight * (micro - mid);
    return {mid, micro, fair};
}

} // namespace

TEST(MMStrategyUnit, ConfigHashUsesCanonicalLittleEndianBytes)
{
    constexpr std::array<std::uint8_t, 8> expected{
        0x08U, 0x07U, 0x06U, 0x05U, 0x04U, 0x03U, 0x02U, 0x01U};
    EXPECT_EQ(canonical_u64_le(0x0102030405060708ULL), expected);
    EXPECT_EQ(config_hash_schema_version, 1U);
}

// ── T01 neutral inventory ───────────────────────────────────────────────────
TEST(MMStrategyUnit, T01_NeutralInventory_SymmetricAroundFair)
{
    const auto cfg = default_config();
    auto strat = make_strategy(cfg);
    const auto market = default_market();
    const auto ctx = default_context();

    const auto res = strat.evaluate(market, flat_inventory(), ctx);
    ASSERT_TRUE(res.ok());
    const auto& d = res.decision;

    EXPECT_EQ(d.state, mm_state::active);
    EXPECT_DOUBLE_EQ(d.inventory_utilization, 0.0);
    EXPECT_EQ(d.reservation_price.raw(), d.fair_value.raw());

    const auto bm = compute_book_math(market, cfg);
    EXPECT_EQ(d.fair_value.raw(), static_cast<std::int64_t>(std::llround(bm.fair)));

    // Neutral inventory quotes the configured base size on both sides.
    EXPECT_EQ(d.bid_size, cfg.quotes.base_size);
    EXPECT_EQ(d.ask_size, cfg.quotes.base_size);

    const auto* bid = find_intent(d, order_side::buy, 0);
    const auto* ask = find_intent(d, order_side::sell, 0);
    ASSERT_NE(bid, nullptr);
    ASSERT_NE(ask, nullptr);

    // Maker-safe rounding is floor/ceil, so the distances differ by at most a
    // tick; the un-rounded targets are exactly symmetric about the reservation.
    const double reservation = static_cast<double>(d.reservation_price.raw());
    const double hs = d.target_half_spread_bps / 10000.0;
    EXPECT_EQ(bid->price.raw(), tick_floor(
        static_cast<std::int64_t>(std::floor(reservation * (1.0 - hs))), ctx.instrument.tick_raw));
    EXPECT_LE(std::fabs((reservation - static_cast<double>(bid->price.raw()))
                        - (static_cast<double>(ask->price.raw()) - reservation)),
              static_cast<double>(ctx.instrument.tick_raw));
    EXPECT_TRUE(has_reason(d, quote_reason::normal));
}

// ── T02 long inventory ──────────────────────────────────────────────────────
TEST(MMStrategyUnit, T02_LongInventory_SkewsDownAndShrinksBid)
{
    const auto cfg = default_config();
    auto strat = make_strategy(cfg);
    const auto market = default_market();
    const auto ctx = default_context();

    const auto neutral = strat.evaluate(market, flat_inventory(), ctx).decision;
    const auto lng = strat.evaluate(market, inventory_at(atoms_from_decimal("0.30")), ctx).decision;

    EXPECT_NEAR(lng.inventory_utilization, 0.30, 1e-12);
    EXPECT_LT(lng.reservation_price.raw(), lng.fair_value.raw());
    EXPECT_EQ(lng.fair_value.raw(), neutral.fair_value.raw());

    EXPECT_LT(lng.bid_size, neutral.bid_size);
    EXPECT_GT(lng.ask_size, neutral.ask_size);

    // base 0.10, size_skew_strength 0.5, u = 0.3
    //   bid multiplier 1 - 0.5 * 0.3 = 0.85 -> 0.085
    //   ask multiplier 1 + 0.5 * 0.3 = 1.15 -> 0.115
    EXPECT_EQ(lng.bid_size, atoms_from_decimal("0.085"));
    EXPECT_EQ(lng.ask_size, atoms_from_decimal("0.115"));
}

// ── T03 short inventory ─────────────────────────────────────────────────────
TEST(MMStrategyUnit, T03_ShortInventory_SkewsUpAndShrinksAsk)
{
    const auto cfg = default_config();
    auto strat = make_strategy(cfg);
    const auto market = default_market();
    const auto ctx = default_context();

    const auto neutral = strat.evaluate(market, flat_inventory(), ctx).decision;
    const auto shrt = strat.evaluate(market, inventory_at(-atoms_from_decimal("0.30")), ctx).decision;

    EXPECT_NEAR(shrt.inventory_utilization, -0.30, 1e-12);
    EXPECT_GT(shrt.reservation_price.raw(), shrt.fair_value.raw());
    EXPECT_LT(shrt.ask_size, neutral.ask_size);
    EXPECT_GT(shrt.bid_size, neutral.bid_size);
}

// ── T04 reservation monotonicity in u ───────────────────────────────────────
TEST(MMStrategyUnit, T04_ReservationStrictlyDecreasingInUtilization)
{
    auto cfg = default_config();
    cfg.inventory.soft_limit_skew_boost = 0.35; // exercise the boosted band too
    auto strat = make_strategy(cfg);
    const auto market = default_market();
    const auto ctx = default_context();

    std::int64_t previous = std::numeric_limits<std::int64_t>::max();
    for (int step = -20; step <= 20; ++step)
    {
        const double u = static_cast<double>(step) / 20.0;
        const auto pos = static_cast<qty_atoms>(
            std::llround(u * static_cast<double>(cfg.inventory.hard_limit_base)));
        const auto d = strat.evaluate(market, inventory_at(pos), ctx).decision;
        EXPECT_LT(d.reservation_price.raw(), previous)
            << "reservation must strictly decrease in u; step=" << step;
        previous = d.reservation_price.raw();
    }
}

// ── T05 soft limit ──────────────────────────────────────────────────────────
TEST(MMStrategyUnit, T05_SoftLimitBand_FlagsReasonAndKeepsQuoting)
{
    const auto cfg = default_config();
    auto strat = make_strategy(cfg);
    const auto d = strat.evaluate(default_market(),
                                  inventory_at(atoms_from_decimal("0.60")),
                                  default_context()).decision;

    EXPECT_EQ(d.state, mm_state::active);
    EXPECT_TRUE(has_reason(d, quote_reason::inventory_soft_limit));
    EXPECT_FALSE(has_reason(d, quote_reason::inventory_reducing_bias));
    EXPECT_GE(count_side(d, order_side::buy), 1u);
    EXPECT_GE(count_side(d, order_side::sell), 1u);
}

// ── T06 reducing band ───────────────────────────────────────────────────────
TEST(MMStrategyUnit, T06_ReducingBand_CutsInventoryIncreasingSide)
{
    const auto cfg = default_config();
    auto strat = make_strategy(cfg);
    const auto market = default_market();
    const auto ctx = default_context();

    const auto neutral = strat.evaluate(market, flat_inventory(), ctx).decision;
    const auto band = strat.evaluate(market, inventory_at(atoms_from_decimal("0.85")), ctx).decision;

    EXPECT_TRUE(has_reason(band, quote_reason::inventory_reducing_bias));
    EXPECT_EQ(band.state, mm_state::active);

    // Skew alone would give 0.575 * base; the reducing factor cuts it to a
    // quarter of that, so well under half the neutral size.
    EXPECT_LT(band.bid_size, neutral.bid_size / 2);
    EXPECT_GT(band.ask_size, neutral.ask_size);
    EXPECT_GT(band.bid_size, 0);
}

// ── T07 long hard limit ─────────────────────────────────────────────────────
TEST(MMStrategyUnit, T07_LongHardLimit_NoBuyIntents)
{
    const auto cfg = default_config();
    auto strat = make_strategy(cfg);
    const auto d = strat.evaluate(default_market(),
                                  inventory_at(cfg.inventory.hard_limit_base),
                                  default_context()).decision;

    EXPECT_EQ(d.state, mm_state::reducing_only);
    EXPECT_TRUE(has_reason(d, quote_reason::inventory_hard_limit));
    EXPECT_EQ(count_side(d, order_side::buy), 0u);
    EXPECT_GE(count_side(d, order_side::sell), 1u);
}

// ── T08 short hard limit ────────────────────────────────────────────────────
TEST(MMStrategyUnit, T08_ShortHardLimit_NoSellIntents)
{
    const auto cfg = default_config();
    auto strat = make_strategy(cfg);
    const auto d = strat.evaluate(default_market(),
                                  inventory_at(-cfg.inventory.hard_limit_base),
                                  default_context()).decision;

    EXPECT_EQ(d.state, mm_state::reducing_only);
    EXPECT_TRUE(has_reason(d, quote_reason::inventory_hard_limit));
    EXPECT_EQ(count_side(d, order_side::sell), 0u);
    EXPECT_GE(count_side(d, order_side::buy), 1u);
}

// ── T09 stale market data ───────────────────────────────────────────────────
TEST(MMStrategyUnit, T09_StaleMarketData_PausesAndEmitsNothing)
{
    const auto cfg = default_config();
    auto strat = make_strategy(cfg);
    const auto market = default_market();
    auto ctx = default_context();
    ctx.decision_time_ns = market.receive_time_ns
        + (cfg.safety.max_market_data_age_ms + 1) * 1'000'000LL;

    const auto res = strat.evaluate(market, flat_inventory(), ctx);
    ASSERT_TRUE(res.ok());
    EXPECT_EQ(res.decision.state, mm_state::paused);
    EXPECT_TRUE(res.decision.intents.empty());
    EXPECT_TRUE(res.decision.cancel_resting_quotes);
    EXPECT_TRUE(has_reason(res.decision, quote_reason::stale_market_data));
}

TEST(MMStrategyUnit, T09b_AgeExactlyAtLimitStillQuotes)
{
    const auto cfg = default_config();
    auto strat = make_strategy(cfg);
    const auto market = default_market();
    auto ctx = default_context();
    ctx.decision_time_ns = market.receive_time_ns
        + cfg.safety.max_market_data_age_ms * 1'000'000LL;

    const auto d = strat.evaluate(market, flat_inventory(), ctx).decision;
    EXPECT_EQ(d.state, mm_state::active);
    EXPECT_FALSE(d.intents.empty());
}

TEST(MMStrategyUnit, ExtremeOrderedMarketTimestampsSaturateAndFailClosed)
{
    auto market = default_market();
    market.event_time_ns = std::numeric_limits<timestamp_ns>::min();
    market.receive_time_ns = std::numeric_limits<timestamp_ns>::min();
    const auto ctx = default_context(std::numeric_limits<timestamp_ns>::max());

    const auto d = make_strategy(default_config()).evaluate(market, flat_inventory(), ctx).decision;
    EXPECT_EQ(d.market_age_ns, std::numeric_limits<timestamp_ns>::max());
    EXPECT_EQ(d.state, mm_state::paused);
    EXPECT_TRUE(has_reason(d, quote_reason::stale_market_data));
}

// ── T10 sequence gap ────────────────────────────────────────────────────────
TEST(MMStrategyUnit, T10_SequenceGap_PausesAndEmitsNothing)
{
    const auto cfg = default_config();
    auto strat = make_strategy(cfg);
    auto market = default_market();
    market.sequence_valid = false;

    const auto d = strat.evaluate(market, flat_inventory(), default_context()).decision;
    EXPECT_EQ(d.state, mm_state::paused);
    EXPECT_TRUE(d.intents.empty());
    EXPECT_TRUE(has_reason(d, quote_reason::sequence_gap));
}

// ── T11 unknown inventory ───────────────────────────────────────────────────
TEST(MMStrategyUnit, T11_NonAuthoritativeInventory_PausesWhenRequired)
{
    const auto cfg = default_config();
    auto strat = make_strategy(cfg);
    auto inv = flat_inventory();
    inv.authoritative = false;

    const auto d = strat.evaluate(default_market(), inv, default_context()).decision;
    EXPECT_EQ(d.state, mm_state::paused);
    EXPECT_TRUE(d.intents.empty());
    EXPECT_TRUE(has_reason(d, quote_reason::unknown_inventory));
}

TEST(MMStrategyUnit, T11b_NonAuthoritativeInventoryStillFlaggedWhenNotRequired)
{
    auto cfg = default_config();
    cfg.safety.require_authoritative_inventory = false;
    auto strat = make_strategy(cfg);
    auto inv = flat_inventory();
    inv.authoritative = false;

    const auto d = strat.evaluate(default_market(), inv, default_context()).decision;
    EXPECT_EQ(d.state, mm_state::active);
    EXPECT_TRUE(has_reason(d, quote_reason::unknown_inventory));
}

// ── T12-T14 microprice behaviour ────────────────────────────────────────────
TEST(MMStrategyUnit, T12_BalancedBook_MicropriceEqualsMid)
{
    const auto cfg = default_config();
    auto strat = make_strategy(cfg);
    auto market = default_market();
    market.best_bid_qty = atoms_from_decimal("2.00");
    market.best_ask_qty = atoms_from_decimal("2.00");

    const auto d = strat.evaluate(market, flat_inventory(), default_context()).decision;
    const auto bm = compute_book_math(market, cfg);
    EXPECT_DOUBLE_EQ(bm.microprice, bm.mid);
    EXPECT_EQ(d.fair_value.raw(), static_cast<std::int64_t>(std::llround(bm.mid)));
}

TEST(MMStrategyUnit, T13_BidHeavyBook_FairMovesTowardAsk)
{
    const auto cfg = default_config();
    auto strat = make_strategy(cfg);
    auto market = default_market();
    market.best_bid_qty = atoms_from_decimal("9.00");
    market.best_ask_qty = atoms_from_decimal("1.00");

    const auto d = strat.evaluate(market, flat_inventory(), default_context()).decision;
    const auto bm = compute_book_math(market, cfg);
    EXPECT_GT(bm.microprice, bm.mid);
    EXPECT_GT(static_cast<double>(d.fair_value.raw()), bm.mid);
    EXPECT_LE(d.fair_value.raw(), market.best_ask.raw());
}

TEST(MMStrategyUnit, T14_AskHeavyBook_FairMovesTowardBid)
{
    const auto cfg = default_config();
    auto strat = make_strategy(cfg);
    auto market = default_market();
    market.best_bid_qty = atoms_from_decimal("1.00");
    market.best_ask_qty = atoms_from_decimal("9.00");

    const auto d = strat.evaluate(market, flat_inventory(), default_context()).decision;
    const auto bm = compute_book_math(market, cfg);
    EXPECT_LT(bm.microprice, bm.mid);
    EXPECT_LT(static_cast<double>(d.fair_value.raw()), bm.mid);
    EXPECT_GE(d.fair_value.raw(), market.best_bid.raw());
}

// ── T15/T16 grid conformance ────────────────────────────────────────────────
TEST(MMStrategyUnit, T15_AllQuotePricesAreTickValid)
{
    auto cfg = default_config();
    cfg.levels = 4;
    auto strat = make_strategy(cfg);
    const auto ctx = default_context();

    for (int step = -10; step <= 10; ++step)
    {
        const auto pos = static_cast<qty_atoms>(step) * (cfg.inventory.hard_limit_base / 10);
        const auto d = strat.evaluate(default_market(), inventory_at(pos), ctx).decision;
        for (std::size_t i = 0; i < d.intents.size(); ++i)
            EXPECT_TRUE(is_tick_valid(d.intents[i].price, ctx.instrument.tick_raw))
                << "price " << d.intents[i].price.raw() << " off the tick grid";
    }
}

TEST(MMStrategyUnit, T16_AllQuoteQuantitiesAreLotValid)
{
    auto cfg = default_config();
    cfg.levels = 4;
    // A base size that is not a lot multiple must be floored, never emitted raw.
    cfg.quotes.base_size = atoms_from_decimal("0.10005") + 7;
    auto strat = make_strategy(cfg);
    const auto ctx = default_context();

    for (int step = -10; step <= 10; ++step)
    {
        const auto pos = static_cast<qty_atoms>(step) * (cfg.inventory.hard_limit_base / 10);
        const auto d = strat.evaluate(default_market(), inventory_at(pos), ctx).decision;
        for (std::size_t i = 0; i < d.intents.size(); ++i)
        {
            EXPECT_TRUE(is_lot_valid(d.intents[i].quantity, ctx.instrument.lot_atoms));
            EXPECT_GT(d.intents[i].quantity, 0);
        }
    }
}

// ── T17 post-only ───────────────────────────────────────────────────────────
namespace
{
// Short at the hard limit with a large reservation skew pushes the bid well
// above the touch, so a naive quote would take liquidity.
mm_config crossing_config()
{
    auto cfg = default_config();
    cfg.inventory.reservation_skew_bps_at_hard_limit = 500.0;
    cfg.spread.min_half_spread_bps = 1.0;
    cfg.spread.max_half_spread_bps = 2.0;
    cfg.spread.fee_buffer_bps = 0.0;
    cfg.spread.maker_fee_multiplier = 0.0;
    return cfg;
}
} // namespace

TEST(MMStrategyUnit, T17_PostOnly_SuppressesCrossingQuotes)
{
    auto cfg = crossing_config();
    cfg.quotes.post_only = true;
    auto strat = make_strategy(cfg);
    const auto market = default_market();

    const auto d = strat.evaluate(market, inventory_at(-cfg.inventory.hard_limit_base),
                                  default_context()).decision;

    EXPECT_TRUE(has_reason(d, quote_reason::post_only_cross_prevented));
    for (std::size_t i = 0; i < d.intents.size(); ++i)
    {
        const auto& q = d.intents[i];
        if (q.side == order_side::buy)
            EXPECT_LT(q.price.raw(), market.best_ask.raw());
        else
            EXPECT_GT(q.price.raw(), market.best_bid.raw());
    }
}

TEST(MMStrategyUnit, T17b_WithoutPostOnly_TheCrossingQuoteIsEmitted)
{
    auto cfg = crossing_config();
    cfg.quotes.post_only = false;
    auto strat = make_strategy(cfg);
    const auto market = default_market();

    const auto d = strat.evaluate(market, inventory_at(-cfg.inventory.hard_limit_base),
                                  default_context()).decision;

    const auto* bid = find_intent(d, order_side::buy, 0);
    ASSERT_NE(bid, nullptr);
    EXPECT_GE(bid->price.raw(), market.best_ask.raw());
    EXPECT_FALSE(bid->post_only);
    EXPECT_FALSE(has_reason(d, quote_reason::post_only_cross_prevented));
}

// ── T18 ladder monotonicity ─────────────────────────────────────────────────
TEST(MMStrategyUnit, T18_LadderIsStrictlyMonotoneOnBothSides)
{
    auto cfg = default_config();
    cfg.levels = max_quote_levels;
    auto strat = make_strategy(cfg);

    const auto d = strat.evaluate(default_market(), flat_inventory(), default_context()).decision;
    EXPECT_EQ(count_side(d, order_side::buy), cfg.levels);
    EXPECT_EQ(count_side(d, order_side::sell), cfg.levels);

    for (unsigned level = 1; level < cfg.levels; ++level)
    {
        const auto* prev_bid = find_intent(d, order_side::buy, level - 1);
        const auto* bid = find_intent(d, order_side::buy, level);
        const auto* prev_ask = find_intent(d, order_side::sell, level - 1);
        const auto* ask = find_intent(d, order_side::sell, level);
        ASSERT_NE(prev_bid, nullptr);
        ASSERT_NE(bid, nullptr);
        ASSERT_NE(prev_ask, nullptr);
        ASSERT_NE(ask, nullptr);
        EXPECT_LT(bid->price.raw(), prev_bid->price.raw());
        EXPECT_GT(ask->price.raw(), prev_ask->price.raw());
    }
}

TEST(MMStrategyUnit, T18b_TightSpacingStillProducesDistinctTicks)
{
    auto cfg = default_config();
    cfg.levels = 4;
    // 0.0001 bp on a 60k price is far below one 0.10 tick; the ladder must
    // still separate levels by at least one tick.
    cfg.quotes.level_spacing_bps = 0.0001;
    auto strat = make_strategy(cfg);
    const auto ctx = default_context();

    const auto d = strat.evaluate(default_market(), flat_inventory(), ctx).decision;
    for (unsigned level = 1; level < cfg.levels; ++level)
    {
        const auto* prev_bid = find_intent(d, order_side::buy, level - 1);
        const auto* bid = find_intent(d, order_side::buy, level);
        ASSERT_NE(prev_bid, nullptr);
        ASSERT_NE(bid, nullptr);
        EXPECT_LE(bid->price.raw(), prev_bid->price.raw() - ctx.instrument.tick_raw);
    }
}

// ── T19-T23 spread controller ───────────────────────────────────────────────
namespace
{
strategy_context zero_fee_context()
{
    instrument_spec spec;
    spec.symbol = "BTCUSDT";
    spec.tick_size = 0.10;
    spec.lot_size = 0.0001;
    spec.maker_rate = 0.0;
    mm_instrument ins{};
    EXPECT_EQ(make_mm_instrument(spec, ins), instrument_status::ok);
    auto ctx = default_context();
    ctx.instrument = ins;
    return ctx;
}
} // namespace

TEST(MMStrategyUnit, T19_HalfSpreadNeverBelowConfiguredMinimum)
{
    auto cfg = default_config();
    cfg.spread.fee_buffer_bps = 0.0;
    cfg.spread.min_half_spread_bps = 3.0;
    auto strat = make_strategy(cfg);

    auto market = default_market();
    market.short_horizon_volatility_bps = 0.0;
    market.toxicity_risk_bps = 0.0;
    market.latency_risk_bps = 0.0;

    const auto d = strat.evaluate(market, flat_inventory(), zero_fee_context()).decision;
    EXPECT_DOUBLE_EQ(d.target_half_spread_bps, 3.0);
}

TEST(MMStrategyUnit, T20_HalfSpreadCappedAtConfiguredMaximum)
{
    auto cfg = default_config();
    cfg.spread.max_half_spread_bps = 12.0;
    auto strat = make_strategy(cfg);

    auto market = default_market();
    market.short_horizon_volatility_bps = 900.0;

    const auto d = strat.evaluate(market, flat_inventory(), default_context()).decision;
    EXPECT_DOUBLE_EQ(d.target_half_spread_bps, 12.0);
}

TEST(MMStrategyUnit, T21_RisingFeesNeverNarrowTheHalfSpread)
{
    auto market = default_market();
    market.short_horizon_volatility_bps = 0.0;
    market.toxicity_risk_bps = 0.0;
    market.latency_risk_bps = 0.0;

    double previous = -1.0;
    for (double fee = 0.0; fee <= 40.0; fee += 2.5)
    {
        auto cfg = default_config();
        cfg.spread.fee_buffer_bps = fee;
        auto strat = make_strategy(cfg);
        const auto d = strat.evaluate(market, flat_inventory(), default_context()).decision;
        EXPECT_GE(d.target_half_spread_bps, previous);
        previous = d.target_half_spread_bps;
    }
    EXPECT_GT(previous, default_config().spread.min_half_spread_bps);
}

TEST(MMStrategyUnit, T22_RisingVolatilityNeverNarrowsTheHalfSpread)
{
    const auto cfg = default_config();
    auto strat = make_strategy(cfg);

    double previous = -1.0;
    for (double vol = 0.0; vol <= 40.0; vol += 2.5)
    {
        auto market = default_market();
        market.short_horizon_volatility_bps = vol;
        const auto d = strat.evaluate(market, flat_inventory(), default_context()).decision;
        EXPECT_GE(d.target_half_spread_bps, previous);
        previous = d.target_half_spread_bps;
    }
}

TEST(MMStrategyUnit, T23_RisingToxicityNeverNarrowsTheHalfSpread)
{
    const auto cfg = default_config();
    auto strat = make_strategy(cfg);

    double previous = -1.0;
    for (double tox = 0.0; tox <= 40.0; tox += 2.5)
    {
        auto market = default_market();
        market.toxicity_risk_bps = tox;
        const auto d = strat.evaluate(market, flat_inventory(), default_context()).decision;
        EXPECT_GE(d.target_half_spread_bps, previous);
        previous = d.target_half_spread_bps;
    }
}

TEST(MMStrategyUnit, T23b_LatencyRiskWidensTheHalfSpread)
{
    const auto cfg = default_config();
    auto strat = make_strategy(cfg);

    auto quiet = default_market();
    quiet.latency_risk_bps = 0.0;
    auto spiked = default_market();
    spiked.latency_risk_bps = 6.0;

    const auto a = strat.evaluate(quiet, flat_inventory(), default_context()).decision;
    const auto b = strat.evaluate(spiked, flat_inventory(), default_context()).decision;
    EXPECT_GT(b.target_half_spread_bps, a.target_half_spread_bps);
}

TEST(MMStrategyUnit, T23c_CapBelowFeeFloorEmitsNothingWithInsufficientEdge)
{
    auto cfg = default_config();
    cfg.spread.fee_buffer_bps = 20.0;
    cfg.spread.min_half_spread_bps = 0.0;
    cfg.spread.max_half_spread_bps = 5.0;
    auto strat = make_strategy(cfg);

    const auto d = strat.evaluate(default_market(), flat_inventory(), default_context()).decision;
    EXPECT_TRUE(has_reason(d, quote_reason::insufficient_edge));
    EXPECT_TRUE(d.intents.empty());
}

// ── T24 determinism ─────────────────────────────────────────────────────────
TEST(MMStrategyUnit, T24_TenThousandIdenticalCallsProduceOneHash)
{
    auto cfg = default_config();
    cfg.levels = 4;
    auto strat = make_strategy(cfg);
    const auto market = default_market();
    const auto inv = inventory_at(atoms_from_decimal("0.37"));
    const auto ctx = default_context();

    const auto first = strat.evaluate(market, inv, ctx).decision;
    const std::uint64_t expected = decision_hash(first);

    for (int i = 0; i < 10000; ++i)
    {
        const auto d = strat.evaluate(market, inv, ctx).decision;
        ASSERT_EQ(decision_hash(d), expected) << "divergence at iteration " << i;
    }

    // A second, independently configured instance must agree as well.
    auto other = make_strategy(cfg);
    EXPECT_EQ(decision_hash(other.evaluate(market, inv, ctx).decision), expected);
    EXPECT_EQ(other.strategy_config_hash(), strat.strategy_config_hash());
}

// ── T25 config validation ───────────────────────────────────────────────────
TEST(MMStrategyUnit, T25_ValidConfigIsAccepted)
{
    EXPECT_TRUE(validate(default_config()).ok);
}

TEST(MMStrategyUnit, T25_InvalidConfigsAreRejectedAtStartup)
{
    struct rejection_case
    {
        const char* label;
        std::function<void(mm_config&)> mutate;
    };

    const double nan_v = std::numeric_limits<double>::quiet_NaN();
    const double inf_v = std::numeric_limits<double>::infinity();

    const std::vector<rejection_case> cases = {
        {"empty strategy_id", [](mm_config& c) { c.strategy_id.clear(); }},
        {"levels = 0", [](mm_config& c) { c.levels = 0; }},
        {"levels above compile-time max",
         [](mm_config& c) { c.levels = max_quote_levels + 1; }},
        {"microprice_weight NaN", [&](mm_config& c) { c.fair_value.microprice_weight = nan_v; }},
        {"microprice_weight > 1", [](mm_config& c) { c.fair_value.microprice_weight = 1.5; }},
        {"microprice_weight < 0", [](mm_config& c) { c.fair_value.microprice_weight = -0.1; }},
        {"imbalance_weight Inf", [&](mm_config& c) { c.fair_value.imbalance_weight = inf_v; }},
        {"short_flow_weight NaN", [&](mm_config& c) { c.fair_value.short_flow_weight = nan_v; }},
        {"hard_limit_base = 0", [](mm_config& c) { c.inventory.hard_limit_base = 0; }},
        {"hard_limit_base negative", [](mm_config& c) { c.inventory.hard_limit_base = -1; }},
        {"hard_limit_base exceeds safe arithmetic bound",
         [](mm_config& c) { c.inventory.hard_limit_base = std::numeric_limits<qty_atoms>::max(); }},
        {"soft >= reducing", [](mm_config& c) { c.inventory.soft_limit_ratio = 0.9; }},
        {"soft == reducing", [](mm_config& c) { c.inventory.reducing_bias_ratio = 0.5; }},
        {"reducing > 1", [](mm_config& c) { c.inventory.reducing_bias_ratio = 1.2; }},
        {"soft negative", [](mm_config& c) { c.inventory.soft_limit_ratio = -0.1; }},
        {"skew negative",
         [](mm_config& c) { c.inventory.reservation_skew_bps_at_hard_limit = -1.0; }},
        {"skew drives reservation non-positive",
         [](mm_config& c) { c.inventory.reservation_skew_bps_at_hard_limit = 10000.0; }},
        {"skew boost pushes past 10000 bps",
         [](mm_config& c) {
             c.inventory.reservation_skew_bps_at_hard_limit = 6000.0;
             c.inventory.soft_limit_skew_boost = 1.0;
         }},
        {"size_skew_strength NaN", [&](mm_config& c) { c.inventory.size_skew_strength = nan_v; }},
        {"min_size_multiplier negative",
         [](mm_config& c) { c.inventory.min_size_multiplier = -0.5; }},
        {"max < min size multiplier",
         [](mm_config& c) { c.inventory.max_size_multiplier = 0.0; c.inventory.min_size_multiplier = 1.0; }},
        {"max_size_multiplier absurd",
         [](mm_config& c) { c.inventory.max_size_multiplier = 1.0e9; }},
        {"reducing_size_factor > 1",
         [](mm_config& c) { c.inventory.reducing_size_factor = 1.5; }},
        {"min_half_spread negative", [](mm_config& c) { c.spread.min_half_spread_bps = -1.0; }},
        {"max < min half spread", [](mm_config& c) { c.spread.max_half_spread_bps = 0.5; }},
        {"fee_buffer negative", [](mm_config& c) { c.spread.fee_buffer_bps = -0.1; }},
        {"volatility_multiplier Inf", [&](mm_config& c) { c.spread.volatility_multiplier = inf_v; }},
        {"toxicity_multiplier negative", [](mm_config& c) { c.spread.toxicity_multiplier = -1.0; }},
        {"latency_buffer NaN", [&](mm_config& c) { c.spread.latency_buffer_bps = nan_v; }},
        {"latency_multiplier negative", [](mm_config& c) { c.spread.latency_multiplier = -2.0; }},
        {"base_size = 0", [](mm_config& c) { c.quotes.base_size = 0; }},
        {"base_size negative", [](mm_config& c) { c.quotes.base_size = -5; }},
        {"base_size exceeds safe arithmetic bound",
         [](mm_config& c) { c.quotes.base_size = std::numeric_limits<qty_atoms>::max(); }},
        {"level_spacing negative", [](mm_config& c) { c.quotes.level_spacing_bps = -1.0; }},
        {"zero spacing with multiple levels",
         [](mm_config& c) { c.levels = 3; c.quotes.level_spacing_bps = 0.0; }},
        {"negative quote lifetime",
         [](mm_config& c) { c.quotes.minimum_quote_lifetime_ms = -1; }},
        {"max_market_data_age = 0", [](mm_config& c) { c.safety.max_market_data_age_ms = 0; }},
        {"max_market_data_age negative",
         [](mm_config& c) { c.safety.max_market_data_age_ms = -10; }},
        {"max_market_data_age exceeds nanosecond range",
         [](mm_config& c) {
             c.safety.max_market_data_age_ms = std::numeric_limits<std::int64_t>::max();
         }},
    };

    for (const auto& tc : cases)
    {
        auto cfg = default_config();
        tc.mutate(cfg);
        const auto status = validate(cfg);
        EXPECT_FALSE(status.ok) << "config should have been rejected: " << tc.label;
        EXPECT_STRNE(status.message, "") << tc.label;

        InventoryAwareMarketMakingStrategy strat;
        EXPECT_FALSE(strat.configure(cfg).ok) << tc.label;
        EXPECT_FALSE(strat.configured()) << tc.label;

        // A strategy that refused its config must not quote.
        const auto res = strat.evaluate(default_market(), flat_inventory(), default_context());
        EXPECT_EQ(res.status, strategy_status::not_configured) << tc.label;
        EXPECT_EQ(res.decision.state, mm_state::paused) << tc.label;
        EXPECT_TRUE(res.decision.intents.empty()) << tc.label;
    }
}

// ── instrument metadata ─────────────────────────────────────────────────────
TEST(MMStrategyUnit, InvalidInstrumentMetadataIsRejectedNotApproximated)
{
    instrument_spec spec;
    spec.tick_size = 0.10;
    spec.lot_size = 0.0001;
    mm_instrument ins{};

    EXPECT_EQ(make_mm_instrument(spec, ins), instrument_status::ok);

    spec.tick_size = 0.0;
    EXPECT_EQ(make_mm_instrument(spec, ins), instrument_status::invalid_tick);

    // Finer than the Price fixed-point grid (1e-4): must not be rounded away.
    spec.tick_size = 0.000001;
    EXPECT_EQ(make_mm_instrument(spec, ins), instrument_status::invalid_tick);

    spec.tick_size = 0.10;
    spec.lot_size = -1.0;
    EXPECT_EQ(make_mm_instrument(spec, ins), instrument_status::invalid_lot);

    spec.lot_size = 0.0001;
    spec.maker_rate = std::numeric_limits<double>::quiet_NaN();
    EXPECT_EQ(make_mm_instrument(spec, ins), instrument_status::invalid_fee);
}

TEST(MMStrategyUnit, EvaluateWithBrokenInstrumentPausesAndReportsStatus)
{
    auto strat = make_strategy(default_config());
    auto ctx = default_context();
    ctx.instrument.tick_raw = 0;

    const auto res = strat.evaluate(default_market(), flat_inventory(), ctx);
    EXPECT_EQ(res.status, strategy_status::invalid_instrument);
    EXPECT_EQ(res.decision.state, mm_state::paused);
    EXPECT_TRUE(res.decision.intents.empty());
    EXPECT_TRUE(has_reason(res.decision, quote_reason::invalid_instrument_metadata));
}

// ── invalid market inputs ───────────────────────────────────────────────────
TEST(MMStrategyUnit, InvalidMarketInputsFailClosed)
{
    auto strat = make_strategy(default_config());
    const auto ctx = default_context();

    struct market_case
    {
        const char* label;
        std::function<void(market_snapshot&)> mutate;
    };

    const std::vector<market_case> cases = {
        {"crossed book", [](market_snapshot& m) { m.best_bid = m.best_ask + Price(10); }},
        {"locked book", [](market_snapshot& m) { m.best_bid = m.best_ask; }},
        {"zero bid", [](market_snapshot& m) { m.best_bid = Price(0); }},
        {"negative ask", [](market_snapshot& m) { m.best_ask = Price(-1); }},
        {"negative bid size", [](market_snapshot& m) { m.best_bid_qty = -1; }},
        {"no displayed size", [](market_snapshot& m) { m.best_bid_qty = 0; m.best_ask_qty = 0; }},
        {"NaN volatility",
         [](market_snapshot& m) { m.short_horizon_volatility_bps = std::numeric_limits<double>::quiet_NaN(); }},
        {"Inf toxicity",
         [](market_snapshot& m) { m.toxicity_risk_bps = std::numeric_limits<double>::infinity(); }},
        {"negative latency risk", [](market_snapshot& m) { m.latency_risk_bps = -1.0; }},
        {"out-of-range flow signal", [](market_snapshot& m) { m.short_flow_signal = 3.0; }},
        {"fair value cannot be rounded into Price",
         [](market_snapshot& m) {
             m.best_bid = Price(std::numeric_limits<std::int64_t>::max() - 1);
             m.best_ask = Price(std::numeric_limits<std::int64_t>::max());
         }},
    };

    for (const auto& tc : cases)
    {
        auto market = default_market();
        tc.mutate(market);
        const auto d = strat.evaluate(market, flat_inventory(), ctx).decision;
        EXPECT_EQ(d.state, mm_state::paused) << tc.label;
        EXPECT_TRUE(d.intents.empty()) << tc.label;
        EXPECT_TRUE(has_reason(d, quote_reason::invalid_market_state)) << tc.label;
    }
}

TEST(MMStrategyUnit, ImplausibleInventoryLedgerPauses)
{
    auto strat = make_strategy(default_config());
    auto inv = flat_inventory();
    inv.signed_base_position = std::numeric_limits<qty_atoms>::max();

    const auto d = strat.evaluate(default_market(), inv, default_context()).decision;
    EXPECT_EQ(d.state, mm_state::paused);
    EXPECT_TRUE(d.intents.empty());
    EXPECT_TRUE(has_reason(d, quote_reason::unknown_inventory));
}

TEST(MMStrategyUnit, UnsafeLedgerHardLimitPausesBeforeHeadroomArithmetic)
{
    auto strat = make_strategy(default_config());
    auto inv = flat_inventory();
    inv.hard_limit = std::numeric_limits<qty_atoms>::max();

    const auto d = strat.evaluate(default_market(), inv, default_context()).decision;
    EXPECT_EQ(d.state, mm_state::paused);
    EXPECT_TRUE(d.intents.empty());
    EXPECT_TRUE(has_reason(d, quote_reason::unknown_inventory));
}

// ── churn guard ─────────────────────────────────────────────────────────────
TEST(MMStrategyUnit, ChurnGuardKeepsRestingQuotesForSmallMoves)
{
    auto cfg = default_config();
    cfg.quotes.minimum_refresh_ticks = 5;
    auto strat = make_strategy(cfg);

    const auto market = default_market();
    auto ctx = default_context();

    const auto fresh = strat.evaluate(market, flat_inventory(), ctx).decision;
    const auto* bid = find_intent(fresh, order_side::buy, 0);
    const auto* ask = find_intent(fresh, order_side::sell, 0);
    ASSERT_NE(bid, nullptr);
    ASSERT_NE(ask, nullptr);

    // Resting one tick away from the new target: below the refresh threshold.
    ctx.resting.has_quotes = true;
    ctx.resting.bid_price = Price(bid->price.raw() - ctx.instrument.tick_raw);
    ctx.resting.ask_price = Price(ask->price.raw() + ctx.instrument.tick_raw);
    ctx.resting.placed_time_ns = ctx.decision_time_ns - 1'000'000'000LL;

    const auto throttled = strat.evaluate(market, flat_inventory(), ctx).decision;
    EXPECT_FALSE(throttled.requote);
    EXPECT_TRUE(throttled.intents.empty());
    EXPECT_FALSE(throttled.cancel_resting_quotes);
    EXPECT_TRUE(has_reason(throttled, quote_reason::quote_refresh_throttled));

    // A large move refreshes.
    ctx.resting.bid_price = Price(bid->price.raw() - 50 * ctx.instrument.tick_raw);
    const auto refreshed = strat.evaluate(market, flat_inventory(), ctx).decision;
    EXPECT_TRUE(refreshed.requote);
    EXPECT_FALSE(refreshed.intents.empty());
}

TEST(MMStrategyUnit, ChurnGuardAcceptsExtremeRestingPriceDifferenceWithoutOverflow)
{
    auto cfg = default_config();
    cfg.quotes.minimum_refresh_ticks = 1;
    auto strat = make_strategy(cfg);
    auto ctx = default_context();
    ctx.resting.has_quotes = true;
    ctx.resting.bid_price = Price(std::numeric_limits<std::int64_t>::min());
    ctx.resting.ask_price = Price(std::numeric_limits<std::int64_t>::max());
    ctx.resting.placed_time_ns = ctx.decision_time_ns - 1'000'000'000LL;

    const auto d = strat.evaluate(default_market(), flat_inventory(), ctx).decision;
    EXPECT_TRUE(d.requote);
    EXPECT_FALSE(has_reason(d, quote_reason::quote_refresh_throttled));
}

TEST(MMStrategyUnit, ChurnGuardSaturatesExtremeOrderedTimestamps)
{
    auto cfg = default_config();
    cfg.quotes.minimum_refresh_ticks = 5;
    cfg.quotes.minimum_quote_lifetime_ms = 1;
    auto strat = make_strategy(cfg);
    auto market = default_market();
    market.event_time_ns = std::numeric_limits<timestamp_ns>::max();
    market.receive_time_ns = std::numeric_limits<timestamp_ns>::max();
    auto ctx = default_context(std::numeric_limits<timestamp_ns>::max());

    const auto fresh = strat.evaluate(market, flat_inventory(), ctx).decision;
    const auto* bid = find_intent(fresh, order_side::buy, 0);
    const auto* ask = find_intent(fresh, order_side::sell, 0);
    ASSERT_NE(bid, nullptr);
    ASSERT_NE(ask, nullptr);

    ctx.resting.has_quotes = true;
    ctx.resting.bid_price = bid->price;
    ctx.resting.ask_price = ask->price;
    ctx.resting.placed_time_ns = std::numeric_limits<timestamp_ns>::min();

    const auto d = strat.evaluate(market, flat_inventory(), ctx).decision;
    EXPECT_TRUE(d.intents.empty());
    EXPECT_TRUE(has_reason(d, quote_reason::quote_refresh_throttled));
}

TEST(MMStrategyUnit, ChurnGuardNeverHoldsQuotesThroughAPause)
{
    auto cfg = default_config();
    cfg.quotes.minimum_refresh_ticks = 100;
    cfg.quotes.minimum_quote_lifetime_ms = 60'000;
    auto strat = make_strategy(cfg);

    auto market = default_market();
    market.sequence_valid = false;

    auto ctx = default_context();
    ctx.resting.has_quotes = true;
    ctx.resting.bid_price = price_from_decimal("59988.30");
    ctx.resting.ask_price = price_from_decimal("60012.40");
    ctx.resting.placed_time_ns = ctx.decision_time_ns;

    const auto d = strat.evaluate(market, flat_inventory(), ctx).decision;
    EXPECT_EQ(d.state, mm_state::paused);
    EXPECT_TRUE(d.cancel_resting_quotes);
    EXPECT_FALSE(has_reason(d, quote_reason::quote_refresh_throttled));
}
