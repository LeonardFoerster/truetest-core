// Golden aggregation tests for footprint.md §2.2's aggregation rules:
// time/volume boundaries, empty buckets, unknown aggression, integer tick
// grouping, diagonal and stacked imbalance, deterministic POC ties, forming
// bars, CVD resets, and quote/base conversion.

#include <gtest/gtest.h>

#include "analytics/footprint/footprint_aggregator.h"

using namespace truetest::footprint;

namespace {

constexpr std::int64_t kSecond = 1'000'000'000LL;
constexpr std::int64_t kMinute = 60 * kSecond;

PublicTrade make_trade(std::int64_t price_ticks, std::int64_t qty_atoms,
                        aggressor_side side, std::int64_t event_ns,
                        std::uint64_t obs_seq = 0)
{
    PublicTrade t;
    t.event_ns = event_ns + 1;
    t.recv_ns = event_ns + 1;
    t.price_ticks = price_ticks;
    t.base_qty_atoms = qty_atoms;
    t.side = side;
    t.obs_seq = obs_seq;
    t.venue_id = 1;
    t.symbol_id = 1;
    t.session_id = 1;
    return t;
}

FootprintAggregatorConfig time_cfg(std::int64_t interval_ns = kMinute,
                                    std::int64_t group_size = 1,
                                    std::int64_t imbalance_min_volume = 0)
{
    FootprintAggregatorConfig cfg;
    cfg.bar_spec.kind = bar_kind::time;
    cfg.bar_spec.interval_ns = interval_ns;
    cfg.group_size = group_size;
    cfg.tick_size = 0.5;
    cfg.qty_atom_scale = 1.0;
    cfg.imbalance_min_volume = imbalance_min_volume;
    return cfg;
}

} // namespace

// --- Time bar boundaries ---

TEST(FootprintAggregator, TradesWithinSameIntervalShareOneBar)
{
    FootprintAggregator agg(time_cfg());
    agg.on_trade(make_trade(100, 5, aggressor_side::buy, 0));
    agg.on_trade(make_trade(101, 3, aggressor_side::sell, kSecond * 10));

    ASSERT_EQ(agg.bars().size(), 1u);
    const auto& bar = agg.bars().front();
    EXPECT_EQ(bar.state, bar_state::forming);
    EXPECT_EQ(bar.start_ns, 0);
    EXPECT_EQ(bar.end_ns, kMinute);
    EXPECT_EQ(bar.buy_volume, 5);
    EXPECT_EQ(bar.sell_volume, 3);
}

TEST(FootprintAggregator, TradeInNextIntervalClosesPreviousBar)
{
    FootprintAggregator agg(time_cfg());
    agg.on_trade(make_trade(100, 5, aggressor_side::buy, 0));
    agg.on_trade(make_trade(100, 5, aggressor_side::buy, kMinute)); // next interval

    ASSERT_EQ(agg.bars().size(), 2u);
    EXPECT_EQ(agg.bars()[0].state, bar_state::complete);
    EXPECT_EQ(agg.bars()[1].state, bar_state::forming);
    EXPECT_EQ(agg.bars()[1].start_ns, kMinute);
}

TEST(FootprintAggregator, SkippedIntervalsBecomeEmptyNotGap)
{
    FootprintAggregator agg(time_cfg());
    agg.on_trade(make_trade(100, 1, aggressor_side::buy, 0));
    // Jump 3 intervals ahead - two whole minutes have no trades at all.
    agg.on_trade(make_trade(100, 1, aggressor_side::buy, 3 * kMinute));

    ASSERT_EQ(agg.bars().size(), 4u);
    EXPECT_EQ(agg.bars()[0].state, bar_state::complete);
    EXPECT_EQ(agg.bars()[1].state, bar_state::empty);
    EXPECT_FALSE(agg.bars()[1].has_trades);
    EXPECT_FALSE(agg.bars()[1].gap); // aggregator itself never sets gap
    EXPECT_EQ(agg.bars()[1].start_ns, kMinute);
    EXPECT_EQ(agg.bars()[2].state, bar_state::empty);
    EXPECT_EQ(agg.bars()[2].start_ns, 2 * kMinute);
    EXPECT_EQ(agg.bars()[3].state, bar_state::forming);
    EXPECT_EQ(agg.bars()[3].start_ns, 3 * kMinute);
}

