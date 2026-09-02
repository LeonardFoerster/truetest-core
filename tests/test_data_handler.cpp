#include <gtest/gtest.h>
#include "api/market_series_symbol_policy.h"
#include "data/data_handler.h"
#include "data/data_source.h"
#include "data/market_series.h"
#include "data/market_types.h"
#include "data/csv_data_source.h"
#include "data/tick_csv_data_source.h"
#include "data/data_wrapper.h"
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace {
// Same-named struct with a different layout exists in other test TUs;
// anonymous-namespace scope prevents LTO ODR-merging from picking the
// wrong definition.
struct SilenceOutput {
    std::ostringstream sink;
    std::streambuf* orig_out;
    std::streambuf* orig_err;
    SilenceOutput()
        : orig_out(std::cout.rdbuf(sink.rdbuf()))
        , orig_err(std::cerr.rdbuf(sink.rdbuf())) {}
    ~SilenceOutput() {
        std::cout.rdbuf(orig_out);
        std::cerr.rdbuf(orig_err);
    }
};

class TypedFailureSource final : public IMarketSource
{
public:
    bool load_into(IMarketSink&, LoadStats*) override { return false; }
    bool supports_stream() const override { return true; }
    StreamResult stream_into(IMarketSink&, std::atomic<bool>*, LoadStats*) override
    {
        return {stream_termination::transport_failure, 3, 1};
    }
};

class PartiallyRejectingSource final : public IMarketSource
{
public:
    bool load_into(IMarketSink& sink, LoadStats* stats) override
    {
        Bar bar;
        bar.date = "1704067200000";
        bar.symbol = "BTCUSDT";
        bar.open = 100.0;
        bar.high = 101.0;
        bar.low = 99.0;
        bar.close = 100.0;
        bar.volume = 1;
        const bool accepted = sink.on_bar(bar);
        if (stats) *stats = {accepted ? 1u : 0u, 1u, {}};
        return accepted;
    }
};

class UnderReportingRejectingSource final : public IMarketSource
{
public:
    bool load_into(IMarketSink& sink, LoadStats* stats) override
    {
        Bar valid;
        valid.date = "1704067200000";
        valid.symbol = "BTCUSDT";
        valid.open = 100.0;
        valid.high = 101.0;
        valid.low = 99.0;
        valid.close = 100.0;
        valid.volume = 1;
        const bool accepted = sink.on_bar(valid);

        Bar invalid = valid;
        invalid.high = std::numeric_limits<double>::quiet_NaN();
        const bool rejected = !sink.on_bar(invalid);
        if (stats) *stats = {accepted ? 1u : 0u, 0u, {}};
        return accepted && rejected;
    }
};

class RejectingThenThrowingSource final : public IMarketSource
{
public:
    bool load_into(IMarketSink& sink, LoadStats* stats) override
    {
        Bar invalid;
        invalid.date = "1704067200000";
        invalid.symbol = "BTCUSDT";
        invalid.open = 100.0;
        invalid.high = std::numeric_limits<double>::quiet_NaN();
        invalid.low = 99.0;
        invalid.close = 100.0;
        invalid.volume = 1;
        (void)sink.on_bar(invalid);
        if (stats) *stats = {0u, 0u, "source failed after sink rejection"};
        throw std::runtime_error("source transport failure");
    }
};

class LegacyAppendingSource final : public IDataSource
{
public:
    bool load_data(std::shared_ptr<data_handler> handler) override
    {
        return handler && handler->load_into_queue(
            "1704067200000", "BTCUSDT", 100.0, 101.0, 99.0, 100.0, 1);
    }
};
}  // namespace

// Helper: path to test fixtures relative to the test binary
static std::string fixture_path(const std::string& name)
{
    std::string p = std::string(TEST_FIXTURES_DIR) + "/" + name;
    return p;
}

TEST(DataHandler, LoadIntoQueue)
{
    data_handler dh;
    dh.load_into_queue("2024-01-01", "AAPL", 1.0, 2.0, 0.5, 1.5, 100);
    dh.load_into_queue("2024-01-02", "AAPL", 2.0, 3.0, 1.5, 2.5, 200);
    dh.load_into_queue("2024-01-03", "AAPL", 3.0, 4.0, 2.5, 3.5, 300);
    EXPECT_EQ(dh.bar_count(), 3u);
    EXPECT_DOUBLE_EQ(dh.bar_at(0).close, 1.5);
    EXPECT_DOUBLE_EQ(dh.bar_at(2).close, 3.5);
}

TEST(DataHandler, HasBarData)
{
    // Use valid bar values (low > 0): the loader's input-validation guard
    // rejects rows with non-positive prices since they break downstream
    // mid/spread math. Earlier this test passed `low=0` which silently
    // dropped the row and left the handler empty.
    data_handler dh;
    dh.load_into_queue("2024-01-01", "X", 1.0, 2.0, 0.5, 1.5, 100);
    EXPECT_TRUE(dh.has_bar_data());
    EXPECT_FALSE(dh.has_tick_data());
}

