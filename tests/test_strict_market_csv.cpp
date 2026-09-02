#include <gtest/gtest.h>

#include "data/csv_data_source.h"
#include "data/tick_csv_data_source.h"
#include "helpers/alloc_counter.h"
#include "providers/local/csv_parser.h"
#include "types/quantity_scale.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

std::int64_t epoch_milliseconds(std::chrono::system_clock::time_point value)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(value.time_since_epoch()).count();
}

class RecordingMarketSink final : public IMarketSink
{
public:
    bool on_bar(const Bar& bar) override
    {
        bars.push_back(bar);
        return true;
    }

    bool on_tick(const Tick& tick) override
    {
        ticks.push_back(tick);
        return true;
    }

    std::vector<Bar> bars;
    std::vector<Tick> ticks;
};

class TemporaryCsv
{
public:
    explicit TemporaryCsv(std::string_view contents)
    {
        static std::uint64_t sequence = 0;
        path_ = std::filesystem::temp_directory_path() /
                ("truetest-strict-market-csv-" + std::to_string(++sequence) + ".csv");
        std::ofstream output(path_);
        output << contents;
        output.close();
    }

    ~TemporaryCsv()
    {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }

    TemporaryCsv(const TemporaryCsv&) = delete;
    TemporaryCsv& operator=(const TemporaryCsv&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

}  // namespace

TEST(StrictMarketCsvBar, HeaderRequiresUnambiguousSchemaAndTime)
{
    CsvBarParser parser;
    EXPECT_FALSE(parser.parse_header("date,symbol,open,high,low,close,volume,open"));
    EXPECT_FALSE(parser.parse_header("symbol,open,high,low,close,volume"));
    EXPECT_FALSE(parser.parse_header("date,symbol,open,high,low,close"));
    EXPECT_FALSE(parser.parse_header("date,symbol,open,high,low,close,volume,"));
    EXPECT_TRUE(parser.parse_header("date,symbol,open,high,low,close,volume"));
    EXPECT_TRUE(parser.parse_header("open_time,open,high,low,close,volume"));
}

TEST(StrictMarketCsvBar, RejectsInvalidPriceDomainAndGeometry)
{
    CsvBarParser parser;
    ASSERT_TRUE(parser.parse_header("date,symbol,open,high,low,close,volume"));

    for (const std::string row : {
             "2024-01-01,BTC,nan,101,99,100,1",
             "2024-01-01,BTC,100,inf,99,100,1",
             "2024-01-01,BTC,0,101,99,100,1",
             "2024-01-01,BTC,100,99,101,100,1",
             "2024-01-01,BTC,102,101,99,100,1",
             "2024-01-01,BTC,100,101,99,98,1",
         }) {
        EXPECT_FALSE(parser.parse_record(row)) << row;
    }
}

TEST(StrictMarketCsvBar, RejectsInvalidTimestampSymbolAndExactVolume)
{
    CsvBarParser parser;
    ASSERT_TRUE(parser.parse_header("date,open_time,symbol,open,high,low,close,volume"));

    for (const std::string row : {
             "2024-02-30,1704067200000,BTC,100,101,99,100,1",
             "2024-01-01,0,BTC,100,101,99,100,1",
             "2024-01-01,,BTC,100,101,99,100,1",
             "2024-01-01,9223372036854775807,BTC,100,101,99,100,1",
             "2024-01-02,1704067200000,BTC,100,101,99,100,1",
             "2024-01-01T00:01:00Z,1704067200000,BTC,100,101,99,100,1",
             "2024-01-01,1704067200000,,100,101,99,100,1",
             "2024-01-01,1704067200000,BTC,100,101,99,100,1.000000001",
             "2024-01-01,1704067200000,BTC,100,101,99,100,",
             "2024-01-01,1704067200000,BTC,100,101,99,100,0",
             "2024-01-01,1704067200000,BTC,100,101,99,100,0.0",
             "2024-01-01,1704067200000,BTC,100,101,99,100,-0",
         }) {
        EXPECT_FALSE(parser.parse_record(row)) << row;
    }
}

TEST(StrictMarketCsvBar, RejectsNonAsciiInstrumentIdentity)
{
    CsvBarParser parser;
    ASSERT_TRUE(parser.parse_header("date,symbol,open,high,low,close,volume"));
    std::string row = "2024-01-01,";
    row.push_back(static_cast<char>(0xff));
    row += ",100,101,99,100,1";
    EXPECT_FALSE(parser.parse_record(row));
}

TEST(StrictMarketCsvBar, EnforcesInstrumentIdentityLength)
{
    CsvBarParser parser;
    ASSERT_TRUE(parser.parse_header("date,symbol,open,high,low,close,volume"));
    const auto row = [](std::size_t length) {
        return "2024-01-01," + std::string(length, 'A') +
            ",100,101,99,100,1";
    };
    EXPECT_TRUE(parser.parse_record(row(256)));
    EXPECT_FALSE(parser.parse_record(row(257)));
}

TEST(StrictMarketCsvBar, AcceptsValidatedDateOrEpochAndExactDecimalVolume)
{
    CsvBarParser dated;
    ASSERT_TRUE(dated.parse_header("date,symbol,open,high,low,close,volume"));
    auto day = dated.parse_record("2024-01-01,BTC,100,101,99,100,0.12345678");
    ASSERT_TRUE(day);
    EXPECT_EQ(day->volume, 12'345'678);
    EXPECT_EQ(day->quantity_scale, 100'000'000u);

    CsvBarParser epoch;
    ASSERT_TRUE(epoch.parse_header("open_time,open,high,low,close,volume"));
    auto bar = epoch.parse_record("1704067200000,100,101,99,100,2");
    ASSERT_TRUE(bar);
    EXPECT_EQ(bar->open_time_ms, 1'704'067'200'000LL);
    EXPECT_EQ(bar->volume, 2);
    EXPECT_EQ(bar->quantity_scale, 1u);
}

TEST(StrictMarketCsvTick, HeaderSchemaControlsFieldOrder)
{
    CsvTickParser parser;
    ASSERT_TRUE(parser.parse_header("symbol,quantity,timestamp_ms,side,price"));
    EXPECT_FALSE(parser.header_frame_contains_records());

    auto tick = parser.parse_record("BTCUSDT,0.25,1704067200000,A,42000.5");
    ASSERT_TRUE(tick);
    EXPECT_EQ(tick->symbol, "BTCUSDT");
    EXPECT_EQ(tick->quantity, 25'000'000);
    EXPECT_EQ(tick->quantity_scale, 100'000'000u);
    EXPECT_EQ(tick->side, data_tick_side::ask);
    EXPECT_DOUBLE_EQ(tick->price, 42000.5);
    EXPECT_EQ(epoch_milliseconds(tick->timestamp), 1'704'067'200'000LL);
}

TEST(StrictMarketCsvTick, HeaderRejectsUnnamedAndDuplicateColumns)
{
    CsvTickParser parser;
    EXPECT_FALSE(parser.parse_header("timestamp_ms,symbol,price,quantity,side,"));
    EXPECT_FALSE(parser.parse_header("timestamp_ms,symbol,price,quantity,price"));
}

TEST(StrictMarketCsvTick, HeaderlessFirstFrameIsDataRatherThanDiscarded)
{
    CsvTickParser parser;
    const std::string first = "1704067200000,BTCUSDT,42000.5,2,B";
    ASSERT_TRUE(parser.parse_header(first));
    EXPECT_TRUE(parser.header_frame_contains_records());

    auto tick = parser.parse_record(first);
    ASSERT_TRUE(tick);
    EXPECT_EQ(tick->symbol, "BTCUSDT");
    EXPECT_EQ(tick->quantity, 2);
    EXPECT_EQ(tick->side, data_tick_side::bid);
}

TEST(StrictMarketCsvTick, HeaderlessInstrumentNamedLikeOneColumnIsStillData)
{
    CsvTickParser parser;
    const std::string first = "1704067200000,price,42000.5,2,B";
    ASSERT_TRUE(parser.parse_header(first));
    EXPECT_TRUE(parser.header_frame_contains_records());
    ASSERT_TRUE(parser.parse_record(first));
}

TEST(StrictMarketCsvTick, RejectsMalformedEconomicFields)
{
    CsvTickParser parser;
    ASSERT_TRUE(parser.parse_header("timestamp_ms,symbol,price,quantity,side"));

    for (const std::string row : {
             "0,BTCUSDT,42000,1,B",
             "-1,BTCUSDT,42000,1,B",
             "9223372036854775807,BTCUSDT,42000,1,B",
             "1704067200000,,42000,1,B",
             "1704067200000,BTCUSDT,nan,1,B",
             "1704067200000,BTCUSDT,inf,1,B",
             "1704067200000,BTCUSDT,0,1,B",
             "1704067200000,BTCUSDT,42000junk,1,B",
             "1704067200000,BTCUSDT,42000,0,B",
             "1704067200000,BTCUSDT,42000,1.000000001,B",
             "1704067200000,BTCUSDT,42000,1,BAD",
         }) {
        EXPECT_FALSE(parser.parse_record(row)) << row;
    }
}

TEST(StrictMarketCsvTick, RejectsNonAsciiInstrumentIdentity)
{
    CsvTickParser parser;
    ASSERT_TRUE(parser.parse_header("timestamp_ms,symbol,price,quantity,side"));
    std::string row = "1704067200000,";
    row.push_back(static_cast<char>(0xff));
    row += ",42000,1,B";
    EXPECT_FALSE(parser.parse_record(row));
}

TEST(StrictMarketCsvNumeric, DecimalAtomsAreExactAtEverySupportedPrecision)
{
    struct Example
    {
        std::string_view token;
        std::int64_t atoms;
        std::uint64_t scale;
    };
    for (const Example example : {
             Example{"0", 0, 1},
             Example{"2", 2, 1},
             Example{"0.1", 10'000'000, 100'000'000},
             Example{"0.01", 1'000'000, 100'000'000},
             Example{"0.001", 100'000, 100'000'000},
             Example{"0.0001", 10'000, 100'000'000},
             Example{"0.00001", 1'000, 100'000'000},
             Example{"0.000001", 100, 100'000'000},
             Example{"0.0000001", 10, 100'000'000},
             Example{"0.00000001", 1, 100'000'000},
             Example{"92233720368.54775807", std::numeric_limits<std::int64_t>::max(), 100'000'000},
         }) {
        std::int64_t atoms = -1;
        std::uint64_t scale = 0;
        ASSERT_TRUE(tt::strict_market_csv::parse_decimal_atoms(example.token, true, atoms, scale))
            << example.token;
        EXPECT_EQ(atoms, example.atoms) << example.token;
        EXPECT_EQ(scale, example.scale) << example.token;
    }
}

TEST(StrictMarketCsvNumeric, DecimalGrammarRejectsRoundingAndOverflow)
{
    for (const std::string_view token : {
             "",
             "+1",
             "-0",
             "-1",
             ".1",
             "1.",
             "1e2",
             "1junk",
             "1.000000001",
             "92233720368.54775808",
             "9223372036854775808",
         }) {
        std::int64_t atoms = 0;
        std::uint64_t scale = 0;
        EXPECT_FALSE(tt::strict_market_csv::parse_decimal_atoms(token, true, atoms, scale))
            << token;
    }
}

TEST(QuantityScale, VenueDecimalAtomsUseExactCanonicalScale)
{
    struct Example
    {
        std::string_view token;
        std::int64_t atoms;
    };
    for (const Example example : {
             Example{"0", 0},
             Example{"0.0", 0},
             Example{"1", 100'000'000},
             Example{"0.1", 10'000'000},
             Example{"0.00000001", 1},
             Example{"1.000000000", 100'000'000},
             Example{"92233720368.54775807",
                     std::numeric_limits<std::int64_t>::max()},
         }) {
        const auto parsed =
            tt::quantity_scale::parse_decimal_canonical_atoms(example.token);
        ASSERT_TRUE(parsed.has_value()) << example.token;
        EXPECT_EQ(*parsed, example.atoms) << example.token;
    }
}

TEST(QuantityScale, VenueDecimalAtomsRejectAmbiguousRoundingAndOverflow)
{
    for (const std::string_view token : {
             "", "+1", "-0", "-1", " 1", "1 ", ".1", "1.",
             "1e-8", "nan", "inf", "0.000000001", "0.123456789",
             "92233720368.54775808", "92233720369", "18446744073709551615",
         }) {
        EXPECT_FALSE(
            tt::quantity_scale::parse_decimal_canonical_atoms(token).has_value())
            << token;
    }
}

TEST(QuantityScale, VenueDecimalAtomsRoundTripSampledAtomIdentities)
{
    for (const std::int64_t atoms : {
             std::int64_t{0}, std::int64_t{1}, std::int64_t{7},
             std::int64_t{99'999'999}, std::int64_t{100'000'000},
             std::int64_t{9'007'199'254'740'993},
             std::numeric_limits<std::int64_t>::max(),
         }) {
        const auto whole = atoms / 100'000'000;
        const auto fraction = atoms % 100'000'000;
        std::string fractional = std::to_string(100'000'000 + fraction);
        const std::string token =
            std::to_string(whole) + "." + fractional.substr(1);
        const auto parsed =
            tt::quantity_scale::parse_decimal_canonical_atoms(token);
        ASSERT_TRUE(parsed.has_value()) << token;
        EXPECT_EQ(*parsed, atoms) << token;
    }
}

TEST(QuantityScale, VenueDecimalAtomParsingDoesNotAllocate)
{
    bool valid = true;
    std::uint64_t checksum = 0;
    truetest::test::alloc::measure_window measured;
    for (std::size_t iteration = 0; iteration < 100'000; ++iteration)
    {
        const auto parsed = tt::quantity_scale::parse_decimal_canonical_atoms(
            "90071992.54740993");
        valid = parsed.has_value() && valid;
        checksum ^= static_cast<std::uint64_t>(parsed.value_or(0));
    }
    const auto allocations = measured.total();

    EXPECT_TRUE(valid);
    EXPECT_EQ(checksum, 0u);
    EXPECT_EQ(allocations.count, 0u);
    EXPECT_EQ(allocations.bytes, 0u);
}

TEST(QuantityScale, RescalingNeverRoundsPositiveEconomicQuantity)
{
    std::uint64_t out = 99;
    EXPECT_FALSE(tt::quantity_scale::rescale_nonnegative(
        1, 100'000'000, 1'000'000.0, out));
    EXPECT_EQ(out, 0u);
    EXPECT_FALSE(tt::quantity_scale::rescale_nonnegative(
        50, 100'000'000, 1'000'000.0, out));
    EXPECT_EQ(out, 0u);
    EXPECT_TRUE(tt::quantity_scale::rescale_nonnegative(
        100, 100'000'000, 1'000'000.0, out));
    EXPECT_EQ(out, 1u);
    EXPECT_TRUE(tt::quantity_scale::rescale_nonnegative(
        1, 1, 100'000'000.0, out));
    EXPECT_EQ(out, 100'000'000u);
    EXPECT_FALSE(tt::quantity_scale::rescale_nonnegative(
        1, 1, 1.5, out));
    EXPECT_EQ(out, 0u);
    EXPECT_FALSE(tt::quantity_scale::rescale_nonnegative(
        1, 1, std::ldexp(1.0, 64), out));
    EXPECT_EQ(out, 0u);
}

TEST(QuantityScale, BaseConversionRejectsTheExactInt64ExclusiveBound)
{
    std::int64_t out = 7;
    EXPECT_FALSE(tt::quantity_scale::from_base_nonnegative(0x1p63, 1, out));
    EXPECT_EQ(out, 0);

    const double largest_binary64_below_bound = std::nextafter(0x1p63, 0.0);
    ASSERT_TRUE(tt::quantity_scale::from_base_nonnegative(
        largest_binary64_below_bound, 1, out));
    EXPECT_EQ(out, 9'223'372'036'854'774'784LL);
}

TEST(StrictMarketCsvNumeric, ValidatedRowParsingDoesNotAllocate)
{
    tt::strict_market_csv::bar_schema bars;
    ASSERT_TRUE(bars.parse_header("date,open_time,symbol,open,high,low,close,volume"));
    tt::strict_market_csv::tick_schema ticks;
    ASSERT_TRUE(ticks.parse_header_or_first_row("timestamp_ms,symbol,price,quantity,side"));

    bool valid = true;
    std::int64_t checksum = 0;
    truetest::test::alloc::measure_window measured;
    for (std::size_t iteration = 0; iteration < 100'000; ++iteration) {
        tt::strict_market_csv::parsed_bar bar;
        tt::strict_market_csv::parsed_tick tick;
        valid = static_cast<bool>(
                    bars.parse_row("2024-01-01,1704067200000,BTC,100,101,99,100,0.25", bar)) &&
                valid;
        valid = static_cast<bool>(ticks.parse_row("1704067200000,BTC,100.5,0.25,B", tick)) && valid;
        checksum += bar.volume + tick.quantity;
    }
    const auto allocations = measured.total();

    EXPECT_TRUE(valid);
    EXPECT_EQ(checksum, 5'000'000'000'000LL);
    EXPECT_EQ(allocations.count, 0u);
    EXPECT_EQ(allocations.bytes, 0u);
}

TEST(StrictMarketCsvParity, DirectBarSourceUsesTheSameFailClosedGrammar)
{
    TemporaryCsv csv("date,symbol,open,high,low,close,volume\n"
                     "2024-01-01,BTC,100,101,99,100,0.25\n"
                     "2024-01-02,BTC,100junk,101,99,100,1\n"
                     "2024-01-03,BTC,102,101,99,100,1\n"
                     "2024-01-04,BTC,100,101,99,100,1.000000001\n");
    CsvDataSource source(csv.path());
    MarketSeries sink;
    LoadStats stats;

    EXPECT_FALSE(source.load_into(sink, &stats));
    EXPECT_EQ(sink.bar_count(), 0u);
    EXPECT_EQ(stats.accepted, 0u);
    EXPECT_EQ(stats.rejected, 3u);
}

TEST(StrictMarketCsvParity, DirectTickSourceRejectsMalformedBatchTransactionally)
{
    TemporaryCsv csv("1704067200000,BTCUSDT,42000.5,2,B\n"
                     "1704067200001,BTCUSDT,42001,1,BAD\n"
                     "1704067200002,BTCUSDT,42002,0.25,A\n");
    TickCsvDataSource source(csv.path().string());
    MarketSeries sink;
    LoadStats stats;

    EXPECT_FALSE(source.load_into(sink, &stats));
    EXPECT_EQ(sink.tick_count(), 0u);
    EXPECT_EQ(stats.accepted, 0u);
    EXPECT_EQ(stats.rejected, 1u);
}

TEST(StrictMarketCsvParity, NonTransactionalSinkIsRejectedBeforeMutation)
{
    class ThrowingSink final : public IMarketSink
    {
    public:
        bool on_bar(const Bar&) override
        {
            ++calls;
            throw std::runtime_error("sink failed");
        }
        bool on_tick(const Tick&) override { return false; }
        int calls = 0;
    };

    TemporaryCsv csv("date,symbol,open,high,low,close,volume\n"
                     "2024-01-01,BTC,100,101,99,100,1\n"
                     "2024-01-02,BTC,100,101,99,100,1\n");
    CsvDataSource source(csv.path());
    ThrowingSink sink;
    LoadStats stats;
    EXPECT_FALSE(source.load_into(sink, &stats));
    EXPECT_EQ(sink.calls, 0);
    EXPECT_EQ(stats.accepted, 0u);
    EXPECT_NE(stats.message.find("transactional"), std::string::npos);
}

TEST(StrictMarketCsvParity, FailedLoadResetsReusedStatistics)
{
    RecordingMarketSink sink;
    LoadStats stats{7, 9, "stale"};

    TemporaryCsv invalid_bar("date,symbol,open,high,low,close,volume,open\n");
    CsvDataSource bars(invalid_bar.path());
    EXPECT_FALSE(bars.load_into(sink, &stats));
    EXPECT_EQ(stats.accepted, 0u);
    EXPECT_EQ(stats.rejected, 0u);
    EXPECT_NE(stats.message, "stale");

    stats = {4, 6, "stale"};
    TemporaryCsv invalid_tick("not,a,valid,row\n");
    TickCsvDataSource ticks(invalid_tick.path().string());
    EXPECT_FALSE(ticks.load_into(sink, &stats));
    EXPECT_EQ(stats.accepted, 0u);
    EXPECT_EQ(stats.rejected, 0u);
    EXPECT_NE(stats.message, "stale");
}