// --- Volume bar boundaries ---

TEST(FootprintAggregator, VolumeBarClosesAtOrAboveThreshold)
{
    FootprintAggregatorConfig cfg;
    cfg.bar_spec.kind = bar_kind::volume;
    cfg.bar_spec.volume_threshold = 100.0; // quote-notional units
    cfg.tick_size = 1.0;
    cfg.qty_atom_scale = 1.0;
    FootprintAggregator agg(cfg);

    // price 10, qty 5 -> notional 50 per trade.
    agg.on_trade(make_trade(10, 5, aggressor_side::buy, 0));
    ASSERT_EQ(agg.bars().size(), 1u);
    EXPECT_EQ(agg.bars()[0].state, bar_state::forming);

    agg.on_trade(make_trade(10, 5, aggressor_side::buy, kSecond)); // notional now 100 >= 100
    ASSERT_EQ(agg.bars().size(), 2u);
    EXPECT_EQ(agg.bars()[0].state, bar_state::complete);
    EXPECT_DOUBLE_EQ(agg.bars()[0].quote_notional, 100.0);
    EXPECT_EQ(agg.bars()[1].state, bar_state::forming);
}

TEST(FootprintAggregator, VolumeBarTradeNeverSplitAcrossBars)
{
    FootprintAggregatorConfig cfg;
    cfg.bar_spec.kind = bar_kind::volume;
    cfg.bar_spec.volume_threshold = 100.0;
    cfg.tick_size = 1.0;
    cfg.qty_atom_scale = 1.0;
    FootprintAggregator agg(cfg);

    // A single trade whose notional (500) blows well past the threshold
    // (100) must land entirely in one bar, not be split.
    agg.on_trade(make_trade(50, 10, aggressor_side::buy, 0));
    ASSERT_EQ(agg.bars().size(), 2u); // closed bar + fresh empty forming bar
    EXPECT_EQ(agg.bars()[0].buy_volume, 10);
    EXPECT_DOUBLE_EQ(agg.bars()[0].quote_notional, 500.0);
    EXPECT_EQ(agg.bars()[1].buy_volume, 0);
}

// --- Unknown aggression ---

TEST(FootprintAggregator, UnknownAggressionCountsTotalAndOhlcNotDeltaOrCvd)
{
    FootprintAggregator agg(time_cfg());
    agg.on_trade(make_trade(100, 7, aggressor_side::unknown, 0));

    const auto& bar = agg.bars().front();
    EXPECT_EQ(bar.unknown_volume, 7);
    EXPECT_EQ(bar.total_volume(), 7);
    EXPECT_EQ(bar.close_price_ticks, 100); // OHLC still updated

    const auto& cell = bar.cells.at(100);
    EXPECT_EQ(cell.unknown_base_qty, 7);
    EXPECT_EQ(cell.total(), 7);
    EXPECT_EQ(cell.delta(), 0); // excluded from delta
    EXPECT_EQ(agg.cvd(), 0);    // excluded from CVD
}

TEST(FootprintAggregator, UnknownAggressionNeverQualifiesForImbalance)
{
    FootprintAggregatorConfig cfg = time_cfg(kMinute, 1, /*imbalance_min_volume=*/1);
    FootprintAggregator agg(cfg);
    // Huge unknown volume at level 99 must not make level 100's (nonexistent)
    // buy side "imbalanced" against it - unknown isn't sell_base_qty at all.
    agg.on_trade(make_trade(99, 1000, aggressor_side::unknown, 0));
    agg.on_trade(make_trade(100, 10, aggressor_side::buy, kSecond));
    agg.flush();

    const auto& bar = agg.bars().front();
    EXPECT_EQ(bar.cells.at(100).diagonal, FootprintCell::imbalance::none);
}

