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

    auto strict = get("margin_type_strict");
    if (strict == "1" || strict == "true")
        provider->set_margin_type_strict(true);

    // Position-based risk caps. Each is independent; absent / malformed
    // input leaves the cap at 0 (= disabled).
    auto parse_double = [&](const char* key, void(BinanceFuturesProvider::*set)(double)) {
        auto raw = get(key);
        if (raw.empty()) return;
        try { (provider.get()->*set)(std::stod(raw)); }
        catch (...) {}
    };
    parse_double("max_notional_usdt",            &BinanceFuturesProvider::set_max_notional_usdt);
    parse_double("max_leverage",                 &BinanceFuturesProvider::set_max_leverage);
    parse_double("min_liquidation_distance_pct", &BinanceFuturesProvider::set_min_liquidation_distance_pct);
    parse_double("maintenance_margin_pct",       &BinanceFuturesProvider::set_maintenance_margin_pct);

    auto parse_int64 = [&](const char* key, void(BinanceFuturesProvider::*set)(int64_t)) {
        auto raw = get(key);
        if (raw.empty()) return;
        try { (provider.get()->*set)(static_cast<int64_t>(std::stoll(raw))); }
        catch (...) {}
    };
    parse_int64("dead_man_countdown_ms",  &BinanceFuturesProvider::set_dead_man_countdown_ms);
    parse_int64("dead_man_heartbeat_ms",  &BinanceFuturesProvider::set_dead_man_heartbeat_ms);

    auto parse_bool = [&](const char* key, void(BinanceFuturesProvider::*set)(bool)) {
        auto raw = get(key);
        if (raw.empty()) return;
        bool v = (raw == "1" || raw == "true" || raw == "on" || raw == "yes");
        (provider.get()->*set)(v);
    };
    parse_bool("dms_attempt_position_close", &BinanceFuturesProvider::set_dms_attempt_position_close);

    return provider;
});

#endif // HAS_BINANCE
