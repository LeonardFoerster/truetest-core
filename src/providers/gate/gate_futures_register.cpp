#ifdef HAS_GATE

#include "providers/provider_registry.h"
#include "providers/gate/gate_futures_provider.h"
#include "providers/gate/gate_endpoints.h"
#include "providers/gate/gate_parser.h"

#include <memory>
#include <stdexcept>
#include <string>

namespace {

std::shared_ptr<IProvider> make_gate_futures(const provider_config& cfg)
{
    auto get = [&](const std::string& key) -> std::string {
        auto it = cfg.find(key);
        if (it == cfg.end()) return "";
        return it->second;
    };

    auto symbol = get("symbol");
    if (symbol.empty())
        throw std::runtime_error(
            "gate-futures provider requires 'symbol' (e.g. BTC_USDT)");

    // Canonicalize to Gate contract form (BTC_USDT). Bare BTCUSDT accepted.
    symbol = gate::normalize_contract_symbol(symbol);

    auto stream = get("stream");
    if (stream.empty())
        stream = "trades";

    const auto host_cfg = get("host");
    const auto port_cfg = get("port");
    const auto rest_host_cfg = get("rest_host");
    const auto testnet_cfg = get("testnet");

    auto is_on = [](const std::string& v) {
        return v == "1" || v == "true" || v == "on" || v == "yes";
    };

    const bool want_testnet =
        is_on(testnet_cfg) || gate::looks_like_testnet_host(host_cfg)
        || gate::looks_like_testnet_host(rest_host_cfg);

    gate::endpoints ep =
        want_testnet ? gate::usdt_testnet() : gate::usdt_mainnet();

    if (!host_cfg.empty())
        ep.ws_host = host_cfg;
    if (!port_cfg.empty())
        ep.ws_port = port_cfg;
    if (!rest_host_cfg.empty())
        ep.rest_host = rest_host_cfg;

    auto provider = std::make_shared<GateFuturesProvider>(
        symbol, stream,
        get("api_key"), get("api_secret"),
        get("user_id"),
        ep);

    auto depth = get("depth_stream");
    if (depth.empty())
        depth = get("depth");
    if (!depth.empty())
        provider->set_depth_spec(depth);

    // Optional advisory inputs (stored for later phases).
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
    if (is_on(strict))
        provider->set_margin_type_strict(true);

    auto parse_double = [&](const char* key,
                            void (GateFuturesProvider::*set)(double)) {
        auto raw = get(key);
        if (raw.empty()) return;
        try { (provider.get()->*set)(std::stod(raw)); }
        catch (...) {}
    };
    parse_double("max_notional_usdt",
                 &GateFuturesProvider::set_max_notional_usdt);
    parse_double("max_leverage",
                 &GateFuturesProvider::set_max_leverage);
    parse_double("min_liquidation_distance_pct",
                 &GateFuturesProvider::set_min_liquidation_distance_pct);
    parse_double("maintenance_margin_pct",
                 &GateFuturesProvider::set_maintenance_margin_pct);

    auto parse_int64 = [&](const char* key,
                           void (GateFuturesProvider::*set)(std::int64_t)) {
        auto raw = get(key);
        if (raw.empty()) return;
        try
        {
            (provider.get()->*set)(static_cast<std::int64_t>(std::stoll(raw)));
        }
        catch (...) {}
    };
    parse_int64("dead_man_countdown_ms",
                &GateFuturesProvider::set_dead_man_countdown_ms);
    parse_int64("dead_man_heartbeat_ms",
                &GateFuturesProvider::set_dead_man_heartbeat_ms);

    auto parse_bool = [&](const char* key,
                          void (GateFuturesProvider::*set)(bool)) {
        auto raw = get(key);
        if (raw.empty()) return;
        (provider.get()->*set)(is_on(raw));
    };
    parse_bool("dms_attempt_position_close",
               &GateFuturesProvider::set_dms_attempt_position_close);

    return provider;
}

} // namespace

// Single static init registers both names. REGISTER_PROVIDER pastes
// __LINE__ literally as _reg___LINE__, so two macro uses collide.
namespace {
static const bool k_reg_gate_futures = []() {
    ProviderRegistry::instance().register_provider(
        "gate-futures", make_gate_futures);
    ProviderRegistry::instance().register_provider(
        "gate", make_gate_futures);
    return true;
}();
} // namespace

#endif // HAS_GATE
