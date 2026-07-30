#include <gtest/gtest.h>

#ifdef HAS_BINANCE

#include "providers/binance/binance_futures_kill_switch.h"

#include <chrono>
#include <sstream>

namespace {

// RAII helper to silence stderr - the kill switch logs deliberately
// noisy diagnostics on the failure paths we exercise here.
struct SilenceStderr {
    std::ostringstream sink;
    std::streambuf* orig;
    SilenceStderr() : orig(std::cerr.rdbuf(sink.rdbuf())) {}
    ~SilenceStderr() { std::cerr.rdbuf(orig); }
};

}

TEST(BinanceFuturesKillSwitch, NullRestReturnsFalseAndDoesNotCrash)
{
    SilenceStderr quiet;
    BinanceFuturesKillSwitch ks(nullptr, "BTCUSDT", nullptr);
    EXPECT_FALSE(ks.cancel_all_and_flatten(std::chrono::milliseconds(1000)));
}

TEST(BinanceFuturesKillSwitch, ImplementsIKillSwitchInterface)
{
    // Compile-time check: BinanceFuturesKillSwitch must be substitutable
    // for an IKillSwitch (the engine queries via the base interface).
    BinanceFuturesKillSwitch ks(nullptr, "BTCUSDT", nullptr);
    IKillSwitch& base = ks;
    SilenceStderr quiet;
    EXPECT_FALSE(base.cancel_all_and_flatten(std::chrono::milliseconds(1)));
}

#endif // HAS_BINANCE
