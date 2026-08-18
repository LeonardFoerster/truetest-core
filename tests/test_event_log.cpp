#include <gtest/gtest.h>
#include "core/event_log.h"
#include "engine/engine.h"
#include "engine/engine_config.h"
#include "data/data_handler.h"
#include "orderbook/orderbook.h"
#include "market_maker/market_maker.h"
#include "types/order_id.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

static auto epoch_ms(int64_t ms)
{
    return std::chrono::system_clock::time_point(std::chrono::milliseconds(ms));
}

// RAII temp file that deletes on destruction.
// Paths must be process-unique: fixed /tmp names race when ASan/TSan (or
// parallel ctest) suites run concurrently and both touch EventLog tests.
struct TempFile {
    std::string path;
    explicit TempFile(const std::string& name)
    {
        static std::atomic<uint64_t> seq{0};
        path = "/tmp/truetest_test_" + std::to_string(static_cast<unsigned long>(::getpid()))
             + "_" + std::to_string(seq.fetch_add(1, std::memory_order_relaxed))
             + "_" + name;
    }
    ~TempFile() { std::remove(path.c_str()); }
};

// RAII helper to silence cout.
// Anonymous namespace prevents ODR clashes with identically-named helpers in
// other test TUs.
namespace {
struct SilenceCout {
    std::ostringstream sink;
    std::streambuf* orig;
    SilenceCout() : sink(), orig(std::cout.rdbuf(sink.rdbuf())) {}
    ~SilenceCout() { std::cout.rdbuf(orig); }
};

void write_raw_event_record(std::ostream& out,
                            event_type type,
                            const std::vector<uint8_t>& payload)
{
    event_serial::write_u8(out, static_cast<uint8_t>(type));
    event_serial::write_u32(
        out, event_serial::checked_u32_length(payload.size(), "test payload"));
    out.write(reinterpret_cast<const char*>(payload.data()),
              static_cast<std::streamsize>(payload.size()));
}

void write_event_log_preamble(std::ostream& out,
                              bool compressed,
                              bool finalized = false)
{
    out.write(reinterpret_cast<const char*>(EVENT_LOG_FILE_MAGIC.data()),
              static_cast<std::streamsize>(EVENT_LOG_FILE_MAGIC.size()));
    event_serial::write_u8(out, EVENT_LOG_FILE_VERSION);
    uint8_t flags = compressed ? EVENT_LOG_FILE_FLAG_ZSTD : uint8_t{0};
    if (finalized)
        flags |= EVENT_LOG_FILE_FLAG_FINALIZED;
    event_serial::write_u8(out, flags);
}
}

// ─── Round-trip tests for each event type ───────────────────────────────────

TEST(EventLog, RoundTrip_MarketEvent)
{
    TempFile tf("market.bin");
    auto ts = epoch_ms(1000000);

    {
        EventLogger logger(tf.path);
        market_event mkt(ts, "AAPL", 150.25, 152.00, 149.50, 151.75,
                         1000000, 100'000'000ULL);
        logger.log(mkt);
        logger.flush();
    }

    EventReplayer replayer(tf.path);
    ASSERT_TRUE(replayer.has_next());
    auto ev = replayer.next();
    ASSERT_NE(ev, nullptr);
    ASSERT_EQ(ev->get_type(), event_type::market);

    auto& mkt = static_cast<market_event&>(*ev);
    EXPECT_EQ(mkt.get_timestamp(), ts);
    EXPECT_EQ(mkt.get_symbol(), "AAPL");
    EXPECT_DOUBLE_EQ(mkt.get_open(), 150.25);
    EXPECT_DOUBLE_EQ(mkt.get_high(), 152.00);
    EXPECT_DOUBLE_EQ(mkt.get_low(), 149.50);
    EXPECT_DOUBLE_EQ(mkt.get_close(), 151.75);
    EXPECT_EQ(mkt.get_volume(), 1000000);
    EXPECT_EQ(mkt.get_quantity_scale(), 100'000'000ULL);

    EXPECT_FALSE(replayer.has_next());
}

TEST(EventLog, RoundTrip_SignalEvent)
{
    TempFile tf("signal.bin");
    auto ts = epoch_ms(2000000);

    {
        EventLogger logger(tf.path);
        signal_event sig(ts, "BTC", signal_type::buy, 0.85);
        logger.log(sig);
        logger.flush();
    }

    EventReplayer replayer(tf.path);
    auto ev = replayer.next();
    ASSERT_NE(ev, nullptr);
    ASSERT_EQ(ev->get_type(), event_type::signal);

    auto& sig = static_cast<signal_event&>(*ev);
    EXPECT_EQ(sig.get_symbol(), "BTC");
    EXPECT_EQ(sig.get_signal(), signal_type::buy);
    EXPECT_DOUBLE_EQ(sig.get_strength(), 0.85);
}

TEST(EventLog, RoundTrip_OrderEvent)
{
    TempFile tf("order.bin");
    auto ts = epoch_ms(3000000);
    auto elig_ts = epoch_ms(3000500);

    {
        EventLogger logger(tf.path);
        order_event ord(ts, "ETH", order_type::limit, order_side::sell, 50, 2500.0,
                        time_in_force::gtc, 0.0);
        ord.set_order_id(12345);
        ord.set_earliest_eligible_ts(elig_ts);
        ord.set_opener_order_id(77);
        ord.set_strategy_name("breakout");
        logger.log(ord);
        logger.flush();
    }

    EventReplayer replayer(tf.path);
    auto ev = replayer.next();
    ASSERT_NE(ev, nullptr);
    ASSERT_EQ(ev->get_type(), event_type::order);

    auto& ord = static_cast<order_event&>(*ev);
    EXPECT_EQ(ord.get_symbol(), "ETH");
    EXPECT_EQ(ord.get_order_type(), order_type::limit);
    EXPECT_EQ(ord.get_side(), order_side::sell);
    EXPECT_EQ(ord.get_quantity(), 50);
    EXPECT_DOUBLE_EQ(ord.get_price(), 2500.0);
    EXPECT_EQ(ord.get_tif(), time_in_force::gtc);
    EXPECT_EQ(ord.get_order_id(), 12345u);
    EXPECT_EQ(ord.get_earliest_eligible_ts(), elig_ts);
    EXPECT_EQ(ord.get_opener_order_id(), 77u);
    EXPECT_EQ(ord.get_strategy_name(), "breakout");
}

TEST(EventLog, RoundTrip_FillEvent)
{
    TempFile tf("fill.bin");
    auto ts = epoch_ms(4000000);

    {
        EventLogger logger(tf.path);
        fill_event fill(ts, "MSFT", 99, order_side::buy, 100, 310.50, 1.25,
                        /*remaining_qty=*/0.0, /*fill_id=*/7,
                        /*strategy_name=*/"mean_reversion",
                        /*opener_order_id=*/42);
        fill.set_source(fill_source::exchange);
        logger.log(fill);
        logger.flush();
    }

    EventReplayer replayer(tf.path);
    auto ev = replayer.next();
    ASSERT_NE(ev, nullptr);
    ASSERT_EQ(ev->get_type(), event_type::fill);

    auto& fill = static_cast<fill_event&>(*ev);
    EXPECT_EQ(fill.get_symbol(), "MSFT");
    EXPECT_EQ(fill.get_order_id(), 99u);
    EXPECT_EQ(fill.get_side(), order_side::buy);
    EXPECT_EQ(fill.get_filled_quantity(), 100);
    EXPECT_DOUBLE_EQ(fill.get_fill_price(), 310.50);
    EXPECT_DOUBLE_EQ(fill.get_commission(), 1.25);
    EXPECT_EQ(fill.get_fill_id(), 7u);
    EXPECT_EQ(fill.get_source(), fill_source::exchange);
    EXPECT_EQ(fill.get_opener_order_id(), 42u);
    EXPECT_EQ(fill.get_strategy_name(), "mean_reversion");
}

