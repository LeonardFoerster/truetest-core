#include <gtest/gtest.h>

#include "providers/binance/binance_endpoints.h"

TEST(BinanceEndpoints, MainnetDefaults)
{
    auto ep = binance::spot_mainnet();
    EXPECT_EQ(ep.ws_host, "stream.binance.com");
    EXPECT_EQ(ep.ws_port, "9443");
    EXPECT_EQ(ep.rest_host, "api.binance.com");
    EXPECT_EQ(ep.rest_port, "443");
    EXPECT_FALSE(ep.is_testnet);
}

TEST(BinanceEndpoints, TestnetDefaults)
{
    auto ep = binance::spot_testnet();
    EXPECT_EQ(ep.ws_host, "stream.testnet.binance.vision");
    EXPECT_EQ(ep.rest_host, "testnet.binance.vision");
    EXPECT_TRUE(ep.is_testnet);
}

TEST(BinanceEndpoints, LooksLikeTestnetHostDetectsSubstring)
{
    EXPECT_TRUE(binance::looks_like_testnet_host("stream.testnet.binance.vision"));
    EXPECT_TRUE(binance::looks_like_testnet_host("testnet.binance.vision"));
    EXPECT_FALSE(binance::looks_like_testnet_host("stream.binance.com"));
    EXPECT_FALSE(binance::looks_like_testnet_host(""));
}

TEST(BinanceEndpoints, FromHostPicksTestnetWhenMatching)
{
    auto ep = binance::from_host("stream.testnet.binance.vision");
    EXPECT_TRUE(ep.is_testnet);
    EXPECT_EQ(ep.rest_host, "testnet.binance.vision");
}

TEST(BinanceEndpoints, FromHostFallsBackToMainnet)
{
    auto ep = binance::from_host("stream.binance.com");
    EXPECT_FALSE(ep.is_testnet);
    EXPECT_EQ(ep.rest_host, "api.binance.com");
}

TEST(BinanceEndpoints, FromHostEmptyUsesMainnet)
{
    auto ep = binance::from_host("");
    EXPECT_FALSE(ep.is_testnet);
    EXPECT_EQ(ep.ws_host, "stream.binance.com");
}

TEST(BinanceEndpoints, FromHostOverridesWsHostButKeepsClassification)
{
    auto ep = binance::from_host("custom.testnet.mydomain");
    EXPECT_TRUE(ep.is_testnet);
    EXPECT_EQ(ep.ws_host, "custom.testnet.mydomain");
    EXPECT_EQ(ep.rest_host, "testnet.binance.vision");
}