// --- Integer tick grouping ---

TEST(FootprintAggregator, IntegerTickGroupingBucketsBySize)
{
    FootprintAggregator agg(time_cfg(kMinute, /*group_size=*/5));
    agg.on_trade(make_trade(100, 1, aggressor_side::buy, 0));
    agg.on_trade(make_trade(104, 1, aggressor_side::buy, kSecond));
    agg.on_trade(make_trade(105, 1, aggressor_side::buy, 2 * kSecond));

    const auto& bar = agg.bars().front();
    ASSERT_EQ(bar.cells.size(), 2u); // [100,104] -> level 20; [105,109] -> level 21
    EXPECT_EQ(bar.cells.at(20).buy_base_qty, 2);
    EXPECT_EQ(bar.cells.at(21).buy_base_qty, 1);
}

// --- Diagonal imbalance ---

TEST(FootprintAggregator, DiagonalBuyImbalanceDetectedAtRatioAndGate)
{
    FootprintAggregatorConfig cfg = time_cfg(kMinute, 1, /*imbalance_min_volume=*/10);
    FootprintAggregator agg(cfg);
    agg.on_trade(make_trade(99, 10, aggressor_side::sell, 0));   // level 99 sell = 10
    agg.on_trade(make_trade(100, 30, aggressor_side::buy, kSecond)); // level 100 buy = 30 (3x)
    agg.flush();

    const auto& bar = agg.bars().front();
    EXPECT_EQ(bar.cells.at(100).diagonal, FootprintCell::imbalance::buy);
}

TEST(FootprintAggregator, DiagonalImbalanceNotDetectedBelowRatio)
{
    FootprintAggregatorConfig cfg = time_cfg(kMinute, 1, /*imbalance_min_volume=*/1);
    FootprintAggregator agg(cfg);
    agg.on_trade(make_trade(99, 10, aggressor_side::sell, 0));
    agg.on_trade(make_trade(100, 29, aggressor_side::buy, kSecond)); // just under 3x
    agg.flush();

    EXPECT_EQ(agg.bars().front().cells.at(100).diagonal, FootprintCell::imbalance::none);
}

TEST(FootprintAggregator, DiagonalImbalanceGatedByMinVolume)
{
    FootprintAggregatorConfig cfg = time_cfg(kMinute, 1, /*imbalance_min_volume=*/100);
    FootprintAggregator agg(cfg);
    agg.on_trade(make_trade(99, 1, aggressor_side::sell, 0));
    agg.on_trade(make_trade(100, 10, aggressor_side::buy, kSecond)); // 10x ratio, but < gate
    agg.flush();

    EXPECT_EQ(agg.bars().front().cells.at(100).diagonal, FootprintCell::imbalance::none);
}

TEST(FootprintAggregator, DiagonalSellImbalanceDetected)
{
    FootprintAggregatorConfig cfg = time_cfg(kMinute, 1, /*imbalance_min_volume=*/10);
    FootprintAggregator agg(cfg);
    agg.on_trade(make_trade(100, 30, aggressor_side::sell, 0));  // level 100 sell = 30
    agg.on_trade(make_trade(101, 10, aggressor_side::buy, kSecond)); // level 101 buy = 10
    agg.flush();

    EXPECT_EQ(agg.bars().front().cells.at(100).diagonal, FootprintCell::imbalance::sell);
}

// --- Stacked imbalance ---

