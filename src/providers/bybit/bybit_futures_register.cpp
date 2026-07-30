#ifdef HAS_BYBIT

#include "providers/provider_registry.h"
#include "providers/bybit/bybit_futures_provider.h"
#include "providers/bybit/bybit_endpoints.h"

#include <memory>
#include <stdexcept>
#include <string>

namespace {

std::shared_ptr<IProvider> make_bybit_futures(const provider_config& cfg)
{
    auto get = [&](const std::string& key) -> std::string {
        auto it = cfg.find(key);
        if (it == cfg.end()) return "";
        return it->second;
    };

    auto symbol = get("symbol");
    if (symbol.empty())
        throw std::runtime_error(
            "bybit-futures provider requires 'symbol' (e.g. BTCUSDT)");

    auto stream = get("stream");
    if (stream.empty())
        stream = "trade";

    const auto host_cfg = get("host");
    const auto port_cfg = get("port");
    const auto testnet_cfg = get("testnet");
    const auto demo_cfg = get("demo");

    auto is_on = [](const std::string& v) {
        return v == "1" || v == "true" || v == "on" || v == "yes";
    };

    // demo takes precedence over testnet (distinct Bybit environments).
    const bool want_demo =
        is_on(demo_cfg) || bybit::looks_like_demo_host(host_cfg);
    const bool want_testnet =
        !want_demo &&
        (is_on(testnet_cfg) || bybit::looks_like_testnet_host(host_cfg));

    bybit::endpoints ep = want_demo
        ? bybit::linear_demo()
        : want_testnet
            ? bybit::linear_testnet()
            : bybit::linear_mainnet();

    if (!host_cfg.empty())
    {
        ep.ws_public_host = host_cfg;
        // Private host stays on the environment default unless overridden
        // later; public host override is the common CLI case.
    }
    if (!port_cfg.empty())
        ep.ws_port = port_cfg;

    auto provider = std::make_shared<BybitFuturesProvider>(
        symbol, stream,
        get("api_key"), get("api_secret"),
        ep);
    provider->set_endpoints(ep);

    auto depth = get("depth_stream");
    if (!depth.empty())
        provider->set_depth_stream(depth);

    // Optional advisory inputs. Silent absence = provider defaults.
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

    // Position-based risk caps. Absent / malformed → cap stays 0 (disabled).
    auto parse_double = [&](const char* key, void(BybitFuturesProvider::*set)(double)) {
        auto raw = get(key);
        if (raw.empty()) return;
        try { (provider.get()->*set)(std::stod(raw)); }
        catch (...) {}
    };
    parse_double("max_notional_usdt",            &BybitFuturesProvider::set_max_notional_usdt);
    parse_double("max_leverage",                 &BybitFuturesProvider::set_max_leverage);
    parse_double("min_liquidation_distance_pct", &BybitFuturesProvider::set_min_liquidation_distance_pct);
    parse_double("maintenance_margin_pct",       &BybitFuturesProvider::set_maintenance_margin_pct);

    auto parse_int64 = [&](const char* key, void(BybitFuturesProvider::*set)(std::int64_t)) {
        auto raw = get(key);
        if (raw.empty()) return;
        try { (provider.get()->*set)(static_cast<std::int64_t>(std::stoll(raw))); }
        catch (...) {}
    };
    parse_int64("dead_man_countdown_ms",  &BybitFuturesProvider::set_dead_man_countdown_ms);
    parse_int64("dead_man_heartbeat_ms",  &BybitFuturesProvider::set_dead_man_heartbeat_ms);

    auto parse_bool = [&](const char* key, void(BybitFuturesProvider::*set)(bool)) {
        auto raw = get(key);
        if (raw.empty()) return;
        bool v = (raw == "1" || raw == "true" || raw == "on" || raw == "yes");
        (provider.get()->*set)(v);
    };
    parse_bool("dms_attempt_position_close", &BybitFuturesProvider::set_dms_attempt_position_close);

    return provider;
}

} // namespace

// Single static init registers both names. REGISTER_PROVIDER pastes
// __LINE__ literally as _reg___LINE_, so two macro uses collide.
namespace {
static const bool k_reg_bybit_futures = []() {
    ProviderRegistry::instance().register_provider(
        "bybit-futures", make_bybit_futures);
    ProviderRegistry::instance().register_provider(
        "bybit", make_bybit_futures);
    return true;
}();
} // namespace

#endif // HAS_BYBIT