TEST(DataHandler, HasTickData)
{
    data_handler dh;
    tick_record t;
    t.symbol = "X";
    t.price = 1.0;
    t.quantity = 1;
    t.side = data_tick_side::unknown;
    t.timestamp = std::chrono::system_clock::now();
    ASSERT_TRUE(dh.add_tick(t));
    EXPECT_TRUE(dh.has_tick_data());
    EXPECT_FALSE(dh.has_bar_data());
}

TEST(MarketSeries, OnBarOnTick)
{
    MarketSeries series;
    Bar b;
    b.date = "2024-01-01";
    b.symbol = "AAPL";
    b.open = 150; b.high = 155; b.low = 149; b.close = 153; b.volume = 1000;
    b.quantity_scale = 100'000'000ULL;
    ASSERT_TRUE(series.on_bar(b));
    EXPECT_EQ(series.bar_count(), 1u);
    EXPECT_EQ(series.bar_at(0).symbol, "AAPL");
    EXPECT_DOUBLE_EQ(series.bar_at(0).close, 153.0);
    EXPECT_EQ(series.bar_at(0).quantity_scale, 100'000'000ULL);

    Tick t;
    t.symbol = "AAPL";
    t.price = 150.25;
    t.quantity = 10;
    t.quantity_scale = 100'000'000ULL;
    t.side = data_tick_side::bid;
    t.timestamp = std::chrono::system_clock::time_point(std::chrono::milliseconds(1704067200000));
    ASSERT_TRUE(series.on_tick(t));
    EXPECT_EQ(series.tick_count(), 1u);
    EXPECT_DOUBLE_EQ(series.tick_at(0).price, 150.25);
    EXPECT_EQ(series.tick_at(0).quantity_scale, 100'000'000ULL);
}

TEST(MarketSeries, ValidationRejectsNonPositive)
{
    SilenceOutput quiet;
    MarketSeries series;
    Bar b;
    b.symbol = "X";
    b.open = -1; b.high = 1; b.low = 0.5; b.close = 1; b.volume = 1;
    EXPECT_FALSE(series.on_bar(b));
    EXPECT_EQ(series.bar_count(), 0u);
    EXPECT_EQ(series.validation_errors(), 1u);
}

TEST(MarketSeries, ValidationRejectsNonFinitePricesWithoutMutation)
{
    SilenceOutput quiet;
    MarketSeries series;

    Bar bar;
    bar.symbol = "X";
    bar.open = std::numeric_limits<double>::quiet_NaN();
    bar.high = 2.0;
    bar.low = 0.5;
    bar.close = 1.0;
    bar.volume = 1;
    EXPECT_FALSE(series.on_bar(bar));

    Tick tick;
    tick.symbol = "X";
    tick.price = std::numeric_limits<double>::infinity();
    tick.quantity = 1;
    tick.timestamp = std::chrono::system_clock::time_point{
        std::chrono::milliseconds{1}};
    EXPECT_FALSE(series.on_tick(tick));

    EXPECT_EQ(series.bar_count(), 0u);
    EXPECT_EQ(series.tick_count(), 0u);
    EXPECT_EQ(series.validation_errors(), 2u);
}

TEST(MarketSeries, ValidationRejectsPreEpochTimestampsWithoutMutation)
{
    SilenceOutput quiet;
    MarketSeries series;

    Bar bar;
    bar.ts = std::chrono::system_clock::time_point{
        std::chrono::milliseconds{-1}};
    bar.symbol = "X";
    bar.open = 1.0;
    bar.high = 2.0;
    bar.low = 0.5;
    bar.close = 1.5;
    bar.volume = 1;
    EXPECT_FALSE(series.on_bar(bar));

    Tick tick;
    tick.timestamp = std::chrono::system_clock::time_point{
        std::chrono::milliseconds{-1}};
    tick.symbol = "X";
    tick.price = 1.0;
    tick.quantity = 1;
    EXPECT_FALSE(series.on_tick(tick));

    EXPECT_TRUE(series.empty());
    EXPECT_EQ(series.validation_errors(), 2u);
}

TEST(MarketSeries, ValidationRejectsZeroVolumeWithoutMutation)
{
    SilenceOutput quiet;
    MarketSeries series;
    Bar bar;
    bar.ts = std::chrono::system_clock::time_point{
        std::chrono::milliseconds{1}};
    bar.symbol = "X";
    bar.open = 1.0;
    bar.high = 2.0;
    bar.low = 0.5;
    bar.close = 1.5;
    bar.volume = 0;

    EXPECT_FALSE(series.on_bar(bar));
    EXPECT_EQ(series.bar_count(), 0u);
    EXPECT_EQ(series.last_bar_rejection(),
              tt::data_provenance::rejection_reason::non_positive_volume);
}