TEST(FootprintAggregator, StackedImbalanceMarksThreeConsecutiveLevels)
{
    FootprintAggregatorConfig cfg = time_cfg(kMinute, 1, /*imbalance_min_volume=*/10);
    FootprintAggregator agg(cfg);
    // Sell "floor" levels 99,100,101 each thin; buy levels 100,101,102 each
    // 3x the sell floor directly below them -> three consecutive buy-diagonal
    // levels (100,101,102) -> stacked.
    agg.on_trade(make_trade(99, 10, aggressor_side::sell, 0));
    agg.on_trade(make_trade(100, 10, aggressor_side::sell, 0));
    agg.on_trade(make_trade(101, 10, aggressor_side::sell, 0));
    agg.on_trade(make_trade(100, 30, aggressor_side::buy, kSecond));
    agg.on_trade(make_trade(101, 30, aggressor_side::buy, kSecond));
    agg.on_trade(make_trade(102, 30, aggressor_side::buy, kSecond));
    agg.flush();

    const auto& bar = agg.bars().front();
    EXPECT_EQ(bar.cells.at(100).diagonal, FootprintCell::imbalance::buy);
    EXPECT_EQ(bar.cells.at(101).diagonal, FootprintCell::imbalance::buy);
    EXPECT_EQ(bar.cells.at(102).diagonal, FootprintCell::imbalance::buy);
    EXPECT_TRUE(bar.cells.at(100).stacked);
    EXPECT_TRUE(bar.cells.at(101).stacked);
    EXPECT_TRUE(bar.cells.at(102).stacked);
}

TEST(FootprintAggregator, TwoConsecutiveLevelsAreNotStacked)
{
    FootprintAggregatorConfig cfg = time_cfg(kMinute, 1, /*imbalance_min_volume=*/10);
    FootprintAggregator agg(cfg);
    agg.on_trade(make_trade(99, 10, aggressor_side::sell, 0));
    agg.on_trade(make_trade(100, 10, aggressor_side::sell, 0));
    agg.on_trade(make_trade(100, 30, aggressor_side::buy, kSecond));
    agg.on_trade(make_trade(101, 30, aggressor_side::buy, kSecond));
    agg.flush();

    const auto& bar = agg.bars().front();
    EXPECT_EQ(bar.cells.at(100).diagonal, FootprintCell::imbalance::buy);
    EXPECT_FALSE(bar.cells.at(100).stacked);
    // 101 has no sell at 100... wait 100 has sell=10, so 101's diagonal
    // depends on level 100's sell, which exists - re-derive: 101 qualifies
    // too only if buy(101) >= 3x sell(100). It does (30 >= 30), so both
    // levels are diagonal-buy but the run is only length 2 -> not stacked.
    EXPECT_FALSE(bar.cells.at(101).stacked);
}

// --- Deterministic POC ties ---

TEST(FootprintAggregator, PocPicksHighestTotalVolume)
{
    FootprintAggregator agg(time_cfg());
    agg.on_trade(make_trade(100, 5, aggressor_side::buy, 0));
    agg.on_trade(make_trade(101, 20, aggressor_side::buy, kSecond));
    agg.on_trade(make_trade(102, 3, aggressor_side::sell, 2 * kSecond));
    agg.flush();

    const auto& bar = agg.bars().front();
    ASSERT_TRUE(bar.poc_valid);
    EXPECT_EQ(bar.poc_level, 101);
}

TEST(FootprintAggregator, PocTieBreaksNearestCloseThenLowerPrice)
{
    FootprintAggregator agg(time_cfg());
    // Levels 100 and 104 both get total=10; close price is 102, equidistant
    // (|100-102|==|104-102|==2) -> lower price (100) wins.
    agg.on_trade(make_trade(100, 10, aggressor_side::buy, 0));
    agg.on_trade(make_trade(104, 10, aggressor_side::buy, kSecond));
    agg.on_trade(make_trade(102, 1, aggressor_side::sell, 2 * kSecond)); // sets close=102
    agg.flush();

    const auto& bar = agg.bars().front();
    EXPECT_EQ(bar.close_price_ticks, 102);
    EXPECT_EQ(bar.poc_level, 100);
}

TEST(FootprintAggregator, PocTieBreaksNearestCloseWhenNotEquidistant)
{
    FootprintAggregator agg(time_cfg());
    // Levels 100 (total 10) and 103 (total 10) tie; close=102 -> 103 is
    // nearer (distance 1 vs 2) and wins despite being the higher price.
    agg.on_trade(make_trade(100, 10, aggressor_side::buy, 0));
    agg.on_trade(make_trade(103, 10, aggressor_side::buy, kSecond));
    agg.on_trade(make_trade(102, 1, aggressor_side::sell, 2 * kSecond));
    agg.flush();

    const auto& bar = agg.bars().front();
    EXPECT_EQ(bar.poc_level, 103);
}

