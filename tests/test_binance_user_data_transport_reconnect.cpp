#include <gtest/gtest.h>

#ifdef HAS_BINANCE

#include "providers/binance/binance_user_data_transport.h"

namespace {

using run_result = BinanceUserDataTransport::run_result;
using next_step  = BinanceUserDataTransport::next_step;
using state_t    = BinanceUserDataTransport::reconnect_state;

constexpr int k_max = 10;
constexpr long long k_reset_ms = 5 * 60 * 1000;

}

TEST(BinanceUserDataReconnect, StoppedResultIsStop)
{
    state_t s{0, 0, false};
    auto step = BinanceUserDataTransport::decide_next(
        s, run_result::stopped, k_max, 1'000, k_reset_ms);
    EXPECT_EQ(step, next_step::stop);
}

TEST(BinanceUserDataReconnect, StopFlagShortCircuits)
{
    state_t s{0, 0, true};
    auto step = BinanceUserDataTransport::decide_next(
        s, run_result::network_error, k_max, 1'000, k_reset_ms);
    EXPECT_EQ(step, next_step::stop);
}

TEST(BinanceUserDataReconnect, FirstAttemptAlwaysRetries)
{
    state_t s{0, 0, false};
    auto step = BinanceUserDataTransport::decide_next(
        s, run_result::network_error, k_max, 1'000, k_reset_ms);
    EXPECT_EQ(step, next_step::retry_after);
}

TEST(BinanceUserDataReconnect, PenultimateAttemptRetries)
{
    state_t s{k_max - 2, 0, false};
    auto step = BinanceUserDataTransport::decide_next(
        s, run_result::network_error, k_max, 1'000, k_reset_ms);
    EXPECT_EQ(step, next_step::retry_after);
}

TEST(BinanceUserDataReconnect, FinalAttemptGivesUp)
{
    state_t s{k_max - 1, 0, false};
    auto step = BinanceUserDataTransport::decide_next(
        s, run_result::network_error, k_max, 1'000, k_reset_ms);
    EXPECT_EQ(step, next_step::give_up);
}

TEST(BinanceUserDataReconnect, LongSessionAllowsRetryAfterExhaustion)
{
    long long now_ms = 10 * 60 * 1000;
    long long last_open_ms = 1 * 60 * 1000;     // 9 min ago
    state_t s{k_max - 1, last_open_ms, false};
    auto step = BinanceUserDataTransport::decide_next(
        s, run_result::network_error, k_max, now_ms, k_reset_ms);
    EXPECT_EQ(step, next_step::retry_after);
}

TEST(BinanceUserDataReconnect, HandshakeErrorUsesAttemptBudget)
{
    state_t s{k_max - 1, 0, false};
    auto step = BinanceUserDataTransport::decide_next(
        s, run_result::handshake_error, k_max, 1'000, k_reset_ms);
    EXPECT_EQ(step, next_step::give_up);
}

#endif // HAS_BINANCE
