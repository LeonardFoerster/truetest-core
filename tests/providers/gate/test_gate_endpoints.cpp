#include <gtest/gtest.h>

#ifdef HAS_GATE

#include "providers/gate/gate_endpoints.h"

TEST(GateEndpoints, MainnetUsdtDefaults)
{
    auto ep = gate::usdt_mainnet();
    EXPECT_EQ(ep.ws_host, "fx-ws.gateio.ws");
    EXPECT_EQ(ep.ws_path, "/v4/ws/usdt");
    EXPECT_EQ(ep.rest_host, "api.gateio.ws");
    EXPECT_EQ(ep.rest_prefix, "/api/v4");
    EXPECT_EQ(ep.ws_port, "443");
    EXPECT_EQ(ep.rest_port, "443");
    EXPECT_FALSE(ep.is_testnet);
    EXPECT_EQ(gate::settle_str(ep), "usdt");
}

TEST(GateEndpoints, TestnetHosts)
{
    auto ep = gate::usdt_testnet();
    EXPECT_EQ(ep.ws_host, "fx-ws-testnet.gateio.ws");
    EXPECT_EQ(ep.rest_host, "api-testnet.gateapi.io");
    EXPECT_TRUE(ep.is_testnet);
}

TEST(GateEndpoints, FuturesPathIncludesSettle)
{
    auto ep = gate::usdt_mainnet();
    EXPECT_EQ(gate::futures_path(ep, "/orders"),
              "/api/v4/futures/usdt/orders");
    EXPECT_EQ(gate::futures_path(ep, "/contracts/BTC_USDT"),
              "/api/v4/futures/usdt/contracts/BTC_USDT");
}

TEST(GateEndpoints, SpotTimePath)
{
    auto ep = gate::usdt_mainnet();
    EXPECT_EQ(gate::spot_time_path(ep), "/api/v4/spot/time");
}

TEST(GateEndpoints, HostClassification)
{
    EXPECT_TRUE(gate::looks_like_testnet_host("fx-ws-testnet.gateio.ws"));
    EXPECT_TRUE(gate::looks_like_testnet_host("api-testnet.gateapi.io"));
    EXPECT_FALSE(gate::looks_like_testnet_host("api.gateio.ws"));
}

#endif // HAS_GATE