// --- Forming bars stay live; completed bars are frozen ---

TEST(FootprintAggregator, FormingBarPocUpdatesLiveUntilClosed)
{
    FootprintAggregator agg(time_cfg());
    agg.on_trade(make_trade(100, 5, aggressor_side::buy, 0));
    EXPECT_EQ(agg.bars().front().poc_level, 100);

    agg.on_trade(make_trade(101, 50, aggressor_side::buy, kSecond));
    EXPECT_EQ(agg.bars().front().state, bar_state::forming);
    EXPECT_EQ(agg.bars().front().poc_level, 101); // live update
}

TEST(FootprintAggregator, ClosedBarNeverMutatesAfterLaterTrades)
{
    FootprintAggregator agg(time_cfg());
    agg.on_trade(make_trade(100, 5, aggressor_side::buy, 0));
    agg.on_trade(make_trade(100, 5, aggressor_side::buy, kMinute)); // closes bar 0

    const FootprintBar snapshot = agg.bars()[0];
    ASSERT_EQ(snapshot.state, bar_state::complete);

    // Feed a lot more trades into later bars.
    for (int i = 1; i <= 5; ++i)
        agg.on_trade(make_trade(200 + i, 100, aggressor_side::sell, kMinute + i * kMinute));

    const auto& still = agg.bars().front();
    EXPECT_EQ(still.buy_volume, snapshot.buy_volume);
    EXPECT_EQ(still.poc_level, snapshot.poc_level);
    EXPECT_EQ(still.cells.size(), snapshot.cells.size());
}

// --- CVD resets ---

TEST(FootprintAggregator, CvdAccumulatesBuyMinusSellExcludingUnknown)
{
    FootprintAggregator agg(time_cfg());
    agg.on_trade(make_trade(100, 10, aggressor_side::buy, 0));
    agg.on_trade(make_trade(100, 4, aggressor_side::sell, kSecond));
    agg.on_trade(make_trade(100, 1000, aggressor_side::unknown, 2 * kSecond));
    EXPECT_EQ(agg.cvd(), 6);
}

TEST(FootprintAggregator, BarCvdSnapshotsRunningTotalAndFreezesOnClose)
{
    FootprintAggregator agg(time_cfg());
    agg.on_trade(make_trade(100, 10, aggressor_side::buy, 0));
    EXPECT_EQ(agg.bars().front().cvd, 10); // live on the forming bar

    agg.on_trade(make_trade(100, 4, aggressor_side::sell, kSecond));
    EXPECT_EQ(agg.bars().front().cvd, 6);

    agg.on_trade(make_trade(100, 100, aggressor_side::buy, kMinute)); // closes bar 0, opens bar 1
    ASSERT_EQ(agg.bars().size(), 2u);
    EXPECT_EQ(agg.bars()[0].cvd, 6);   // frozen at close
    EXPECT_EQ(agg.bars()[1].cvd, 106); // continues the running total, not reset per-bar
}

TEST(FootprintAggregator, CvdResetsAtUtcSessionBoundary)
{
    FootprintAggregatorConfig cfg = time_cfg();
    cfg.cvd_reset_ns_of_day = 0; // 00:00 UTC
    FootprintAggregator agg(cfg);

    constexpr std::int64_t kDayNs = 86'400LL * kSecond;
    const std::int64_t just_before_midnight = kDayNs - kSecond;
    const std::int64_t just_after_midnight = kDayNs + kSecond;

    agg.on_trade(make_trade(100, 10, aggressor_side::buy, just_before_midnight));
    EXPECT_EQ(agg.cvd(), 10);

    agg.on_trade(make_trade(100, 3, aggressor_side::sell, just_after_midnight));
    EXPECT_EQ(agg.cvd(), -3); // reset wiped the prior +10 before applying -3
}

