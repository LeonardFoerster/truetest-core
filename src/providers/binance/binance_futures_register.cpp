#ifdef HAS_BINANCE

#include "providers/provider_registry.h"
#include "providers/binance/binance_futures_provider.h"
#include "providers/binance/binance_endpoints.h"

#include <stdexcept>

REGISTER_PROVIDER("binance-futures", [](const provider_config& cfg) {
    auto get = [&](const std::string& key) -> std::string {
        auto it = cfg.find(key);
        if (it == cfg.end()) return "";
        return it->second;
    };

    auto symbol = get("symbol");
    if (symbol.empty())
        throw std::runtime_error(
            "binance-futures provider requires 'symbol' (e.g. btcusdt)");

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
        ? binance::usdm_testnet()
        : binance::usdm_mainnet();

    if (!host_cfg.empty()) ep.ws_host = host_cfg;
    if (!port_cfg.empty()) ep.ws_port = port_cfg;

    auto provider = std::make_shared<BinanceFuturesProvider>(
        symbol, stream,
        get("api_key"), get("api_secret"),
        ep.ws_host, ep.ws_port
    );
    provider->set_endpoints(ep);

    auto depth = get("depth_stream");
    if (!depth.empty())
        provider->set_depth_stream(depth);

    // Optional advisory inputs. Silent absence = use provider defaults
    // (margin-mode check off, liquidation-distance check at 5%).
    auto margin_type = get("margin_type");
    if (!margin_type.empty())
        provider->set_expected_margin_type(margin_type);

    auto liq_pct_str = get("liquidation_warn_pct");
    if (!liq_pct_str.empty())
    {
        try
        {
            provider->set_liquidation_warn_pct(std::stod(liq_pct_str));
        }
        catch (...)
        {
            // Ignore malformed input; provider default applies.
        }
    }

    return provider;
});

#endif // HAS_BINANCE