TEST(MarketSeries, ValidationRejectsUnknownBarAndTickTimeWithoutMutation)
{
    SilenceOutput quiet;
    MarketSeries series;

    Bar bar;
    bar.symbol = "X";
    bar.open = 1.0;
    bar.high = 2.0;
    bar.low = 0.5;
    bar.close = 1.5;
    bar.volume = 1;
    EXPECT_FALSE(series.on_bar(bar));

    Tick tick;
    tick.symbol = "X";
    tick.price = 1.0;
    tick.quantity = 1;
    EXPECT_FALSE(series.on_tick(tick));

    EXPECT_EQ(series.bar_count(), 0u);
    EXPECT_EQ(series.tick_count(), 0u);
}

TEST(MarketSeries, ValidationRejectsCompleteOhlcGeometry)
{
    SilenceOutput quiet;
    MarketSeries series;

    struct invalid_case
    {
        double open;
        double high;
        double low;
        double close;
        tt::data_provenance::rejection_reason expected;
    };
    const invalid_case cases[] = {
        { 94.0, 105.0, 95.0, 102.0,
          tt::data_provenance::rejection_reason::open_outside_range },
        { 106.0, 105.0, 95.0, 102.0,
          tt::data_provenance::rejection_reason::open_outside_range },
        { 100.0, 105.0, 95.0, 94.0,
          tt::data_provenance::rejection_reason::close_outside_range },
        { 100.0, 105.0, 95.0, 106.0,
          tt::data_provenance::rejection_reason::close_outside_range },
    };

    for (const auto& test : cases)
    {
        Bar bar;
        bar.symbol = "X";
        bar.open = test.open;
        bar.high = test.high;
        bar.low = test.low;
        bar.close = test.close;
        bar.volume = 1;
        EXPECT_FALSE(series.on_bar(bar));
        EXPECT_EQ(series.last_bar_rejection(), test.expected);
    }

    EXPECT_EQ(series.bar_count(), 0u);
    EXPECT_EQ(series.validation_errors(), std::size(cases));
}

TEST(MarketSeries, RollbackRestoresLastBarRejection)
{
    SilenceOutput quiet;
    MarketSeries series;

    Bar first_invalid;
    first_invalid.open = 0;
    first_invalid.high = 105;
    first_invalid.low = 95;
    first_invalid.close = 102;
    EXPECT_FALSE(series.on_bar(first_invalid));
    EXPECT_EQ(series.last_bar_rejection(),
              tt::data_provenance::rejection_reason::non_positive_price);
    const auto checkpoint = series.append_checkpoint();

    Bar second_invalid;
    second_invalid.open = 106;
    second_invalid.high = 105;
    second_invalid.low = 95;
    second_invalid.close = 102;
    EXPECT_FALSE(series.on_bar(second_invalid));
    EXPECT_EQ(series.last_bar_rejection(),
              tt::data_provenance::rejection_reason::open_outside_range);

    series.rollback_appends(checkpoint);
    EXPECT_EQ(series.validation_errors(), 1u);
    EXPECT_EQ(series.last_bar_rejection(),
              tt::data_provenance::rejection_reason::non_positive_price);
}

TEST(MarketSeries, ClearKeepsCapacityForMcReuse)
{
    MarketSeries series;
    series.reserve_bars(1000);
    series.load_into_queue("2024-01-01", "AAPL", 1, 2, 0.5, 1.5, 100);
    series.load_into_queue("2024-01-02", "AAPL", 2, 3, 1.5, 2.5, 200);
    EXPECT_EQ(series.bar_count(), 2u);
    series.clear();
    EXPECT_EQ(series.bar_count(), 0u);
    EXPECT_TRUE(series.empty());
    // Reload after clear (MC reuse path)
    series.load_into_queue("2024-01-03", "AAPL", 3, 4, 2.5, 3.5, 300);
    EXPECT_EQ(series.bar_count(), 1u);
}

TEST(MarketSeries, SetAllBarSymbolsOnEmptySeriesCannotCreateAPhantomBar)
{
    MarketSeries series;

    series.set_all_bar_symbols("BTCUSDT");

    EXPECT_TRUE(series.empty());
    EXPECT_FALSE(series.has_bar_data());
    EXPECT_EQ(series.bar_count(), 0u);
    EXPECT_TRUE(series.first_symbol().empty());

    ASSERT_TRUE(series.load_into_queue(
        "1704067200000", "ETHUSDT", 100, 101, 99, 100, 1));
    ASSERT_EQ(series.bar_count(), 1u);
    EXPECT_EQ(series.bar_symbol_at(0), "ETHUSDT");
    EXPECT_DOUBLE_EQ(series.bar_at(0).close, 100.0);
}

TEST(MarketSeries, SetAllBarSymbolsOnTickOnlySeriesCannotCreateAPhantomBar)
{
    MarketSeries series;
    Tick tick;
    tick.timestamp = std::chrono::system_clock::time_point{
        std::chrono::milliseconds{1}};
    tick.symbol = "ETHUSDT";
    tick.price = 100.0;
    tick.quantity = 1;
    ASSERT_TRUE(series.on_tick(tick));

    series.set_all_bar_symbols("BTCUSDT");

    EXPECT_EQ(series.bar_count(), 0u);
    EXPECT_EQ(series.tick_count(), 1u);
    EXPECT_FALSE(series.has_bar_data());
    EXPECT_EQ(series.tick_at(0).symbol, "ETHUSDT");
}