TEST(FootprintAggregator, CvdDoesNotResetWithinSameUtcDay)
{
    FootprintAggregatorConfig cfg = time_cfg();
    FootprintAggregator agg(cfg);
    agg.on_trade(make_trade(100, 10, aggressor_side::buy, kSecond));
    agg.on_trade(make_trade(100, 3, aggressor_side::sell, kSecond * 3600)); // +1h, same day
    EXPECT_EQ(agg.cvd(), 7);
}

TEST(FootprintAggregator, OverflowingEconomicVolumeFailsWithoutMutation)
{
    FootprintAggregator agg(time_cfg());
    ASSERT_TRUE(agg.on_trade(make_trade(
        100, std::numeric_limits<std::int64_t>::max(),
        aggressor_side::buy, 0)));
    const auto version = agg.version();
    const auto snapshot = agg.bars().front();

    EXPECT_FALSE(agg.on_trade(make_trade(100, 1, aggressor_side::buy,
                                         kSecond)));
    ASSERT_EQ(agg.bars().size(), 1u);
    EXPECT_EQ(agg.version(), version);
    EXPECT_EQ(agg.cvd(), std::numeric_limits<std::int64_t>::max());
    EXPECT_EQ(agg.bars().front().buy_volume, snapshot.buy_volume);
    EXPECT_EQ(agg.bars().front().cells.at(100).buy_base_qty,
              snapshot.cells.at(100).buy_base_qty);
}

TEST(FootprintAggregator, RejectsOutOfOrderAndPostFlushTradesWithoutMutation)
{
    FootprintAggregator agg(time_cfg());
    ASSERT_TRUE(agg.on_trade(make_trade(100, 1, aggressor_side::buy,
                                        kSecond)));
    const auto version = agg.version();
    EXPECT_FALSE(agg.on_trade(make_trade(90, 1, aggressor_side::sell, 0)));
    EXPECT_EQ(agg.version(), version);

    agg.flush();
    const auto sealed_version = agg.version();
    EXPECT_FALSE(agg.on_trade(make_trade(110, 1, aggressor_side::buy,
                                         2 * kSecond)));
    EXPECT_EQ(agg.version(), sealed_version);
    ASSERT_EQ(agg.bars().size(), 1u);
    EXPECT_EQ(agg.bars().front().state, bar_state::complete);
}

TEST(FootprintAggregator, EpochZeroAndUnresolvedIdentityFailWithoutMutation)
{
    FootprintAggregator agg(time_cfg());
    auto invalid = make_trade(100, 1, aggressor_side::buy, 0);
    invalid.event_ns = 0;
    invalid.recv_ns = 1;
    EXPECT_FALSE(agg.on_trade(invalid));
    invalid = make_trade(100, 1, aggressor_side::buy, 0);
    invalid.symbol_id = kInvalidSymbolId;
    EXPECT_FALSE(agg.on_trade(invalid));
    EXPECT_TRUE(agg.bars().empty());
    EXPECT_EQ(agg.version(), 0u);
}

TEST(FootprintAggregator, DenseSymbolIdZeroIsValid)
{
    FootprintAggregator agg(time_cfg());
    auto trade = make_trade(100, 1, aggressor_side::buy, 0);
    trade.symbol_id = 0;
    EXPECT_TRUE(agg.on_trade(trade));
    ASSERT_EQ(agg.bars().size(), 1u);
}

// --- Quote/base conversion ---

TEST(FootprintAggregator, QuoteNotionalAccumulatesPriceTimesQty)
{
    FootprintAggregatorConfig cfg = time_cfg();
    cfg.tick_size = 0.25;
    cfg.qty_atom_scale = 100.0; // 100 atoms per whole unit
    FootprintAggregator agg(cfg);

    // price_ticks=400 -> price=100.0; qty_atoms=250 -> qty=2.5 -> notional=250.0
    agg.on_trade(make_trade(400, 250, aggressor_side::buy, 0));
    EXPECT_DOUBLE_EQ(agg.bars().front().quote_notional, 250.0);
}

