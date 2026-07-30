#include <gtest/gtest.h>

#ifdef HAS_BYBIT

#include "providers/bybit/bybit_endpoints.h"

TEST(BybitEndpoints, MainnetLinearDefaults)
{
    auto ep = bybit::linear_mainnet();
    EXPECT_EQ(ep.rest_host, "api.bybit.com");
    EXPECT_EQ(ep.rest_port, "443");
    EXPECT_EQ(ep.ws_public_host, "stream.bybit.com");
    EXPECT_EQ(ep.ws_private_host, "stream.bybit.com");
    EXPECT_EQ(ep.ws_port, "443");
    EXPECT_EQ(ep.ws_public_path, "/v5/public/linear");
    EXPECT_EQ(ep.ws_private_path, "/v5/private");
    EXPECT_FALSE(ep.is_testnet);
    EXPECT_FALSE(ep.is_demo);
}

TEST(BybitEndpoints, TestnetHosts)
{
    auto ep = bybit::linear_testnet();
    EXPECT_EQ(ep.rest_host, "api-testnet.bybit.com");
    EXPECT_EQ(ep.ws_public_host, "stream-testnet.bybit.com");
    EXPECT_EQ(ep.ws_private_host, "stream-testnet.bybit.com");
    EXPECT_TRUE(ep.is_testnet);
    EXPECT_FALSE(ep.is_demo);
}

TEST(BybitEndpoints, DemoUsesDemoPrivateAndMainnetPublicMd)
{
    auto ep = bybit::linear_demo();
    EXPECT_EQ(ep.rest_host, "api-demo.bybit.com");
    EXPECT_EQ(ep.ws_public_host, "stream.bybit.com");
    EXPECT_EQ(ep.ws_private_host, "stream-demo.bybit.com");
    EXPECT_FALSE(ep.is_testnet);
    EXPECT_TRUE(ep.is_demo);
}

TEST(BybitEndpoints, PathsAreStable)
{
    EXPECT_STREQ(bybit::paths::market_time, "/v5/market/time");
    EXPECT_STREQ(bybit::paths::order_create, "/v5/order/create");
    EXPECT_STREQ(bybit::paths::position_list, "/v5/position/list");
}

TEST(BybitEndpoints, HostClassification)
{
    EXPECT_TRUE(bybit::looks_like_testnet_host("stream-testnet.bybit.com"));
    EXPECT_TRUE(bybit::looks_like_demo_host("api-demo.bybit.com"));
    EXPECT_FALSE(bybit::looks_like_testnet_host("api.bybit.com"));
}

#endif // HAS_BYBIT
