#include <gtest/gtest.h>

#ifdef HAS_BITGET

#include "providers/bitget/bitget_endpoints.h"

TEST(BitgetEndpoints, MainnetDefaults)
{
    auto ep = bitget::uta_mainnet();
    EXPECT_EQ(ep.ws_public_host, "ws.bitget.com");
    EXPECT_EQ(ep.ws_private_host, "ws.bitget.com");
    EXPECT_EQ(ep.ws_port, "443");
    EXPECT_EQ(ep.rest_host, "api.bitget.com");
    EXPECT_EQ(ep.rest_port, "443");
    EXPECT_EQ(ep.ws_public_path, "/v3/ws/public");
    EXPECT_EQ(ep.ws_private_path, "/v3/ws/private");
    EXPECT_FALSE(ep.is_demo);
}

TEST(BitgetEndpoints, DemoUsesWspapHostsRestUnchanged)
{
    auto ep = bitget::uta_demo();
    EXPECT_EQ(ep.ws_public_host, "wspap.bitget.com");
    EXPECT_EQ(ep.ws_private_host, "wspap.bitget.com");
    EXPECT_EQ(ep.ws_port, "443");
    EXPECT_EQ(ep.rest_host, "api.bitget.com");
    EXPECT_EQ(ep.rest_port, "443");
    EXPECT_EQ(ep.ws_public_path, "/v3/ws/public");
    EXPECT_EQ(ep.ws_private_path, "/v3/ws/private");
    EXPECT_TRUE(ep.is_demo);
}

TEST(BitgetEndpoints, PublicAndPrivatePathsDiffer)
{
    auto mainnet = bitget::uta_mainnet();
    auto demo = bitget::uta_demo();
    EXPECT_NE(mainnet.ws_public_path, mainnet.ws_private_path);
    EXPECT_NE(demo.ws_public_path, demo.ws_private_path);
    EXPECT_EQ(mainnet.ws_public_path, demo.ws_public_path);
    EXPECT_EQ(mainnet.ws_private_path, demo.ws_private_path);
}

#endif // HAS_BITGET