// --- flush() / reset() / version() / max_bars ---

TEST(FootprintAggregator, FlushOnEmptyAggregatorIsNoop)
{
    FootprintAggregator agg(time_cfg());
    EXPECT_NO_THROW(agg.flush());
    EXPECT_TRUE(agg.bars().empty());
}

TEST(FootprintAggregator, FlushIsIdempotent)
{
    FootprintAggregator agg(time_cfg());
    agg.on_trade(make_trade(100, 1, aggressor_side::buy, 0));
    agg.flush();
    const auto poc_after_first_flush = agg.bars().front().poc_level;
    const auto version_after_first_flush = agg.version();
    agg.flush(); // second flush must not crash or change anything
    EXPECT_EQ(agg.bars().front().poc_level, poc_after_first_flush);
    EXPECT_EQ(agg.bars().front().state, bar_state::complete);
    EXPECT_EQ(agg.version(), version_after_first_flush);
}

TEST(FootprintAggregator, VersionBumpsOnEveryTradeAndReset)
{
    FootprintAggregator agg(time_cfg());
    const auto v0 = agg.version();
    agg.on_trade(make_trade(100, 1, aggressor_side::buy, 0));
    const auto v1 = agg.version();
    EXPECT_GT(v1, v0);

    agg.reset(time_cfg());
    EXPECT_GT(agg.version(), v1);
    EXPECT_TRUE(agg.bars().empty());
    EXPECT_EQ(agg.cvd(), 0);
}

TEST(FootprintAggregator, MaxBarsEvictsOldestFromFront)
{
    FootprintAggregatorConfig cfg = time_cfg();
    cfg.max_bars = 3;
    FootprintAggregator agg(cfg);

    for (int i = 0; i < 6; ++i)
        agg.on_trade(make_trade(100, 1, aggressor_side::buy, i * kMinute));

    EXPECT_EQ(agg.bars().size(), 3u);
    EXPECT_EQ(agg.bars().front().start_ns, 3 * kMinute);
    EXPECT_EQ(agg.bars().back().start_ns, 5 * kMinute);
}

// --- Config sanitization / anomalous-gap safety (verifier-found bugs) ---

TEST(FootprintAggregator, InvalidConfigurationFailsClosedWithoutMutation)
{
    FootprintAggregatorConfig zero = time_cfg();
    zero.bar_spec.interval_ns = 0;
    FootprintAggregator agg_zero(zero);
    EXPECT_FALSE(agg_zero.on_trade(make_trade(100, 1, aggressor_side::buy, 0)));
    EXPECT_TRUE(agg_zero.bars().empty());

    FootprintAggregatorConfig negative = time_cfg();
    negative.bar_spec.interval_ns = -kSecond;
    FootprintAggregator agg_neg(negative);
    EXPECT_FALSE(agg_neg.on_trade(make_trade(100, 1, aggressor_side::buy, 0)));
    EXPECT_TRUE(agg_neg.bars().empty());
}

TEST(FootprintAggregator, HugeTimestampGapDoesNotHangOrExceedMaxBars)
{
    FootprintAggregatorConfig cfg = time_cfg(kSecond, /*group_size=*/1);
    cfg.max_bars = 5;
    FootprintAggregator agg(cfg);

    agg.on_trade(make_trade(100, 1, aggressor_side::buy, 0));
    // A ~10-year gap at 1s bars would be ~300M synthesized EMPTY bars
    // without the fast-forward bound. This must return promptly and never
    // hold more than max_bars.
    const std::int64_t ten_years_ns = 10LL * 365 * 24 * 3600 * kSecond;
    agg.on_trade(make_trade(100, 1, aggressor_side::buy, ten_years_ns));

    EXPECT_LE(agg.bars().size(), cfg.max_bars);
    EXPECT_EQ(agg.bars().back().has_trades, true);
}