TEST(EventLog, RoundTrip_TickEvent)
{
    TempFile tf("tick.bin");
    auto ts = epoch_ms(5000000);

    {
        EventLogger logger(tf.path);
        tick_event tick(ts, "SPY", 450.25, 500, tick_side::bid,
                        100'000'000ULL);
        logger.log(tick);
        logger.flush();
    }

    EventReplayer replayer(tf.path);
    auto ev = replayer.next();
    ASSERT_NE(ev, nullptr);
    ASSERT_EQ(ev->get_type(), event_type::tick);

    auto& tick = static_cast<tick_event&>(*ev);
    EXPECT_EQ(tick.get_symbol(), "SPY");
    EXPECT_DOUBLE_EQ(tick.get_price(), 450.25);
    EXPECT_EQ(tick.get_quantity(), 500);
    EXPECT_EQ(tick.get_side(), tick_side::bid);
    EXPECT_EQ(tick.get_quantity_scale(), 100'000'000ULL);
}

TEST(EventLog, RoundTrip_L2SnapshotEvent)
{
    TempFile tf("l2snap.bin");
    auto ts = epoch_ms(6000000);

    {
        EventLogger logger(tf.path);
        std::vector<l2_level> bids = {{100.5, 200}, {100.0, 300}};
        std::vector<l2_level> asks = {{101.0, 150}, {101.5, 250}};
        l2_snapshot_event snap(ts, "DOGE",
                               bids.data(), bids.size(),
                               asks.data(), asks.size(), 100'000'000ULL);
        logger.log(snap);
        logger.flush();
    }

    EventReplayer replayer(tf.path);
    auto ev = replayer.next();
    ASSERT_NE(ev, nullptr);
    ASSERT_EQ(ev->get_type(), event_type::l2_snapshot);

    auto& snap = static_cast<l2_snapshot_event&>(*ev);
    EXPECT_EQ(snap.get_symbol(), "DOGE");
    ASSERT_EQ(snap.bid_count(), 2u);
    EXPECT_DOUBLE_EQ(snap.bid(0).price, 100.5);
    EXPECT_EQ(snap.bid(0).quantity, 200);
    EXPECT_DOUBLE_EQ(snap.bid(1).price, 100.0);
    ASSERT_EQ(snap.ask_count(), 2u);
    EXPECT_DOUBLE_EQ(snap.ask(0).price, 101.0);
    EXPECT_EQ(snap.ask(0).quantity, 150);
    EXPECT_EQ(snap.get_quantity_scale(), 100'000'000ULL);
}

TEST(EventLog, RoundTrip_L2UpdateEvent)
{
    TempFile tf("l2upd.bin");
    auto ts = epoch_ms(7000000);

    {
        EventLogger logger(tf.path);
        l2_update_event upd(ts, "SOL", tick_side::ask, 25.50, 1000,
                            100'000'000ULL);
        logger.log(upd);
        logger.flush();
    }

    EventReplayer replayer(tf.path);
    auto ev = replayer.next();
    ASSERT_NE(ev, nullptr);
    ASSERT_EQ(ev->get_type(), event_type::l2_update);

    auto& upd = static_cast<l2_update_event&>(*ev);
    EXPECT_EQ(upd.get_symbol(), "SOL");
    EXPECT_EQ(upd.get_side(), tick_side::ask);
    EXPECT_DOUBLE_EQ(upd.get_price(), 25.50);
    EXPECT_EQ(upd.get_new_quantity(), 1000);
    EXPECT_EQ(upd.get_quantity_scale(), 100'000'000ULL);
}

TEST(EventLog, RoundTrip_FundingEvent)
{
    TempFile tf("funding.bin");
    const auto ts = epoch_ms(7'500'000);
    {
        EventLogger logger(tf.path);
        logger.log(funding_event(ts, "BTCUSDT", 0.0, -1.25,
                                 "FUNDING_FEE"));
    }

    EventReplayer replayer(tf.path);
    auto ev = replayer.next();
    ASSERT_NE(ev, nullptr);
    ASSERT_EQ(ev->get_type(), event_type::funding);
    const auto& funding = static_cast<const funding_event&>(*ev);
    EXPECT_EQ(funding.get_timestamp(), ts);
    EXPECT_EQ(funding.get_symbol(), "BTCUSDT");
    EXPECT_DOUBLE_EQ(funding.get_cash_delta(), -1.25);
    EXPECT_EQ(funding.get_reason(), "FUNDING_FEE");
}

// ─── Multiple events in sequence ────────────────────────────────────────────

TEST(EventLog, MultipleEvents_RoundTrip)
{
    TempFile tf("multi.bin");
    auto ts1 = epoch_ms(1000);
    auto ts2 = epoch_ms(2000);
    auto ts3 = epoch_ms(3000);

    {
        EventLogger logger(tf.path);
        logger.log(market_event(ts1, "X", 10, 11, 9, 10.5, 100));
        logger.log(tick_event(ts2, "X", 10.5, 50, tick_side::ask));
        logger.log(fill_event(ts3, "X", 1, order_side::buy, 10, 10.5, 0.0));
        logger.flush();
    }

    EventReplayer replayer(tf.path);

    auto e1 = replayer.next();
    ASSERT_NE(e1, nullptr);
    EXPECT_EQ(e1->get_type(), event_type::market);

    auto e2 = replayer.next();
    ASSERT_NE(e2, nullptr);
    EXPECT_EQ(e2->get_type(), event_type::tick);

    auto e3 = replayer.next();
    ASSERT_NE(e3, nullptr);
    EXPECT_EQ(e3->get_type(), event_type::fill);

    EXPECT_FALSE(replayer.has_next());
}

// ─── Empty log ──────────────────────────────────────────────────────────────

TEST(EventLog, EmptyLog_NoEvents)
{
    TempFile tf("empty.bin");

    {
        EventLogger logger(tf.path);
        logger.flush();
    }

    EventReplayer replayer(tf.path);
    EXPECT_FALSE(replayer.has_next());
    EXPECT_EQ(replayer.next(), nullptr);
}

// ─── Bad path throws ────────────────────────────────────────────────────────

TEST(EventLog, BadPath_Throws)
{
    EXPECT_THROW(EventLogger("/nonexistent/dir/file.bin"), std::runtime_error);
    EXPECT_THROW(EventReplayer("/nonexistent/dir/file.bin"), std::runtime_error);
}

TEST(EventLog, LoggerRejectsWritesAfterFinalize)
{
    TempFile tf("logger_finalized.bin");
    EventLogger logger(tf.path);
    logger.log(market_event(epoch_ms(1), "TEST", 1, 2, 0.5, 1.5, 10));
    logger.finalize();
    logger.finalize();

    EXPECT_THROW(
        logger.log(market_event(epoch_ms(2), "TEST", 1, 2, 0.5, 1.5, 10)),
        std::logic_error);

    EventReplayer replayer(tf.path);
    const auto replayed = replayer.next();
    ASSERT_NE(replayed, nullptr);
    EXPECT_EQ(replayed->get_type(), event_type::market);
    EXPECT_FALSE(replayer.has_next());
}

TEST(EventLog, LoggerRejectsUnknownEventType)
{
    TempFile tf("logger_unknown_type.bin");
    EventLogger logger(tf.path);
    const event unknown(static_cast<event_type>(0xFF), epoch_ms(1));

    EXPECT_THROW(logger.log(unknown), std::runtime_error);
}

TEST(EventLog, LoggerRejectsTagDynamicTypeMismatchWithoutPoisoning)
{
    TempFile tf("logger_type_mismatch.bin");
    EventLogger logger(tf.path, false);
    const event mismatched(event_type::market, epoch_ms(1));
    EXPECT_THROW(logger.log(mismatched), std::runtime_error);

    logger.log(market_event(epoch_ms(2), "TEST", 1, 2, 0.5, 1.5, 10));
    logger.finalize();

    EventReplayer replayer(tf.path);
    const auto replayed = replayer.next();
    ASSERT_NE(replayed, nullptr);
    EXPECT_EQ(replayed->get_timestamp(), epoch_ms(2));
    EXPECT_FALSE(replayer.has_next());
}