TEST(MarketSeries, SetAllBarSymbolsTreatsAnEmptyTargetAsNoOp)
{
    MarketSeries series;
    ASSERT_TRUE(series.load_into_queue(
        "1704067200000", "BTCUSDT", 100, 101, 99, 100, 1));

    series.set_all_bar_symbols("");

    ASSERT_EQ(series.bar_count(), 1u);
    EXPECT_EQ(series.bar_symbol_at(0), "BTCUSDT");
}

TEST(MarketSeries, SetAllBarSymbolsPreservesSoACardinalityAndNonSymbolProjection)
{
    struct projection
    {
        std::chrono::system_clock::time_point ts;
        std::string date;
        double open;
        double high;
        double low;
        double close;
        std::int64_t volume;
        std::uint64_t quantity_scale;
    };

    for (std::size_t count = 0; count <= 32; ++count)
    {
        MarketSeries series;
        std::vector<projection> before;
        before.reserve(count);
        for (std::size_t i = 0; i < count; ++i)
        {
            const auto timestamp = 1'704'067'200'000LL
                + static_cast<long long>(i) * 60'000LL;
            const double price = 100.0 + static_cast<double>(i);
            ASSERT_TRUE(series.load_into_queue(
                std::to_string(timestamp), i % 2 == 0 ? "BTCUSDT" : "ETHUSDT",
                price, price + 1.0, price - 1.0, price + 0.5,
                static_cast<std::int64_t>(i + 1), i + 1));
            const auto bar = series.bar_at(i);
            before.push_back({bar.ts, std::string(bar.date), bar.open, bar.high,
                              bar.low, bar.close, bar.volume,
                              bar.quantity_scale});
        }

        series.set_all_bar_symbols("SOLUSDT");

        ASSERT_EQ(series.bar_count(), count);
		for (std::size_t i = 0; i < count; ++i)
		{
			const auto bar = series.bar_at(i);
			EXPECT_EQ(bar.ts, before[i].ts);
            EXPECT_EQ(bar.date, before[i].date);
            EXPECT_DOUBLE_EQ(bar.open, before[i].open);
            EXPECT_DOUBLE_EQ(bar.high, before[i].high);
            EXPECT_DOUBLE_EQ(bar.low, before[i].low);
            EXPECT_DOUBLE_EQ(bar.close, before[i].close);
            EXPECT_EQ(bar.volume, before[i].volume);
            EXPECT_EQ(bar.quantity_scale, before[i].quantity_scale);
        }
	}
}

TEST(MarketSeries, AppendAfterExistingRowRenameKeepsEverySoAColumnAligned)
{
	MarketSeries series;
	ASSERT_TRUE(series.load_into_queue(
		"1704067200000", "BTCUSDT", 100, 101, 99, 100.5, 7, 10));
	const auto before = series.bar_at(0);
	const auto before_timestamp = before.ts;
	const std::string before_date{before.date};

	series.set_all_bar_symbols("SOLUSDT");
	ASSERT_TRUE(series.load_into_queue(
		"1704067260000", "ETHUSDT", 200, 201, 199, 200.5, 11, 20));

	ASSERT_EQ(series.bar_count(), 2u);
	const auto first = series.bar_at(0);
	EXPECT_EQ(first.ts, before_timestamp);
	EXPECT_EQ(first.date, before_date);
	EXPECT_DOUBLE_EQ(first.open, 100.0);
	EXPECT_DOUBLE_EQ(first.high, 101.0);
	EXPECT_DOUBLE_EQ(first.low, 99.0);
	EXPECT_DOUBLE_EQ(first.close, 100.5);
	EXPECT_EQ(first.volume, 7);
	EXPECT_EQ(first.quantity_scale, 10u);
	const auto second = series.bar_at(1);
	EXPECT_EQ(second.symbol, "ETHUSDT");
	EXPECT_DOUBLE_EQ(second.close, 200.5);
	EXPECT_EQ(second.volume, 11);
	EXPECT_EQ(second.quantity_scale, 20u);
}

TEST(MarketSeries, BindUnsetSymbolsCoversBarsAndTicksAndIsIdempotent)
{
    MarketSeries series;
    ASSERT_TRUE(series.load_into_queue(
        "1704067200000", "", 100, 101, 99, 100, 1));
    ASSERT_TRUE(series.load_into_queue(
        "1704067260000", "ETHUSDT", 200, 201, 199, 200, 1));
    Tick unnamed;
    unnamed.timestamp = std::chrono::system_clock::time_point{
        std::chrono::milliseconds{1}};
    unnamed.price = 100.0;
    unnamed.quantity = 1;
    ASSERT_TRUE(series.on_tick(unnamed));
    Tick named = unnamed;
    named.timestamp += std::chrono::milliseconds{1};
    named.symbol = "ETHUSDT";
    ASSERT_TRUE(series.on_tick(named));

    const auto first = series.bind_unset_symbols("BTCUSDT");
    EXPECT_EQ(first.bars, 1u);
    EXPECT_EQ(first.ticks, 1u);
    EXPECT_EQ(series.bar_symbol_at(0), "BTCUSDT");
    EXPECT_EQ(series.bar_symbol_at(1), "ETHUSDT");
    EXPECT_EQ(series.tick_at(0).symbol, "BTCUSDT");
    EXPECT_EQ(series.tick_at(1).symbol, "ETHUSDT");

    const auto second = series.bind_unset_symbols("BTCUSDT");
    EXPECT_EQ(second.bars, 0u);
	EXPECT_EQ(second.ticks, 0u);
}