// --- Additional coverage: non-zero CVD reset hour, longer imbalance runs ---

TEST(FootprintAggregator, CvdResetsAtNonZeroUtcHour)
{
    FootprintAggregatorConfig cfg = time_cfg();
    constexpr std::int64_t kHour = 3600 * kSecond;
    cfg.cvd_reset_ns_of_day = 8 * kHour; // 08:00 UTC
    FootprintAggregator agg(cfg);

    constexpr std::int64_t kDayNs = 86'400LL * kSecond;
    const std::int64_t just_before_reset = kDayNs + 8 * kHour - kSecond;
    const std::int64_t just_after_reset = kDayNs + 8 * kHour + kSecond;

    agg.on_trade(make_trade(100, 10, aggressor_side::buy, just_before_reset));
    EXPECT_EQ(agg.cvd(), 10);

    agg.on_trade(make_trade(100, 4, aggressor_side::sell, just_after_reset));
    EXPECT_EQ(agg.cvd(), -4); // reset wiped the prior +10

    // A later trade still well within the same session must not re-reset.
    agg.on_trade(make_trade(100, 1, aggressor_side::buy, just_after_reset + kHour));
    EXPECT_EQ(agg.cvd(), -3);
}

TEST(FootprintAggregator, StackedImbalanceRunOfFourAllMarked)
{
    FootprintAggregatorConfig cfg = time_cfg(kMinute, 1, /*imbalance_min_volume=*/10);
    FootprintAggregator agg(cfg);
    for (std::int64_t lvl = 99; lvl <= 102; ++lvl)
        agg.on_trade(make_trade(lvl, 10, aggressor_side::sell, 0));
    for (std::int64_t lvl = 100; lvl <= 103; ++lvl)
        agg.on_trade(make_trade(lvl, 30, aggressor_side::buy, kSecond));
    agg.flush();

    const auto& bar = agg.bars().front();
    for (std::int64_t lvl = 100; lvl <= 103; ++lvl)
    {
        EXPECT_EQ(bar.cells.at(lvl).diagonal, FootprintCell::imbalance::buy) << lvl;
        EXPECT_TRUE(bar.cells.at(lvl).stacked) << lvl;
    }
}

TEST(FootprintAggregator, DirectionChangeBreaksAStackedRun)
{
    FootprintAggregatorConfig cfg = time_cfg(kMinute, 1, /*imbalance_min_volume=*/10);
    FootprintAggregator agg(cfg);
    // 100,101 qualify buy-diagonal (thin sell floor directly below each);
    // 102 is adjacent to 101 but qualifies sell-diagonal instead (thin buy
    // roof directly above it) - the direction change at 102 must cap the
    // buy run at length 2, so nothing here stacks.
    agg.on_trade(make_trade(99, 10, aggressor_side::sell, 0));   // floor for 100's buy-diagonal
    agg.on_trade(make_trade(100, 10, aggressor_side::sell, 0));  // floor for 101's buy-diagonal
    agg.on_trade(make_trade(100, 30, aggressor_side::buy, kSecond));
    agg.on_trade(make_trade(101, 30, aggressor_side::buy, kSecond));
    agg.on_trade(make_trade(102, 30, aggressor_side::sell, kSecond)); // dominant sell at 102
    agg.on_trade(make_trade(103, 5, aggressor_side::buy, kSecond));   // thin roof above 102
    agg.flush();

    const auto& bar = agg.bars().front();
    EXPECT_EQ(bar.cells.at(100).diagonal, FootprintCell::imbalance::buy);
    EXPECT_EQ(bar.cells.at(101).diagonal, FootprintCell::imbalance::buy);
    EXPECT_EQ(bar.cells.at(102).diagonal, FootprintCell::imbalance::sell);
    EXPECT_FALSE(bar.cells.at(100).stacked);
    EXPECT_FALSE(bar.cells.at(101).stacked); // run of 2 - below the stacking threshold
    EXPECT_FALSE(bar.cells.at(102).stacked); // direction differs from its neighbor's run
}