#if defined(__linux__)
TEST(EventLog, FinalizeFailurePoisonsLoggerWithoutTerminatingDestructor)
{
    ASSERT_EXIT(
        {
            bool first_finalize_failed = false;
            bool repeated_finalize_failed = false;
            bool subsequent_log_failed = false;
            {
                EventLogger logger("/dev/full", false);
                try {
                    logger.finalize();
                } catch (const std::runtime_error&) {
                    first_finalize_failed = true;
                }
                try {
                    logger.finalize();
                } catch (const std::runtime_error&) {
                    repeated_finalize_failed = true;
                }
                try {
                    logger.log(market_event(epoch_ms(1), "TEST", 1, 2, 0.5, 1.5, 10));
                } catch (const std::runtime_error&) {
                    subsequent_log_failed = true;
                }
            }
            ::_exit(first_finalize_failed && repeated_finalize_failed && subsequent_log_failed
                        ? 0
                        : 1);
        },
        ::testing::ExitedWithCode(0), "");
}

TEST(EventLog, ThreadedDurableFinalizeFailureHaltsEngine)
{
    SilenceCout quiet;
    auto dh = std::make_shared<data_handler>();
    dh->load_into_queue("1704067200000", "TEST",
                        100.0, 101.0, 99.0, 100.0, 1000);

    engine_config cfg;
    cfg.threading = thread_preset::standard;
    cfg.disable_pinning = true;
    cfg.show_progress = false;
    cfg.event_log_path = "/dev/full";
    cfg.compress_log = false;
    engine eng(dh, std::make_shared<orderbook>(), nullptr, std::move(cfg));

    eng.run();

    EXPECT_TRUE(eng.is_halted());
    EXPECT_FALSE(eng.run_succeeded());
}

TEST(EventLog, InlineDurableWriteOrFinalizeFailureHaltsEngine)
{
    SilenceCout quiet;
    auto dh = std::make_shared<data_handler>();
    dh->load_into_queue("1704067200000", "TEST",
                        100.0, 101.0, 99.0, 100.0, 1000);

    engine_config cfg;
    cfg.threading = thread_preset::inline_mode;
    cfg.show_progress = false;
    cfg.event_log_path = "/dev/full";
    cfg.compress_log = false;
    engine eng(dh, std::make_shared<orderbook>(), nullptr, std::move(cfg));

    EXPECT_NO_THROW(eng.run());
    EXPECT_TRUE(eng.is_halted());
    EXPECT_FALSE(eng.run_succeeded());
}
#endif

