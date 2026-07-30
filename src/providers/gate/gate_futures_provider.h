#pragma once
#ifdef HAS_GATE

// Gate.io USDT-M futures provider (Phase 0 scaffold).
// Registry + config surface only. Market data (Phase 1), REST/instrument
// (Phase 2), live safety (Phase 3), and execution bridge (Phase 4) land
// in subsequent PRs. All venue headers stay behind HAS_GATE.

#include "providers/gate/gate_endpoints.h"
#include "providers/provider.h"

#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <utility>

class GateFuturesProvider : public IProvider
{
public:
    GateFuturesProvider(std::string symbol,
                        std::string stream_type,
                        std::string api_key = {},
                        std::string api_secret = {},
                        std::string user_id = {},
                        gate::endpoints ep = gate::usdt_mainnet())
        : symbol_(std::move(symbol))
        , stream_type_(std::move(stream_type))
        , api_key_(std::move(api_key))
        , api_secret_(std::move(api_secret))
        , user_id_(std::move(user_id))
        , endpoints_(std::move(ep))
    {}

    std::string name() const override { return "gate-futures"; }

    bool has_data_feed() const override { return true; }
    bool has_execution() const override { return true; }

    // Phase 0: refuse open. Market WS / REST / safety stack land in later
    // phases — never pretend the venue is live without transports.
    bool open() override
    {
        lifecycle_ = lifecycle::opening;
        std::cerr << "  GateFuturesProvider: Phase 0 stub — "
                  << (endpoints_.is_testnet ? "[TESTNET] " : "")
                  << "ws=" << endpoints_.ws_host << endpoints_.ws_path
                  << " rest=" << endpoints_.rest_host
                  << " symbol=" << symbol_
                  << " (open refused until Phase 1+)\n";
        lifecycle_ = lifecycle::error;
        return false;
    }

    void close() override
    {
        lifecycle_ = lifecycle::closed;
    }

    lifecycle lifecycle_state() const override { return lifecycle_; }

    std::shared_ptr<IDataTransport> get_transport() override
    {
        return nullptr; // Phase 1
    }

    std::shared_ptr<IExecutionAdapter> get_execution_adapter() override
    {
        return nullptr; // Phase 1 paper / Phase 4 live
    }

    // ── Config surface used by register + later phases ──────────────────

    void set_endpoints(gate::endpoints ep) { endpoints_ = std::move(ep); }
    const gate::endpoints& endpoints() const { return endpoints_; }
    bool is_testnet() const { return endpoints_.is_testnet; }

    const std::string& symbol() const { return symbol_; }
    const std::string& stream_type() const { return stream_type_; }
    const std::string& api_key() const { return api_key_; }
    const std::string& api_secret() const { return api_secret_; }
    const std::string& user_id() const { return user_id_; }

    // Depth: "100ms:100" or free-form config from depth_stream key.
    void set_depth_spec(std::string spec) { depth_spec_ = std::move(spec); }
    const std::string& depth_spec() const { return depth_spec_; }

    // Optional advisory / risk keys (wired in later phases; stored now so
    // register.cpp can set them without API churn).
    void set_expected_margin_type(std::string v)
    {
        expected_margin_type_ = std::move(v);
    }
    void set_margin_type_strict(bool v) { margin_type_strict_ = v; }
    void set_liquidation_warn_pct(double v) { liquidation_warn_pct_ = v; }

    void set_max_notional_usdt(double v) { max_notional_usdt_ = v; }
    void set_max_leverage(double v) { max_leverage_ = v; }
    void set_min_liquidation_distance_pct(double v)
    {
        min_liquidation_distance_pct_ = v;
    }
    void set_maintenance_margin_pct(double v)
    {
        maintenance_margin_pct_ = v;
    }

    // DMS knobs stored in ms for CLI parity with binance-futures keys;
    // Gate wire timeout is seconds (converted at arm time in Phase 3).
    void set_dead_man_countdown_ms(std::int64_t v)
    {
        dead_man_countdown_ms_ = v;
    }
    void set_dead_man_heartbeat_ms(std::int64_t v)
    {
        dead_man_heartbeat_ms_ = v;
    }
    void set_dms_attempt_position_close(bool v)
    {
        dms_attempt_position_close_ = v;
    }

    double max_notional_usdt() const { return max_notional_usdt_; }
    double max_leverage() const { return max_leverage_; }
    std::int64_t dead_man_countdown_ms() const
    {
        return dead_man_countdown_ms_;
    }
    std::int64_t dead_man_heartbeat_ms() const
    {
        return dead_man_heartbeat_ms_;
    }
    bool dms_attempt_position_close() const
    {
        return dms_attempt_position_close_;
    }
    const std::string& expected_margin_type() const
    {
        return expected_margin_type_;
    }
    bool margin_type_strict() const { return margin_type_strict_; }

private:
    std::string symbol_;
    std::string stream_type_;
    std::string api_key_;
    std::string api_secret_;
    std::string user_id_;
    gate::endpoints endpoints_;
    std::string depth_spec_;

    std::string expected_margin_type_;
    bool margin_type_strict_ = false;
    double liquidation_warn_pct_ = 5.0;

    double max_notional_usdt_ = 0.0;
    double max_leverage_ = 0.0;
    double min_liquidation_distance_pct_ = 0.0;
    double maintenance_margin_pct_ = 0.0;

    std::int64_t dead_man_countdown_ms_ = 0;
    std::int64_t dead_man_heartbeat_ms_ = 0;
    bool dms_attempt_position_close_ = false;

    lifecycle lifecycle_ = lifecycle::closed;
};

#endif // HAS_GATE