TEST(CApiSymbolPolicy, RejectsUnboundDataWithoutAnExpectedSymbol)
{
	MarketSeries series;
	ASSERT_TRUE(series.load_into_queue(
		"1704067200000", "", 100, 101, 99, 100, 1));

	const auto result =
		tt::api::enforce_series_symbol_policy(series, std::nullopt);

	EXPECT_EQ(result.error,
	          tt::api::series_symbol_error::unbound_without_expected_symbol);
	EXPECT_TRUE(series.bar_symbol_at(0).empty());
}

TEST(CApiSymbolPolicy, ExplicitIdentityBindsOnlyPreviouslyUnboundRecords)
{
	MarketSeries series;
	ASSERT_TRUE(series.load_into_queue(
		"1704067200000", "BTCUSDT", 100, 101, 99, 100, 1));
	ASSERT_TRUE(series.load_into_queue(
		"1704067260000", "", 101, 102, 100, 101, 1));

	const auto result = tt::api::enforce_series_symbol_policy(
		series, std::string_view{"BTCUSDT"});

	ASSERT_TRUE(result.success());
	EXPECT_EQ(result.bound.bars, 1u);
	EXPECT_EQ(series.bar_symbol_at(0), "BTCUSDT");
	EXPECT_EQ(series.bar_symbol_at(1), "BTCUSDT");
}

TEST(CApiSymbolPolicy, MismatchRejectsAtomicallyBeforeBindingBlankRecords)
{
	MarketSeries series;
	ASSERT_TRUE(series.load_into_queue(
		"1704067200000", "ETHUSDT", 100, 101, 99, 100, 1));
	ASSERT_TRUE(series.load_into_queue(
		"1704067260000", "", 101, 102, 100, 101, 1));

	const auto result = tt::api::enforce_series_symbol_policy(
		series, std::string_view{"BTCUSDT"});

	EXPECT_EQ(result.error,
	          tt::api::series_symbol_error::expected_symbol_mismatch);
	EXPECT_EQ(result.observed_symbol, "ETHUSDT");
	EXPECT_EQ(series.bar_symbol_at(0), "ETHUSDT");
	EXPECT_TRUE(series.bar_symbol_at(1).empty());
}

TEST(CApiSymbolPolicy, TickMismatchAlsoRejectsBeforeBindingBlankBars)
{
	MarketSeries series;
	ASSERT_TRUE(series.load_into_queue(
		"1704067200000", "", 100, 101, 99, 100, 1));
	Tick tick;
	tick.timestamp = std::chrono::system_clock::time_point{
		std::chrono::milliseconds{1}};
	tick.symbol = "ETHUSDT";
	tick.price = 100.0;
	tick.quantity = 1;
	ASSERT_TRUE(series.on_tick(tick));

	const auto result = tt::api::enforce_series_symbol_policy(
		series, std::string_view{"BTCUSDT"});

	EXPECT_EQ(result.error,
	          tt::api::series_symbol_error::expected_symbol_mismatch);
	EXPECT_EQ(result.observed_symbol, "ETHUSDT");
	EXPECT_TRUE(series.bar_symbol_at(0).empty());
	EXPECT_EQ(series.tick_at(0).symbol, "ETHUSDT");
}

TEST(CApiSymbolPolicy, NamedMultiSymbolDataNeedsNoSyntheticIdentity)
{
	MarketSeries series;
	ASSERT_TRUE(series.load_into_queue(
		"1704067200000", "BTCUSDT", 100, 101, 99, 100, 1));
	ASSERT_TRUE(series.load_into_queue(
		"1704067260000", "ETHUSDT", 200, 201, 199, 200, 1));

	const auto result =
		tt::api::enforce_series_symbol_policy(series, std::nullopt);

	EXPECT_TRUE(result.success());
	EXPECT_EQ(series.bar_symbol_at(0), "BTCUSDT");
	EXPECT_EQ(series.bar_symbol_at(1), "ETHUSDT");
}

TEST(CApiSymbolPolicy, RejectsEmptyOrWhitespaceExpectedIdentityWithoutMutation)
{
	const std::string high_byte(1, static_cast<char>(0xff));
	for (const std::string_view invalid : {
		std::string_view{}, std::string_view{"BTC USDT"}, std::string_view{"\t"},
		std::string_view{high_byte}})
	{
		MarketSeries series;
		ASSERT_TRUE(series.load_into_queue(
			"1704067200000", "", 100, 101, 99, 100, 1));

		const auto result =
			tt::api::enforce_series_symbol_policy(series, invalid);

		EXPECT_EQ(result.error,
		          tt::api::series_symbol_error::invalid_expected_symbol);
		EXPECT_TRUE(series.bar_symbol_at(0).empty());
	}
}

