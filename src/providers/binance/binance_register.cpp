#ifdef HAS_BINANCE

#include "providers/provider_registry.h"
#include "providers/binance/binance_provider.h"
#include "providers/binance/binance_endpoints.h"

#include <stdexcept>

REGISTER_PROVIDER("binance", [](const provider_config& cfg) {
    auto get = [&](const std::string& key) -> std::string {
        auto it = cfg.find(key);
        if (it == cfg.end()) return "";
        return it->second;
    };

    auto symbol = get("symbol");
    if (symbol.empty())
        throw std::runtime_error("binance provider requires 'symbol' (e.g. btcusdt)");

    auto stream = get("stream");
    if (stream.empty())
        stream = "trade";

    const auto host_cfg = get("host");
    const auto port_cfg = get("port");
    const auto testnet_cfg = get("testnet");
    const bool want_testnet =
        testnet_cfg == "1" || testnet_cfg == "true" ||
        binance::looks_like_testnet_host(host_cfg);

    binance::endpoints ep = want_testnet
        ? binance::spot_testnet()
        : binance::spot_mainnet();

    if (!host_cfg.empty()) ep.ws_host = host_cfg;
    if (!port_cfg.empty()) ep.ws_port = port_cfg;

    auto provider = std::make_shared<BinanceProvider>(
        symbol, stream,
        get("api_key"), get("api_secret"),
        ep.ws_host, ep.ws_port
    );
    provider->set_endpoints(ep);
    return provider;
});

#endif // HAS_BINANCE
