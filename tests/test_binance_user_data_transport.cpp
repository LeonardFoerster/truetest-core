// Compile-only smoke tests for BinanceUserDataTransport. Full behavioural
// tests live in the integration suite against a mock exchange because this
// class owns real TLS WebSocket I/O and listenKey REST lifecycle.

#include <gtest/gtest.h>

#ifdef HAS_BINANCE

#include "providers/binance/binance_user_data_transport.h"

#include <memory>

TEST(BinanceUserDataTransport, ConstructDoesNotOpenConnections)
{
    BinanceUserDataTransport tx(nullptr);
    EXPECT_EQ(tx.state(), IFillTransport::lifecycle::closed);
    EXPECT_TRUE(tx.listen_key().empty());
}

TEST(BinanceUserDataTransport, OpenWithoutRestReportsError)
{
    BinanceUserDataTransport tx(nullptr);
    IFillTransport::lifecycle seen = IFillTransport::lifecycle::closed;
    tx.set_on_status([&](IFillTransport::lifecycle s, std::string_view) {
        seen = s;
    });
    EXPECT_FALSE(tx.open());
    EXPECT_EQ(tx.state(), IFillTransport::lifecycle::error);
    EXPECT_EQ(seen,       IFillTransport::lifecycle::error);
}

TEST(BinanceUserDataTransport, DestructorWithNoOpenIsClean)
{
    // just making sure the destructor doesn't deadlock when open() was never
    // successful — regressing this would hang the whole test binary.
    BinanceUserDataTransport tx(nullptr);
    (void)tx.open();
}

#endif // HAS_BINANCE