TEST(MarketSeries, RejectsInvalidNonEmptySymbolsBeforeMutation)
{
	SilenceOutput quiet;
	const std::string invalid_symbol(1, static_cast<char>(0xff));
	MarketSeries series;
	EXPECT_FALSE(series.load_into_queue(
		"1704067200000", invalid_symbol, 100, 101, 99, 100, 1));
	EXPECT_EQ(series.bar_count(), 0u);

	tick_record tick;
	tick.timestamp = std::chrono::system_clock::time_point{
		std::chrono::milliseconds{1}};
	tick.symbol = invalid_symbol;
	tick.price = 100.0;
	tick.quantity = 1;
	tick.quantity_scale = 1;
	EXPECT_FALSE(series.add_tick(tick));
	EXPECT_EQ(series.tick_count(), 0u);
	EXPECT_EQ(series.validation_errors(), 2u);
}

TEST(MarketSeries, EnforcesInstrumentIdentityLengthBeforeMutation)
{
	SilenceOutput quiet;
	MarketSeries series;
	EXPECT_TRUE(series.load_into_queue(
		"1704067200000", std::string(256, 'A'), 100, 101, 99, 100, 1));
	EXPECT_FALSE(series.load_into_queue(
		"1704067260000", std::string(257, 'A'), 100, 101, 99, 100, 1));
	EXPECT_EQ(series.bar_count(), 1u);
	EXPECT_EQ(series.validation_errors(), 1u);
	EXPECT_FALSE(tt::api::valid_expected_symbol(std::string(257, 'A')));
}

TEST(MarketSeries, SymbolMutationApisRejectInvalidIdentityAtomically)
{
	SilenceOutput quiet;
	MarketSeries series;
	ASSERT_TRUE(series.load_into_queue(
		"1704067200000", "", 100, 101, 99, 100, 1));
	EXPECT_THROW(series.bind_unset_symbols("BTC\nUSDT"), std::invalid_argument);
	EXPECT_TRUE(series.bar_symbol_at(0).empty());

	ASSERT_EQ(series.bind_unset_symbols("BTCUSDT").bars, 1u);
	EXPECT_THROW(series.set_all_bar_symbols(std::string(257, 'X')),
	             std::invalid_argument);
	EXPECT_EQ(series.bar_symbol_at(0), "BTCUSDT");
}

TEST(MarketSeries, SortBarsByTimeMultiSymbol)
{
    MarketSeries series;
    series.load_into_queue("2024-01-02", "BBB", 1, 1, 1, 1, 1, 2);
    series.load_into_queue("2024-01-01", "AAA", 1, 1, 1, 1, 1, 3);
    series.load_into_queue("2024-01-01", "BBB", 1, 1, 1, 1, 1, 4);
    series.sort_bars_by_time();
    EXPECT_EQ(series.bar_at(0).symbol, "AAA");
    EXPECT_EQ(series.bar_at(0).quantity_scale, 3u);
    EXPECT_EQ(series.bar_at(1).symbol, "BBB");
    EXPECT_EQ(series.bar_at(1).quantity_scale, 4u);
    EXPECT_EQ(std::string(series.bar_at(2).date), "2024-01-02");
    EXPECT_EQ(series.bar_at(2).quantity_scale, 2u);
}

TEST(CsvDataSource, LoadsCorrectly)
{
    SilenceOutput quiet;
    auto dh = std::make_shared<data_handler>();
    CsvDataSource src(fixture_path("sample_ohlcv.csv"));
    ASSERT_TRUE(src.load_data(dh));
    EXPECT_EQ(dh->bar_count(), 2u);
    EXPECT_DOUBLE_EQ(dh->bar_at(0).close, 153.0);
    EXPECT_DOUBLE_EQ(dh->bar_at(1).close, 157.0);
}

TEST(CsvDataSource, LoadIntoSink)
{
    SilenceOutput quiet;
    MarketSeries series;
    CsvDataSource src(fixture_path("sample_ohlcv.csv"));
    LoadStats stats;
    ASSERT_TRUE(src.load_into(series, &stats));
    EXPECT_EQ(stats.accepted, 2u);
    EXPECT_EQ(series.bar_count(), 2u);
}

TEST(CsvDataSource, FileNotFound)
{
    SilenceOutput quiet;
    auto dh = std::make_shared<data_handler>();
    CsvDataSource src("/nonexistent/path.csv");
    EXPECT_FALSE(src.load_data(dh));
}