TEST(EventLog, InlineEngineHonorsConfiguredRotation)
{
    SilenceCout quiet;
    TempFile tf("inline_rotation.bin");
    const std::string rotated = tf.path + ".1";
    auto dh = std::make_shared<data_handler>();
    for (int i = 0; i < 3; ++i)
        dh->load_into_queue(std::to_string(1704067200000LL + i * 60'000LL),
                            "TEST", 100.0, 101.0, 99.0, 100.0, 1000);

    {
        engine_config cfg;
        cfg.threading = thread_preset::inline_mode;
        cfg.show_progress = false;
        cfg.event_log_path = tf.path;
        cfg.compress_log = false;
        cfg.log_max_bytes = 1;
        cfg.log_max_files = 1;
        engine eng(dh, std::make_shared<orderbook>(), nullptr, std::move(cfg));
        eng.run();
    }

    std::ifstream rotated_log(rotated, std::ios::binary);
    EXPECT_TRUE(rotated_log.good());
    rotated_log.close();
    std::remove(rotated.c_str());
}

TEST(EventLog, AuthoritativeReplayRefusesUnfinalizedPrefix)
{
    SilenceCout quiet;
    TempFile tf("unfinalized-prefix.bin");
    {
        std::ofstream out(tf.path, std::ios::binary | std::ios::trunc);
        write_event_log_preamble(out, /*compressed=*/false,
                                 /*finalized=*/false);
        const auto payload = event_serial::serialise(
            market_event(epoch_ms(1), "TEST", 1, 1, 1, 1, 1));
        write_raw_event_record(out, event_type::market, payload);
    }

    EventReplayer inspection(tf.path);
    EXPECT_FALSE(inspection.file_finalized());
    EXPECT_NE(inspection.next(), nullptr);

    engine_config cfg;
    cfg.threading = thread_preset::inline_mode;
    engine eng(std::make_shared<data_handler>(), nullptr, nullptr,
               std::move(cfg));
    EXPECT_THROW(eng.run_replay(tf.path), std::runtime_error);
}

TEST(EventLog, AuthoritativeReplayRefusesEveryRotatedSegment)
{
    SilenceCout quiet;
    TempFile tf("segmented-ledger.bin");
    const std::string rotated = tf.path + ".1";
    {
        EventLogger logger(tf.path, /*compress=*/false,
                           /*max_bytes=*/1, /*max_files=*/1);
        logger.log(market_event(epoch_ms(1), "TEST", 1, 1, 1, 1, 1));
    }

    EventReplayer current(tf.path);
    EventReplayer previous(rotated);
    EXPECT_TRUE(current.file_finalized());
    EXPECT_TRUE(previous.file_finalized());
    EXPECT_TRUE(current.file_segmented());
    EXPECT_TRUE(previous.file_segmented());

    engine_config cfg1;
    cfg1.threading = thread_preset::inline_mode;
    engine current_engine(std::make_shared<data_handler>(), nullptr, nullptr,
                          std::move(cfg1));
    EXPECT_THROW(current_engine.run_replay(tf.path), std::runtime_error);

    engine_config cfg2;
    cfg2.threading = thread_preset::inline_mode;
    engine previous_engine(std::make_shared<data_handler>(), nullptr, nullptr,
                           std::move(cfg2));
    EXPECT_THROW(previous_engine.run_replay(rotated), std::runtime_error);
    std::remove(rotated.c_str());
}

TEST(EventLog, RotationFailurePreservesFinalizedBaseAndPoisonsLogger)
{
    TempFile tf("rotation-failure.bin");
    const std::string blocked_target = tf.path + ".1";
    const std::string child = blocked_target + "/keep";
    ASSERT_TRUE(std::filesystem::create_directory(blocked_target));
    {
        std::ofstream marker(child);
        marker << "occupied";
    }

    {
        EventLogger logger(tf.path, /*compress=*/false,
                           /*max_bytes=*/1, /*max_files=*/1);
        EXPECT_THROW(logger.log(market_event(
            epoch_ms(1), "TEST", 1, 1, 1, 1, 1)), std::runtime_error);
        EXPECT_THROW(logger.flush(), std::runtime_error);
    }

    EventReplayer preserved(tf.path);
    EXPECT_TRUE(preserved.file_finalized());
    EXPECT_TRUE(preserved.file_segmented());
    EXPECT_NE(preserved.next(), nullptr);

    std::filesystem::remove(child);
    std::filesystem::remove(blocked_target);
}

TEST(EventLog, HeaderedRawPayloadStartingWithZstdMagicReplaysRaw)
{
    TempFile tf("headered_raw_zstd_magic.bin");
    constexpr int64_t zstd_magic_timestamp_us = 0xFD2FB528LL;
    const auto timestamp = std::chrono::system_clock::time_point(
        std::chrono::microseconds(zstd_magic_timestamp_us));
    {
        EventLogger logger(tf.path, false);
        logger.log(market_event(timestamp, "TEST", 1, 2, 0.5, 1.5, 10));
    }

    EventReplayer replayer(tf.path, zstd_magic_timestamp_us);
    const auto replayed = replayer.next();
    ASSERT_NE(replayed, nullptr);
    EXPECT_EQ(replayed->get_timestamp(), timestamp);
    EXPECT_FALSE(replayer.has_next());
}

TEST(EventLog, HeaderedCompressedLogDeclaresCompression)
{
    TempFile tf("headered_compressed.bin");
    {
        EventLogger logger(tf.path, true);
        logger.log(market_event(epoch_ms(1), "TEST", 1, 2, 0.5, 1.5, 10));
    }

    std::array<uint8_t, EVENT_LOG_FILE_PREAMBLE_BYTES> preamble{};
    {
        std::ifstream in(tf.path, std::ios::binary);
        in.read(reinterpret_cast<char*>(preamble.data()),
                static_cast<std::streamsize>(preamble.size()));
        ASSERT_EQ(in.gcount(), static_cast<std::streamsize>(preamble.size()));
    }
    EXPECT_TRUE(std::equal(EVENT_LOG_FILE_MAGIC.begin(), EVENT_LOG_FILE_MAGIC.end(),
                           preamble.begin()));
    EXPECT_EQ(preamble[4], EVENT_LOG_FILE_VERSION);
    EXPECT_EQ(preamble[5],
              EVENT_LOG_FILE_FLAG_ZSTD | EVENT_LOG_FILE_FLAG_FINALIZED);

    EventReplayer replayer(tf.path);
    EXPECT_NE(replayer.next(), nullptr);
}

TEST(EventLog, HeaderlessRawAndCompressedLogsRemainReplayable)
{
    const auto decoded = event_serial::serialise(
        market_event(epoch_ms(1), "TEST", 1, 2, 0.5, 1.5, 10));

    TempFile raw_tf("legacy_raw.bin");
    {
        std::ofstream out(raw_tf.path, std::ios::binary | std::ios::trunc);
        write_raw_event_record(out, event_type::market, decoded);
    }
    EventReplayer raw_replayer(raw_tf.path);
    EXPECT_NE(raw_replayer.next(), nullptr);

    TempFile compressed_tf("legacy_compressed.bin");
    std::vector<uint8_t> compressed(ZSTD_compressBound(decoded.size()));
    const auto compressed_size = ZSTD_compress(
        compressed.data(), compressed.size(), decoded.data(), decoded.size(), 1);
    ASSERT_FALSE(ZSTD_isError(compressed_size));
    compressed.resize(compressed_size);
    {
        std::ofstream out(compressed_tf.path, std::ios::binary | std::ios::trunc);
        write_raw_event_record(out, event_type::market, compressed);
    }
    EventReplayer compressed_replayer(compressed_tf.path);
    EXPECT_NE(compressed_replayer.next(), nullptr);
}

TEST(EventLog, InvalidHeaderPreambleIsRejectedBeforeReplay)
{
    TempFile truncated_tf("truncated_preamble.bin");
    {
        std::ofstream out(truncated_tf.path, std::ios::binary | std::ios::trunc);
        event_serial::write_u8(out, EVENT_LOG_FILE_MAGIC.front());
    }
    EXPECT_THROW(EventReplayer(truncated_tf.path), std::runtime_error);

    TempFile version_tf("unknown_preamble_version.bin");
    {
        std::ofstream out(version_tf.path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(EVENT_LOG_FILE_MAGIC.data()),
                  static_cast<std::streamsize>(EVENT_LOG_FILE_MAGIC.size()));
        event_serial::write_u8(out, EVENT_LOG_FILE_VERSION + 1U);
        event_serial::write_u8(out, 0U);
    }
    EXPECT_THROW(EventReplayer(version_tf.path), std::runtime_error);

    TempFile index_tf("preamble_index_before_data.bin");
    {
        std::ofstream out(index_tf.path, std::ios::binary | std::ios::trunc);
        write_event_log_preamble(out, false, true);
        event_serial::write_u64(out, 0U);
        event_serial::write_u32(out, 0U);
        event_serial::write_u32(out, EVENT_LOG_INDEX_MAGIC);
    }
    EXPECT_THROW(EventReplayer(index_tf.path), std::runtime_error);
}

TEST(EventLog, UnfinalizedHeaderNeverTreatsRecordTailAsIndexTrailer)
{
    TempFile tf("unfinalized_tail_looks_like_index.bin");
    auto payload = event_serial::serialise(
        market_event(epoch_ms(1), "TEST", 1, 2, 0.5, 1.5, 10));
    ASSERT_GE(payload.size(), 16U);

    const uint64_t apparent_index_offset =
        static_cast<uint64_t>(EVENT_LOG_FILE_PREAMBLE_BYTES + 5U +
                              payload.size() - 16U);
    const uint32_t zero_entries = 0;
    std::memcpy(payload.data() + payload.size() - 16U,
                &apparent_index_offset, sizeof(apparent_index_offset));
    std::memcpy(payload.data() + payload.size() - 8U,
                &zero_entries, sizeof(zero_entries));
    std::memcpy(payload.data() + payload.size() - 4U,
                &EVENT_LOG_INDEX_MAGIC, sizeof(EVENT_LOG_INDEX_MAGIC));

    {
        std::ofstream out(tf.path, std::ios::binary | std::ios::trunc);
        write_event_log_preamble(out, false);
        write_raw_event_record(out, event_type::market, payload);
    }

    EventReplayer replayer(tf.path);
    EXPECT_NE(replayer.next(), nullptr);
    EXPECT_FALSE(replayer.has_next());
}

TEST(EventLog, FinalizedHeaderRequiresAValidIndexTrailer)
{
    TempFile tf("finalized_without_index.bin");
    {
        std::ofstream out(tf.path, std::ios::binary | std::ios::trunc);
        write_event_log_preamble(out, false, true);
    }
    EXPECT_THROW(EventReplayer(tf.path), std::runtime_error);
}

TEST(EventLog, FinalizedIndexIsValidatedForDefaultReplay)
{
    TempFile tf("default_replay_invalid_index_boundary.bin");
    const auto payload = event_serial::serialise(
        market_event(epoch_ms(1), "TEST", 1, 2, 0.5, 1.5, 10));
    const uint64_t record_offset =
        static_cast<uint64_t>(EVENT_LOG_FILE_PREAMBLE_BYTES);
    const uint64_t index_offset = record_offset + 5U + payload.size();

    {
        std::ofstream out(tf.path, std::ios::binary | std::ios::trunc);
        write_event_log_preamble(out, false, true);
        write_raw_event_record(out, event_type::market, payload);
        event_serial::write_i64(out, 0);
        event_serial::write_u64(out, record_offset + 1U);
        event_serial::write_u64(out, index_offset);
        event_serial::write_u32(out, 1U);
        event_serial::write_u32(out, EVENT_LOG_INDEX_MAGIC);
    }

    EXPECT_THROW(EventReplayer(tf.path), std::runtime_error);
}

// ─── Defensive parsing and allocation limits ───

// Malformed inputs stay tiny: rejection must happen before allocation.
TEST(EventLog, OversizedPayloadHeaderIsRejectedBeforeAllocation)
{
    TempFile tf("oversized_payload.bin");
    {
        std::ofstream out(tf.path, std::ios::binary | std::ios::trunc);
        event_serial::write_u8(out, static_cast<uint8_t>(event_type::market));
        event_serial::write_u32(out, std::numeric_limits<uint32_t>::max());
    }

    EventReplayer replayer(tf.path);
    EXPECT_THROW((void)replayer.next(), std::runtime_error);
}

TEST(EventLog, TruncatedHeaderIsNotReportedAsCleanEof)
{
    TempFile tf("truncated_header.bin");
    {
        std::ofstream out(tf.path, std::ios::binary | std::ios::trunc);
        event_serial::write_u32(out, 0U);
    }

    EventReplayer replayer(tf.path);
    ASSERT_TRUE(replayer.has_next());
    EXPECT_THROW((void)replayer.next(), std::runtime_error);
}

TEST(EventLog, TruncatedPayloadIsRejectedBeforeAllocation)
{
    TempFile tf("truncated_payload.bin");
    {
        std::ofstream out(tf.path, std::ios::binary | std::ios::trunc);
        event_serial::write_u8(out, static_cast<uint8_t>(event_type::market));
        event_serial::write_u32(out, 8U);
        event_serial::write_u8(out, 0U);
    }

    EventReplayer replayer(tf.path);
    EXPECT_THROW((void)replayer.next(), std::runtime_error);
}

TEST(EventLog, EmptyRawPayloadIsRejectedWithoutPointerArithmetic)
{
    TempFile tf("empty_raw_payload.bin");
    {
        std::ofstream out(tf.path, std::ios::binary | std::ios::trunc);
        event_serial::write_u8(out, static_cast<uint8_t>(event_type::market));
        event_serial::write_u32(out, 0U);
    }

    EventReplayer replayer(tf.path);
    EXPECT_THROW((void)replayer.next(), std::runtime_error);
}

TEST(EventLog, EmptyDecodedZstdPayloadIsRejected)
{
    TempFile tf("empty_zstd_payload.bin");
    std::vector<uint8_t> compressed(ZSTD_compressBound(0));
    const uint8_t empty_source = 0;
    const auto compressed_size = ZSTD_compress(
        compressed.data(), compressed.size(), &empty_source, 0, 1);
    ASSERT_FALSE(ZSTD_isError(compressed_size));
    compressed.resize(compressed_size);
    ASSERT_EQ(ZSTD_getFrameContentSize(compressed.data(), compressed.size()), 0U);

    {
        std::ofstream out(tf.path, std::ios::binary | std::ios::trunc);
        event_serial::write_u8(out, static_cast<uint8_t>(event_type::market));
        event_serial::write_u32(
            out, event_serial::checked_u32_length(compressed.size(),
                                                  "test payload"));
        out.write(reinterpret_cast<const char*>(compressed.data()),
                  static_cast<std::streamsize>(compressed.size()));
    }

    EventReplayer replayer(tf.path);
    EXPECT_THROW((void)replayer.next(), std::runtime_error);
}

TEST(EventLog, MultipleZstdFramesWithinOneRecordAreRejected)
{
    TempFile tf("multiple_zstd_frames.bin");
    const auto decoded = event_serial::serialise(
        market_event(epoch_ms(1), "TEST", 1, 2, 0.5, 1.5, 10));

    std::vector<uint8_t> payload(ZSTD_compressBound(decoded.size()));
    const auto payload_size = ZSTD_compress(
        payload.data(), payload.size(), decoded.data(), decoded.size(), 1);
    ASSERT_FALSE(ZSTD_isError(payload_size));
    payload.resize(payload_size);

    const uint8_t empty_source = 0;
    std::vector<uint8_t> trailing_frame(ZSTD_compressBound(0));
    const auto trailing_size = ZSTD_compress(
        trailing_frame.data(), trailing_frame.size(), &empty_source, 0, 1);
    ASSERT_FALSE(ZSTD_isError(trailing_size));
    trailing_frame.resize(trailing_size);
    payload.insert(payload.end(), trailing_frame.begin(), trailing_frame.end());

    {
        std::ofstream out(tf.path, std::ios::binary | std::ios::trunc);
        write_raw_event_record(out, event_type::market, payload);
    }

    EventReplayer replayer(tf.path);
    EXPECT_THROW((void)replayer.next(), std::runtime_error);
}

TEST(EventLog, DeserialiseRejectsTrailingPayloadBytes)
{
    auto payload = event_serial::serialise(
        market_event(epoch_ms(1), "TEST", 1, 2, 0.5, 1.5, 10));
    payload.push_back(0xA5);

    EXPECT_THROW(
        (void)event_serial::deserialise(event_type::market,
                                        payload.data(), payload.size()),
        std::runtime_error);
}

TEST(EventLog, FillExtensionAcceptsOnlyCompleteHistoricShapes)
{
    fill_event fill(epoch_ms(1), "TEST", 42, order_side::buy,
                    2.0, 100.0, 0.5, 3.0, 99, "alpha", 7);
    fill.set_source(fill_source::exchange);
    const auto current = event_serial::serialise(fill);
    const std::size_t attribution_size = sizeof(uint64_t) + sizeof(uint16_t)
                                       + fill.get_strategy_name().size();
    ASSERT_GE(current.size(), 17u + attribution_size);
    const auto v1_complete_size = current.size() - attribution_size;
    const auto base_size = v1_complete_size - 17U;

    auto legacy = current;
    legacy.resize(base_size);
    const auto legacy_event = event_serial::deserialise(
        event_type::fill, legacy.data(), legacy.size());
    ASSERT_NE(legacy_event, nullptr);
    const auto& legacy_fill = static_cast<const fill_event&>(*legacy_event);
    EXPECT_DOUBLE_EQ(legacy_fill.get_remaining_qty(), 0.0);
    EXPECT_EQ(legacy_fill.get_fill_id(), 0U);
    EXPECT_EQ(legacy_fill.get_source(), fill_source::unknown);

    auto prior_extension = current;
    prior_extension.resize(base_size + 16U);
    const auto prior_event = event_serial::deserialise(
        event_type::fill, prior_extension.data(), prior_extension.size());
    ASSERT_NE(prior_event, nullptr);
    const auto& prior_fill = static_cast<const fill_event&>(*prior_event);
    EXPECT_DOUBLE_EQ(prior_fill.get_remaining_qty(), 3.0);
    EXPECT_EQ(prior_fill.get_fill_id(), 99U);
    EXPECT_EQ(prior_fill.get_source(), fill_source::unknown);

    auto v1_complete = current;
    v1_complete.resize(v1_complete_size);
    const auto v1_event = event_serial::deserialise(
        event_type::fill, v1_complete.data(), v1_complete.size());
    ASSERT_NE(v1_event, nullptr);
    const auto& v1_fill = static_cast<const fill_event&>(*v1_event);
    EXPECT_DOUBLE_EQ(v1_fill.get_remaining_qty(), 3.0);
    EXPECT_EQ(v1_fill.get_fill_id(), 99U);
    EXPECT_EQ(v1_fill.get_source(), fill_source::exchange);
    EXPECT_EQ(v1_fill.get_opener_order_id(), 0U);
    EXPECT_TRUE(v1_fill.get_strategy_name().empty());

    const auto current_event = event_serial::deserialise(
        event_type::fill, current.data(), current.size());
    ASSERT_NE(current_event, nullptr);
    const auto& current_fill = static_cast<const fill_event&>(*current_event);
    EXPECT_DOUBLE_EQ(current_fill.get_remaining_qty(), 3.0);
    EXPECT_EQ(current_fill.get_fill_id(), 99U);
    EXPECT_EQ(current_fill.get_source(), fill_source::exchange);
    EXPECT_EQ(current_fill.get_opener_order_id(), 7U);
    EXPECT_EQ(current_fill.get_strategy_name(), "alpha");

    for (std::size_t tail_size = 1; tail_size < 16; ++tail_size) {
        auto partial = current;
        partial.resize(base_size + tail_size);
        EXPECT_THROW(
            (void)event_serial::deserialise(event_type::fill,
                                            partial.data(), partial.size()),
            std::runtime_error);
    }

    for (std::size_t tail_size = 18; tail_size < 27; ++tail_size) {
        auto partial = current;
        partial.resize(base_size + tail_size);
        EXPECT_THROW(
            (void)event_serial::deserialise(event_type::fill,
                                            partial.data(), partial.size()),
            std::runtime_error);
    }

    auto extra_extension = current;
    extra_extension.push_back(0x42);
    EXPECT_THROW(
        (void)event_serial::deserialise(event_type::fill,
                                        extra_extension.data(), extra_extension.size()),
        std::runtime_error);
}

TEST(EventLog, OrderAttributionExtensionRemainsBackwardCompatible)
{
    order_event order(epoch_ms(1), "TEST", order_type::limit,
                      order_side::sell, 2.0, 100.0);
    order.set_order_id(12);
    order.set_opener_order_id(7);
    order.set_strategy_name("alpha");
    auto current = event_serial::serialise(order);
    const std::size_t attribution_size = sizeof(uint64_t) + sizeof(uint16_t)
                                       + order.get_strategy_name().size();
    ASSERT_GT(current.size(), attribution_size);

    auto legacy = current;
    legacy.resize(legacy.size() - attribution_size);
    const auto legacy_event = event_serial::deserialise(
        event_type::order, legacy.data(), legacy.size());
    ASSERT_NE(legacy_event, nullptr);
    const auto& legacy_order = static_cast<const order_event&>(*legacy_event);
    EXPECT_EQ(legacy_order.get_opener_order_id(), 0U);
    EXPECT_TRUE(legacy_order.get_strategy_name().empty());

    for (std::size_t partial_size = 1; partial_size < 10; ++partial_size) {
        auto partial = legacy;
        partial.insert(partial.end(), partial_size, 0);
        EXPECT_THROW(
            (void)event_serial::deserialise(event_type::order,
                                            partial.data(), partial.size()),
            std::runtime_error);
    }
}

TEST(EventLog, DeserialiseRejectsOutOfRangeEnumBytes)
{
    constexpr std::size_t prefix_size = sizeof(int64_t) + sizeof(uint16_t) + 1U;
    const auto expect_rejected = [](event_type type,
                                    std::vector<uint8_t> payload,
                                    std::size_t offset) {
        ASSERT_LT(offset, payload.size());
        payload[offset] = 0xFF;
        EXPECT_THROW(
            (void)event_serial::deserialise(type, payload.data(), payload.size()),
            std::runtime_error);
    };

    expect_rejected(
        event_type::signal,
        event_serial::serialise(signal_event(epoch_ms(1), "X", signal_type::buy, 1.0)),
        prefix_size);

    const auto order = order_event(epoch_ms(1), "X", order_type::limit,
                                   order_side::buy, 1.0, 10.0,
                                   time_in_force::gtc);
    expect_rejected(event_type::order, event_serial::serialise(order), prefix_size);
    expect_rejected(event_type::order, event_serial::serialise(order), prefix_size + 1U);
    expect_rejected(event_type::order, event_serial::serialise(order), prefix_size + 18U);

    const auto fill = fill_event(epoch_ms(1), "X", 1, order_side::buy,
                                 1.0, 10.0, 0.0, 0.0, 1);
    expect_rejected(event_type::fill, event_serial::serialise(fill), prefix_size + 8U);
    constexpr std::size_t fill_source_offset =
        prefix_size + sizeof(uint64_t) + sizeof(uint8_t)
                    + 4U * sizeof(double) + sizeof(uint64_t);
    expect_rejected(event_type::fill, event_serial::serialise(fill),
                    fill_source_offset);

    const auto tick = tick_event(epoch_ms(1), "X", 10.0, 1, tick_side::bid);
    constexpr std::size_t tick_side_offset =
        prefix_size + sizeof(double) + sizeof(int64_t);
    expect_rejected(event_type::tick, event_serial::serialise(tick),
                    tick_side_offset);

    const auto update = l2_update_event(epoch_ms(1), "X", tick_side::bid, 10.0, 1);
    expect_rejected(event_type::l2_update, event_serial::serialise(update), prefix_size);
}

TEST(EventLog, ReplayFailureIsTerminal)
{
    TempFile tf("terminal_replay_failure.bin");
    auto corrupt = event_serial::serialise(
        market_event(epoch_ms(1), "BAD", 1, 2, 0.5, 1.5, 10));
    corrupt.push_back(0xA5);
    const auto valid = event_serial::serialise(
        market_event(epoch_ms(2), "GOOD", 1, 2, 0.5, 1.5, 10));

    {
        std::ofstream out(tf.path, std::ios::binary | std::ios::trunc);
        write_raw_event_record(out, event_type::market, corrupt);
        write_raw_event_record(out, event_type::market, valid);
    }

    EventReplayer replayer(tf.path);
    ASSERT_TRUE(replayer.has_next());
    EXPECT_THROW((void)replayer.next(), std::runtime_error);
    EXPECT_FALSE(replayer.has_next());
    EXPECT_THROW((void)replayer.next(), std::runtime_error);
}

TEST(EventLog, ImpossibleIndexTrailerIsRejectedBeforeAllocation)
{
    TempFile tf("bad_index_count.bin");
    {
        std::ofstream out(tf.path, std::ios::binary | std::ios::trunc);
        event_serial::write_u64(out, 0U);
        event_serial::write_u32(out, std::numeric_limits<uint32_t>::max());
        event_serial::write_u32(out, EVENT_LOG_INDEX_MAGIC);
    }

    EXPECT_THROW(EventReplayer(tf.path), std::runtime_error);
}

TEST(EventLog, ConfiguredIndexScanLimitIsEnforced)
{
    TempFile tf("index_limit.bin");
    {
        std::ofstream out(tf.path, std::ios::binary | std::ios::trunc);
        event_serial::write_u8(out, 0U); // one byte of data before the index
        event_serial::write_i64(out, 0);
        event_serial::write_u64(out, 0U);
        event_serial::write_i64(out, 1);
        event_serial::write_u64(out, 0U);
        event_serial::write_u64(out, 1U);
        event_serial::write_u32(out, 2U);
        event_serial::write_u32(out, EVENT_LOG_INDEX_MAGIC);
    }

    EventReplayLimits limits;
    limits.max_index_entries = 1;
    EXPECT_THROW(EventReplayer(tf.path, 1, INT64_MAX, limits),
                 std::runtime_error);
}

TEST(EventLog, InvalidIndexedFileOffsetIsRejected)
{
    TempFile tf("bad_index_offset.bin");
    {
        std::ofstream out(tf.path, std::ios::binary | std::ios::trunc);
        event_serial::write_u8(out, 0U); // data_end is one; offset one is invalid
        event_serial::write_i64(out, 0);
        event_serial::write_u64(out, 1U);
        event_serial::write_u64(out, 1U);
        event_serial::write_u32(out, 1U);
        event_serial::write_u32(out, EVENT_LOG_INDEX_MAGIC);
    }

    EXPECT_THROW(EventReplayer(tf.path, 1), std::runtime_error);
}

TEST(EventLog, DecodedPayloadLimitIsEnforcedBeforeAllocation)
{
    TempFile tf("decoded_limit.bin");
    {
        EventLogger logger(tf.path);
        logger.log(market_event(epoch_ms(1), "TEST", 1, 2, 0.5, 1.5, 10));
    }

    EventReplayLimits limits;
    limits.max_decoded_payload_bytes = 8;
    EventReplayer replayer(tf.path, 0, INT64_MAX, limits);
    EXPECT_THROW((void)replayer.next(), std::runtime_error);
}

TEST(EventLog, ZstdFrameWithoutContentSizeIsRejected)
{
    TempFile tf("unknown_zstd_size.bin");
    const auto payload = event_serial::serialise(
        market_event(epoch_ms(1), "TEST", 1, 2, 0.5, 1.5, 10));

    std::unique_ptr<ZSTD_CCtx, decltype(&ZSTD_freeCCtx)> cctx(
        ZSTD_createCCtx(), &ZSTD_freeCCtx);
    ASSERT_NE(cctx, nullptr);
    const auto parameter_result =
        ZSTD_CCtx_setParameter(cctx.get(), ZSTD_c_contentSizeFlag, 0);
    ASSERT_FALSE(ZSTD_isError(parameter_result));

    std::vector<uint8_t> compressed(ZSTD_compressBound(payload.size()));
    const auto compressed_size = ZSTD_compress2(
        cctx.get(), compressed.data(), compressed.size(),
        payload.data(), payload.size());
    ASSERT_FALSE(ZSTD_isError(compressed_size));
    compressed.resize(compressed_size);
    ASSERT_EQ(ZSTD_getFrameContentSize(compressed.data(), compressed.size()),
              ZSTD_CONTENTSIZE_UNKNOWN);

    {
        std::ofstream out(tf.path, std::ios::binary | std::ios::trunc);
        event_serial::write_u8(out, static_cast<uint8_t>(event_type::market));
        event_serial::write_u32(
            out, event_serial::checked_u32_length(compressed.size(),
                                                  "test payload"));
        out.write(reinterpret_cast<const char*>(compressed.data()),
                  static_cast<std::streamsize>(compressed.size()));
    }

    EventReplayer replayer(tf.path);
    EXPECT_THROW((void)replayer.next(), std::runtime_error);
}

TEST(EventLog, OversizedStringFieldIsRejectedInsteadOfTruncated)
{
    const std::string oversized(
        static_cast<std::size_t>(std::numeric_limits<uint16_t>::max()) + 1U,
        'x');
    const cancel_event cancel(epoch_ms(1), "TEST", 1U, oversized);
    EXPECT_THROW((void)event_serial::serialise(cancel), std::length_error);
}

TEST(EventLog, FailedRecordDoesNotCorruptEmptyLogIndex)
{
    TempFile tf("failed_record_empty_log.bin");
    const std::string oversized(
        static_cast<std::size_t>(std::numeric_limits<uint16_t>::max()) + 1U,
        'x');
    {
        EventLogger logger(tf.path);
        const cancel_event cancel(epoch_ms(1), "TEST", 1U, oversized);
        EXPECT_THROW(logger.log(cancel), std::length_error);
    }

    EventReplayer replayer(tf.path, 1);
    EXPECT_FALSE(replayer.has_next());
    EXPECT_EQ(replayer.next(), nullptr);
}

TEST(EventLog, FailedRecordDoesNotPreventFollowingValidRecord)
{
    TempFile tf("failed_then_valid.bin");
    const std::string oversized(
        static_cast<std::size_t>(std::numeric_limits<uint16_t>::max()) + 1U,
        'x');
    {
        EventLogger logger(tf.path);
        const cancel_event cancel(epoch_ms(1), "TEST", 1U, oversized);
        EXPECT_THROW(logger.log(cancel), std::length_error);
        logger.log(market_event(epoch_ms(2), "TEST", 1, 2, 0.5, 1.5, 10));
    }

    EventReplayer replayer(tf.path, 2'000);
    const auto replayed = replayer.next();
    ASSERT_NE(replayed, nullptr);
    EXPECT_EQ(replayed->get_type(), event_type::market);
    EXPECT_FALSE(replayer.has_next());
}

TEST(EventLog, IndexedReplayStartsAtOrBeforeRequestedTimestamp)
{
    TempFile tf("indexed_seek.bin");
    {
        EventLogger logger(tf.path);
        for (int64_t i = 0; i <= 2000; ++i) {
            logger.log(market_event(epoch_ms(i), "TEST", 1, 2, 0.5, 1.5, 10));
        }
    }

    constexpr int64_t replay_from_us = 1'500'000;
    EventReplayer replayer(tf.path, replay_from_us);
    const auto replayed_event = replayer.next();
    ASSERT_NE(replayed_event, nullptr);
    const auto replayed_us = std::chrono::duration_cast<std::chrono::microseconds>(
        replayed_event->get_timestamp().time_since_epoch()).count();
    EXPECT_EQ(replayed_us, replay_from_us);
}

// ─── Deterministic mode ───

// Test strategy: buys on bar 3, sells on bar 6.
class DetTestStrategy : public IStrategy
{
    bool position_open_ = false;
    int call_count_ = 0;
public:
    int call_count() const { return call_count_; }

    std::optional<order_event> on_market(const market_event& mkt) override
    {
        call_count_++;
        if (call_count_ == 3 && !position_open_)
        {
            return order_event(mkt.get_timestamp(), mkt.get_symbol(),
                               order_type::market, order_side::buy, 10, mkt.get_close());
        }
        if (call_count_ == 6 && position_open_)
        {
            return order_event(mkt.get_timestamp(), mkt.get_symbol(),
                               order_type::market, order_side::sell, 10, mkt.get_close());
        }
        return std::nullopt;
    }
    void set_position_open(const std::string&, bool open) override { position_open_ = open; }
};

static std::shared_ptr<data_handler> make_det_data(int n)
{
    auto dh = std::make_shared<data_handler>();
    for (int i = 0; i < n; ++i)
        dh->load_into_queue("2024-01-01", "TEST",
                            100.0 + i, 105.0 + i, 95.0 + i, 102.0 + i, 1000);
    return dh;
}

static std::vector<uint8_t> read_file_bytes(const std::string& path)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    auto size = f.tellg();
    f.seekg(0);
    std::vector<uint8_t> buf(static_cast<std::size_t>(size));
    f.read(reinterpret_cast<char*>(buf.data()), size);
    return buf;
}

TEST(EventLog, Deterministic_SameSeedSameLog)
{
    SilenceCout quiet;
    TempFile tf1("det1.bin");
    TempFile tf2("det2.bin");

    auto run_with_seed = [](uint64_t seed, const std::string& log_path) {
        OrderIdGenerator::reset(1);
        auto dh = make_det_data(20);
        auto strat = std::make_shared<DetTestStrategy>();

        engine_config cfg;
        cfg.seed = seed;
        cfg.event_log_path = log_path;

        engine eng(dh, nullptr, strat, std::move(cfg));
        eng.set_primary_strategy_name("deterministic");
        eng.run();
    };

    run_with_seed(42, tf1.path);
    run_with_seed(42, tf2.path);

    auto bytes1 = read_file_bytes(tf1.path);
    auto bytes2 = read_file_bytes(tf2.path);

    ASSERT_FALSE(bytes1.empty()) << "log path: " << tf1.path;
    ASSERT_EQ(bytes1.size(), bytes2.size())
        << "same seed must yield equal log sizes; paths=" << tf1.path
        << " vs " << tf2.path;
    EXPECT_EQ(bytes1, bytes2)
        << "same seed must yield byte-identical event logs; paths="
        << tf1.path << " vs " << tf2.path;
}

TEST(EventLog, Deterministic_DifferentSeedDifferentLog)
{
    SilenceCout quiet;
    TempFile tf1("diffseed1.bin");
    TempFile tf2("diffseed2.bin");

    auto run_with_seed = [](uint64_t seed, const std::string& log_path) {
        auto dh = make_det_data(20);
        auto strat = std::make_shared<DetTestStrategy>();

        engine_config cfg;
        cfg.seed = seed;
        cfg.event_log_path = log_path;

        engine eng(dh, nullptr, strat, std::move(cfg));
        eng.run();
    };

    run_with_seed(42, tf1.path);
    run_with_seed(99, tf2.path);

    auto bytes1 = read_file_bytes(tf1.path);
    auto bytes2 = read_file_bytes(tf2.path);

    // Logs should differ because market maker RNG produces different book state
    ASSERT_FALSE(bytes1.empty());
    ASSERT_FALSE(bytes2.empty());
    // The market events themselves are identical (same data), but fill events
    // differ because the orderbook is seeded differently
    EXPECT_NE(bytes1, bytes2);
}

// ─── Replay produces identical analytics ────────────────────────────────────

TEST(EventLog, Replay_ProducesIdenticalAnalytics)
{
    SilenceCout quiet;
    TempFile tf("replay.bin");

    AnalyticsReport original;
    double original_funding = 0.0;
    {
        OrderIdGenerator::reset(1);
        auto dh = make_det_data(20);
        auto strat = std::make_shared<DetTestStrategy>();

        engine_config cfg;
        cfg.seed = 42;
        cfg.event_log_path = tf.path;

        engine eng(dh, nullptr, strat, std::move(cfg));
        eng.set_primary_strategy_name("deterministic");
        eng.run();
        original = eng.get_analytics().generate_report();
        original_funding = eng.get_analytics().total_funding_pnl();
    }

    AnalyticsReport replayed;
    double replayed_funding = 0.0;
    auto replay_strategy = std::make_shared<DetTestStrategy>();
    {
        auto dh = std::make_shared<data_handler>();

        engine_config cfg;
        cfg.seed = 9999; // ledger replay must not depend on simulation RNG.
        cfg.initial_balance = original.initial_equity;
        cfg.threading = thread_preset::inline_mode;

        engine eng(dh, nullptr, replay_strategy, std::move(cfg));
        eng.run_replay(tf.path);
        replayed = eng.get_analytics().generate_report();
        replayed_funding = eng.get_analytics().total_funding_pnl();
    }

    EXPECT_EQ(replay_strategy->call_count(), 0)
        << "ledger replay must never regenerate strategy decisions";
    EXPECT_EQ(replayed.total_orders, original.total_orders);
    EXPECT_EQ(replayed.total_fills, original.total_fills);
    EXPECT_EQ(replayed.total_trades, original.total_trades);
    EXPECT_EQ(replayed.trades.size(), original.trades.size());
    EXPECT_NEAR(replayed.final_equity, original.final_equity, 1e-9);
    EXPECT_NEAR(replayed.realized_pnl, original.realized_pnl, 1e-9);
    EXPECT_NEAR(replayed.unrealized_pnl, original.unrealized_pnl, 1e-9);
    EXPECT_NEAR(replayed.max_drawdown, original.max_drawdown, 1e-12);
    EXPECT_NEAR(replayed_funding, original_funding, 1e-12);
    ASSERT_FALSE(replayed.trades.empty());
    EXPECT_EQ(replayed.trades.front().strategy_name, "deterministic");

    for (std::size_t i = 0; i < original.trades.size(); ++i)
    {
        const auto& a = original.trades[i];
        const auto& b = replayed.trades[i];
        EXPECT_EQ(b.order_id, a.order_id);
        EXPECT_EQ(b.side, a.side);
        EXPECT_EQ(b.symbol, a.symbol);
        EXPECT_EQ(b.strategy_name, a.strategy_name);
        EXPECT_EQ(b.timestamp, a.timestamp);
        EXPECT_NEAR(b.quantity, a.quantity, 1e-12);
        EXPECT_NEAR(b.fill_price, a.fill_price, 1e-12);
        EXPECT_NEAR(b.commission, a.commission, 1e-12);
        EXPECT_NEAR(b.pnl, a.pnl, 1e-9);
    }
}

TEST(EventLog, LedgerReplayRefusesMissingPrefixState)
{
    SilenceCout quiet;
    TempFile tf("partial_replay_refused.bin");
    {
        EventLogger logger(tf.path);
        logger.log(market_event(epoch_ms(1), "TEST", 1, 1, 1, 1, 1));
    }

    engine_config cfg;
    cfg.threading = thread_preset::inline_mode;
    engine eng(std::make_shared<data_handler>(), nullptr, nullptr,
               std::move(cfg));
    EXPECT_THROW(eng.run_replay(tf.path, /*replay_from_us=*/1),
                 std::runtime_error);
    EXPECT_THROW(eng.run_replay(tf.path, /*replay_from_us=*/0,
                                /*replay_to_us=*/1),
                 std::runtime_error);
}

TEST(EventLog, LedgerReplayRefusesOutputLoggingWithoutTruncatingSource)
{
    SilenceCout quiet;
    TempFile tf("replay-no-overwrite.bin");
    {
        EventLogger logger(tf.path, false);
        market_event market(epoch_ms(1), "TEST", 100, 101, 99, 100, 10);
        logger.log(market);
    }

    engine_config cfg;
    cfg.threading = thread_preset::inline_mode;
    cfg.event_log_path = tf.path;
    engine eng(std::make_shared<data_handler>(), nullptr, nullptr, std::move(cfg));
    EXPECT_THROW(eng.run_replay(tf.path), std::runtime_error);

    EventReplayer intact(tf.path);
    auto ev = intact.next();
    ASSERT_NE(ev, nullptr);
    EXPECT_EQ(ev->get_type(), event_type::market);
}

TEST(EventLog, LedgerReplayAppliesNonzeroFundingExactlyOnce)
{
    SilenceCout quiet;
    TempFile tf("replay-funding.bin");
    {
        EventLogger logger(tf.path, false);
        market_event market(epoch_ms(1), "TEST", 100, 100, 100, 100, 10);
        funding_event funding(epoch_ms(2), "TEST", 0.0, 7.25,
                              "FUNDING_FEE");
        logger.log(market);
        logger.log(funding);
    }

    engine_config cfg;
    cfg.initial_balance = 1'000.0;
    cfg.threading = thread_preset::inline_mode;
    engine eng(std::make_shared<data_handler>(), nullptr, nullptr, std::move(cfg));
    eng.run_replay(tf.path);

    const auto report = eng.get_analytics().generate_report();
    EXPECT_DOUBLE_EQ(eng.get_analytics().total_funding_pnl(), 7.25);
    EXPECT_DOUBLE_EQ(report.initial_equity, 1'000.0);
    EXPECT_DOUBLE_EQ(report.final_equity, 1'007.25);
    EXPECT_DOUBLE_EQ(report.final_equity - report.initial_equity,
                     report.realized_pnl + report.unrealized_pnl + 7.25);
}

TEST(EventLog, LedgerReplayReconstructsIncrementalL2Marks)
{
    SilenceCout quiet;
    TempFile tf("replay-l2-marks.bin");
    {
        EventLogger logger(tf.path, false);
        const l2_level bids[] = {{99.0, 10}};
        const l2_level asks[] = {{101.0, 10}};
        logger.log(l2_snapshot_event(
            epoch_ms(1), "TEST", bids, 1, asks, 1, 1));

        order_event order(epoch_ms(2), "TEST", order_type::market,
                          order_side::buy, 1.0, 100.0);
        order.set_order_id(1);
        order.set_strategy_name("ledger");
        logger.log(order);
        logger.log(fill_event(epoch_ms(3), "TEST", 1,
                              order_side::buy, 1.0, 100.0, 0.0,
                              0.0, 1, "ledger", 1));
        logger.log(l2_update_event(
            epoch_ms(4), "TEST", tick_side::bid, 99.0, 0, 1));
        logger.log(l2_update_event(
            epoch_ms(5), "TEST", tick_side::bid, 79.0, 10, 1));
        logger.log(l2_update_event(
            epoch_ms(6), "TEST", tick_side::ask, 101.0, 0, 1));
        logger.log(l2_update_event(
            epoch_ms(7), "TEST", tick_side::ask, 81.0, 10, 1));
    }

    engine_config cfg;
    cfg.initial_balance = 1'000.0;
    cfg.qty_scale = 1.0;
    cfg.threading = thread_preset::inline_mode;
    engine eng(std::make_shared<data_handler>(), nullptr, nullptr,
               std::move(cfg));
    eng.run_replay(tf.path);

    const auto report = eng.get_analytics().generate_report();
    EXPECT_DOUBLE_EQ(report.final_equity, 980.0);
    EXPECT_DOUBLE_EQ(report.unrealized_pnl, -20.0);
    // Replay mirrors the live incremental sequence, including the temporary
    // one-sided marks while the old best levels are removed/replaced.
    EXPECT_NEAR(report.max_drawdown, 22.0 / 1001.0 * 100.0, 1e-12);
}

TEST(EventLog, LegacyV1IsReadableForInspectionButRefusedAsLedger)
{
    SilenceCout quiet;
    TempFile tf("legacy-v1.bin");
    auto payload = event_serial::serialise(
        market_event(epoch_ms(1), "TEST", 1, 2, 0.5, 1.5, 10));
    ASSERT_GE(payload.size(), sizeof(uint64_t));
    payload.resize(payload.size() - sizeof(uint64_t));

    {
        std::ofstream out(tf.path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(EVENT_LOG_FILE_MAGIC.data()),
                  static_cast<std::streamsize>(EVENT_LOG_FILE_MAGIC.size()));
        const uint8_t version = EVENT_LOG_LEGACY_FILE_VERSION;
        const uint8_t flags = 0;
        out.write(reinterpret_cast<const char*>(&version), 1);
        out.write(reinterpret_cast<const char*>(&flags), 1);
        write_raw_event_record(out, event_type::market, payload);
    }

    EventReplayer inspection(tf.path);
    EXPECT_EQ(inspection.file_version(), EVENT_LOG_LEGACY_FILE_VERSION);
    auto inspected = inspection.next();
    ASSERT_NE(inspected, nullptr);
    EXPECT_EQ(static_cast<const market_event&>(*inspected).get_quantity_scale(),
              1U);

    engine_config cfg;
    cfg.threading = thread_preset::inline_mode;
    engine eng(std::make_shared<data_handler>(), nullptr, nullptr, std::move(cfg));
    EXPECT_THROW(eng.run_replay(tf.path), std::runtime_error);
}

// ─── Replay: verify events are consumed ─────────────────────────────────────

TEST(EventLog, Replay_ConsumesAllEvents)
{
    SilenceCout quiet;
    TempFile tf("replay_count.bin");

    // Log some events
    {
        EventLogger logger(tf.path);
        for (int i = 0; i < 10; ++i)
            logger.log(market_event(epoch_ms(i * 1000), "TEST",
                                    100.0 + i, 105.0 + i, 95.0 + i, 102.0 + i, 1000));
        logger.flush();
    }

    // Count events via replayer
    EventReplayer replayer(tf.path);
    int count = 0;
    while (replayer.has_next())
    {
        auto ev = replayer.next();
        if (ev) count++;
    }
    EXPECT_EQ(count, 10);
}
