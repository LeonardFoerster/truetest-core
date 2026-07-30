#pragma once
#ifdef HAS_BYBIT

// ============================================================
// Bybit V5 linear USDT perpetual provider — Phase 0 scaffold.
// Market data, REST orders, and safety stack land in later
// phases. open() currently refuses (stub) so nothing can
// accidentally go live without the full stack.
// ============================================================

#include "engine/engine_config.h"
#include "providers/bybit/bybit_endpoints.h"
#include "providers/provider.h"
#include "risk/futures_risk_check.h"
#include "risk/maintenance_margin_table.h"

#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <utility>

class BybitFuturesProvider : public IProvider
{
public:
    BybitFuturesProvider(
        const std::string& symbol,
        const std::string& stream_type,
        const std::string& api_key = "",
        const std::string& api_secret = "",
        bybit::endpoints ep = bybit::linear_mainnet())
        : symbol_(symbol)
        , stream_type_(stream_type)
        , api_key_(api_key)
        , api_secret_(api_secret)
        , endpoints_(std::move(ep))
    {
    }

    void set_depth_stream(const std::string& depth_stream)
    {
        depth_stream_ = depth_stream;
    }

    const std::string& depth_stream() const { return depth_stream_; }
    const std::string& symbol() const { return symbol_; }
    const std::string& stream_type() const { return stream_type_; }
    const std::string& api_key() const { return api_key_; }

    // Operator-set advisory / risk inputs (wired by register; enforced
    // once live open is implemented in later phases).
    void set_expected_margin_type(std::string mt)
    {
        expected_margin_type_ = std::move(mt);
    }
    void set_liquidation_warn_pct(double pct)
    {
        liquidation_warn_pct_ = pct;
    }
    void set_margin_type_strict(bool strict)
    {
        margin_type_strict_ = strict;
    }

    void set_max_notional_usdt(double v)             { rc_cfg_.max_notional_usdt = v; }
    void set_max_leverage(double v)                  { rc_cfg_.max_leverage = v; }
    void set_min_liquidation_distance_pct(double v)  { rc_cfg_.min_liquidation_distance_pct = v; }
    void set_maintenance_margin_pct(double v)        { rc_cfg_.maintenance_margin_pct = v; }

    void set_dead_man_countdown_ms(std::int64_t v)   { dead_man_countdown_ms_ = v; }
    void set_dead_man_heartbeat_ms(std::int64_t v)   { dead_man_heartbeat_ms_ = v; }
    void set_dms_attempt_position_close(bool v)      { dms_attempt_position_close_ = v; }

    void set_endpoints(bybit::endpoints ep)
    {
        endpoints_ = std::move(ep);
    }

    const bybit::endpoints& endpoints() const { return endpoints_; }

    bool is_testnet() const { return endpoints_.is_testnet; }
    bool is_demo() const { return endpoints_.is_demo; }

    std::string name() const override { return "bybit-futures"; }

    bool has_data_feed() const override { return true; }
    bool has_execution() const override { return true; }

    lifecycle lifecycle_state() const override { return state_; }

    void configure(const engine_config& cfg) override
    {
        mode_ = cfg.mode;
        qty_scale_ = cfg.qty_scale;
        seed_ = cfg.seed;
        configured_ = true;
    }

    // Phase 0: refuse open. Later phases wire public WS + execution.
    // Building FuturesRiskCheck early keeps risk-cap register tests offline.
    bool open() override
    {
        state_ = lifecycle::opening;

        if (rc_cfg_.max_notional_usdt > 0.0
            || rc_cfg_.max_leverage > 0.0
            || rc_cfg_.min_liquidation_distance_pct > 0.0)
        {
            risk_check_ = std::make_shared<FuturesRiskCheck>(rc_cfg_, mm_table_);
        }

        std::cerr << "  BybitFuturesProvider: Phase 0 stub — "
                  << (endpoints_.is_demo ? "[DEMO] "
                      : endpoints_.is_testnet ? "[TESTNET] " : "")
                  << "ws_public=" << endpoints_.ws_public_host
                  << endpoints_.ws_public_path
                  << " rest=" << endpoints_.rest_host
                  << " symbol=" << symbol_
                  << " (open refused until Phase 1+)\n";

        state_ = lifecycle::error;
        return false;
    }

    void close() override
    {
        risk_check_.reset();
        state_ = lifecycle::closed;
    }

    std::shared_ptr<IDataTransport> get_transport() override
    {
        return nullptr;
    }

    std::shared_ptr<IExecutionAdapter> get_execution_adapter() override
    {
        return nullptr;
    }

    std::shared_ptr<IRiskCheck> get_risk_check() override
    {
        return risk_check_;
    }

private:
    std::string symbol_;
    std::string stream_type_;
    std::string api_key_;
    std::string api_secret_;
    std::string depth_stream_;
    bybit::endpoints endpoints_;

    engine_mode mode_ = engine_mode::backtest;
    double qty_scale_ = 1.0;
    std::uint64_t seed_ = 0;
    bool configured_ = false;

    std::string expected_margin_type_;
    double liquidation_warn_pct_ = 5.0;
    bool margin_type_strict_ = false;

    FuturesRiskCheck::config rc_cfg_{};
    MaintenanceMarginTable mm_table_{};
    std::shared_ptr<IRiskCheck> risk_check_;

    std::int64_t dead_man_countdown_ms_ = 0;
    std::int64_t dead_man_heartbeat_ms_ = 0;
    bool dms_attempt_position_close_ = false;

    lifecycle state_ = lifecycle::closed;
};

#endif // HAS_BYBIT