TEST(TickCsvDataSource, LoadsCorrectly)
{
    SilenceOutput quiet;
    auto dh = std::make_shared<data_handler>();
    TickCsvDataSource src(fixture_path("sample_ticks.csv"));
    ASSERT_TRUE(src.load_data(dh));
    EXPECT_EQ(dh->tick_count(), 3u);
    EXPECT_DOUBLE_EQ(dh->tick_at(0).price, 150.25);
    EXPECT_DOUBLE_EQ(dh->tick_at(1).price, 150.30);
    EXPECT_EQ(dh->tick_at(2).quantity, 200);
}

TEST(TickCsvDataSource, LoadIntoPreservesEmissionOrder)
{
    SilenceOutput quiet;
    MarketSeries series;
    TickCsvDataSource src(fixture_path("sample_ticks_ooo.csv"));
    ASSERT_TRUE(src.load_into(series));
    ASSERT_EQ(series.tick_count(), 3u);
    EXPECT_LT(series.tick_at(1).timestamp, series.tick_at(0).timestamp);
    EXPECT_LT(series.tick_at(1).timestamp, series.tick_at(2).timestamp);
}

TEST(TickCsvDataSource, LoadDataRetainsSortedTapeContract)
{
    SilenceOutput quiet;
    auto dh = std::make_shared<data_handler>();
    TickCsvDataSource src(fixture_path("sample_ticks_ooo.csv"));
    ASSERT_TRUE(src.load_data(dh));
    ASSERT_EQ(dh->tick_count(), 3u);
    EXPECT_LT(dh->tick_at(0).timestamp, dh->tick_at(1).timestamp);
    EXPECT_LT(dh->tick_at(1).timestamp, dh->tick_at(2).timestamp);
}

TEST(TickCsvDataSource, SideParsing)
{
    SilenceOutput quiet;
    auto dh = std::make_shared<data_handler>();
    TickCsvDataSource src(fixture_path("sample_ticks.csv"));
    ASSERT_TRUE(src.load_data(dh));
    EXPECT_EQ(dh->tick_at(0).side, data_tick_side::bid);
    EXPECT_EQ(dh->tick_at(1).side, data_tick_side::ask);
    EXPECT_EQ(dh->tick_at(2).side, data_tick_side::bid);
}

TEST(TickCsvDataSource, FileNotFound)
{
    SilenceOutput quiet;
    auto dh = std::make_shared<data_handler>();
    TickCsvDataSource src("/nonexistent/ticks.csv");
    EXPECT_FALSE(src.load_data(dh));
}

TEST(DataWrapper, FromPathLoadsCsv)
{
    SilenceOutput quiet;
    MarketSeries series;
    auto w = DataWrapper::from_path(fixture_path("sample_ohlcv.csv"));
    ASSERT_TRUE(w.load(series));
    EXPECT_EQ(series.bar_count(), 2u);
    EXPECT_DOUBLE_EQ(series.bar_at(0).close, 153.0);
}

TEST(DataWrapper, FromUriCsvScheme)
{
    SilenceOutput quiet;
    MarketSeries series;
    auto uri = std::string("csv:") + fixture_path("sample_ohlcv.csv");
    auto w = DataWrapper::from_uri(uri);
    ASSERT_TRUE(w.load(series));
    EXPECT_EQ(series.bar_count(), 2u);
}

TEST(DataWrapper, StrictRejectedRowPolicyRollsBackTheCompleteAppendedBatch)
{
    SilenceOutput quiet;
    MarketSeries series;
    ASSERT_TRUE(series.load_into_queue("1704067140000", "PREEXISTING", 90, 91, 89, 90, 1));
    DataLoadOptions options;
    options.fail_on_rejected_rows = true;
    auto wrapper = DataWrapper::from_source(std::make_unique<PartiallyRejectingSource>(), options);

    EXPECT_FALSE(wrapper.load(series));

    ASSERT_EQ(series.bar_count(), 1u);
    EXPECT_EQ(series.bar_symbol_at(0), "PREEXISTING");
    EXPECT_EQ(wrapper.last_load_stats().accepted, 1u);
    EXPECT_EQ(wrapper.last_load_stats().rejected, 1u);
    EXPECT_FALSE(wrapper.last_load_stats().message.empty());
}

TEST(DataWrapper, StrictPolicyObservesSinkRejectionEvenWhenSourceOmitsItFromStats)
{
    SilenceOutput quiet;
    MarketSeries series;
    DataLoadOptions options;
    options.fail_on_rejected_rows = true;
    auto wrapper =
        DataWrapper::from_source(std::make_unique<UnderReportingRejectingSource>(), options);

    EXPECT_FALSE(wrapper.load(series));
    EXPECT_TRUE(series.empty());
    EXPECT_EQ(series.validation_errors(), 0u);
    EXPECT_EQ(wrapper.last_load_stats().accepted, 1u);
    EXPECT_EQ(wrapper.last_load_stats().rejected, 1u);
}

