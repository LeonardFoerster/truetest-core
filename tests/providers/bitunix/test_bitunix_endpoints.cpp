#include <gtest/gtest.h>

#ifdef HAS_BITUNIX

#include "providers/bitunix/bitunix_endpoints.h"
#include "providers/bitunix/bitunix_transport.h"

TEST(BitunixEndpoints, MainnetHosts)
{
    auto ep = bitunix::mainnet();
    EXPECT_EQ(ep.ws_public_host, "fapi.bitunix.com");
    EXPECT_EQ(ep.rest_host, "fapi.bitunix.com");
    EXPECT_EQ(ep.ws_public_path, "/public/");
    EXPECT_EQ(ep.ws_port, "443");
}

TEST(BitunixEndpoints, NormalizeSymbol)
{
    EXPECT_EQ(bitunix::normalize_symbol("btcusdt"), "BTCUSDT");
    EXPECT_EQ(bitunix::normalize_symbol("BTCUSDT"), "BTCUSDT");
}

TEST(BitunixTransportHelpers, MapStreamAndSubscribe)
{
    EXPECT_EQ(bitunix::map_stream_to_channel("trade"), "trade");
    EXPECT_EQ(bitunix::map_stream_to_channel(""), "trade");
    EXPECT_EQ(bitunix::map_stream_to_channel("depth_book5"), "depth_book5");

    const auto sub = bitunix::build_subscribe_json("BTCUSDT", "trade");
    EXPECT_EQ(sub,
              "{\"op\":\"subscribe\",\"args\":[{\"symbol\":\"BTCUSDT\","
              "\"ch\":\"trade\"}]}");

    const auto ping = bitunix::build_ping_json(1732519687);
    EXPECT_EQ(ping, "{\"op\":\"ping\",\"ping\":1732519687}");
}

#else

TEST(BitunixEndpoints, SkippedWithoutHasBitunix)
{
    GTEST_SKIP() << "HAS_BITUNIX not defined";
}

#endif // HAS_BITUNIX