TEST(DataWrapper, LegacySourceStatsArePerLoadDeltasOnReusedSeries)
{
    SilenceOutput quiet;
    MarketSeries series;
    Bar invalid;
    invalid.ts = std::chrono::system_clock::time_point{
        std::chrono::milliseconds{1}};
    invalid.symbol = "BAD";
    invalid.open = 1.0;
    invalid.high = std::numeric_limits<double>::quiet_NaN();
    invalid.low = 0.5;
    invalid.close = 1.0;
    invalid.volume = 1;
    ASSERT_FALSE(series.on_bar(invalid));
    ASSERT_EQ(series.validation_errors(), 1u);

    DataLoadOptions options;
    options.fail_on_rejected_rows = true;
    auto wrapper = DataWrapper::from_source(
        std::make_unique<LegacyAppendingSource>(), options);

    EXPECT_TRUE(wrapper.load(series));
    EXPECT_EQ(wrapper.last_load_stats().accepted, 1u);
    EXPECT_EQ(wrapper.last_load_stats().rejected, 0u);
    EXPECT_EQ(series.bar_count(), 1u);
    EXPECT_EQ(series.validation_errors(), 1u);
}

TEST(DataWrapper, ExceptionPathPreservesObservedSinkRejectionAndRollsBack)
{
    SilenceOutput quiet;
    MarketSeries series;
    ASSERT_TRUE(series.load_into_queue("1704067140000", "PREEXISTING", 90, 91, 89, 90, 1));
    auto wrapper = DataWrapper::from_source(std::make_unique<RejectingThenThrowingSource>());

    EXPECT_THROW(wrapper.load(series), std::runtime_error);
    ASSERT_EQ(series.bar_count(), 1u);
    EXPECT_EQ(series.bar_symbol_at(0), "PREEXISTING");
    EXPECT_EQ(series.validation_errors(), 0u);
    EXPECT_EQ(wrapper.last_load_stats().rejected, 1u);
    EXPECT_EQ(wrapper.last_load_stats().message, "source failed after sink rejection");
}

TEST(DataWrapper, StrictPartialMultiSourceCannotDiscardFailedPartRejections)
{
    SilenceOutput quiet;
    const auto invalid_path =
        std::filesystem::temp_directory_path() / "truetest_datawrapper_strict_partial_invalid.csv";
    {
        std::ofstream invalid(invalid_path);
        ASSERT_TRUE(invalid.good());
        invalid << "date,symbol,open,high,low,close,volume\n"
                   "1704067200000,BTCUSDT,junk,101,99,100,1\n";
    }

    DataLoadOptions options;
    options.allow_partial_sources = true;
    options.fail_on_rejected_rows = true;
    auto wrapper =
        DataWrapper::from_paths({fixture_path("sample_ohlcv.csv"), invalid_path}, options);
    MarketSeries series;
    const bool loaded = wrapper.load(series);
    std::error_code remove_error;
    std::filesystem::remove(invalid_path, remove_error);

    EXPECT_FALSE(loaded);
    EXPECT_TRUE(series.empty());
    EXPECT_EQ(wrapper.last_load_stats().accepted, 2u);
    EXPECT_EQ(wrapper.last_load_stats().rejected, 1u);
}

TEST(DataWrapper, ExplicitPartialMultiSourceKeepsOnlyValidPartAndReportsRejection)
{
    SilenceOutput quiet;
    const auto invalid_path =
        std::filesystem::temp_directory_path() / "truetest_datawrapper_partial_invalid.csv";
    {
        std::ofstream invalid(invalid_path);
        ASSERT_TRUE(invalid.good());
        invalid << "date,symbol,open,high,low,close,volume\n"
                   "1704067200000,BTCUSDT,junk,101,99,100,1\n";
    }

    DataLoadOptions options;
    options.allow_partial_sources = true;
    auto wrapper =
        DataWrapper::from_paths({fixture_path("sample_ohlcv.csv"), invalid_path}, options);
    MarketSeries series;
    const bool loaded = wrapper.load(series);
    std::error_code remove_error;
    std::filesystem::remove(invalid_path, remove_error);

    EXPECT_TRUE(loaded);
    EXPECT_EQ(series.bar_count(), 2u);
    EXPECT_EQ(wrapper.last_load_stats().accepted, 2u);
    EXPECT_EQ(wrapper.last_load_stats().rejected, 1u);
    EXPECT_FALSE(wrapper.last_load_stats().message.empty());
}

TEST(DataWrapper, DefaultPolicyKeepsAcceptedRowsForExplicitResearchUse)
{
    SilenceOutput quiet;
    MarketSeries series;
    auto wrapper = DataWrapper::from_source(std::make_unique<PartiallyRejectingSource>());

    EXPECT_TRUE(wrapper.load(series));
    ASSERT_EQ(series.bar_count(), 1u);
    EXPECT_EQ(series.bar_symbol_at(0), "BTCUSDT");
}

TEST(DataWrapper, PreservesTypedStreamFailure)
{
    MarketSeries series;
    auto wrapper = DataWrapper::from_source(std::make_unique<TypedFailureSource>());
    auto result = wrapper.stream(series);
    EXPECT_FALSE(result.success());
    EXPECT_EQ(result.termination, stream_termination::transport_failure);
    EXPECT_EQ(result.accepted, 3u);
    EXPECT_EQ(result.rejected, 1u);
}
