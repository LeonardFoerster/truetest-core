#pragma once
#ifdef HAS_BITGET

// Bitget UTA v3 USDT-M futures provider — Phase 0 non-live.
// Public market data + HybridExecutor / TradeTapeShadowAdapter.
// Live order routing is refused until Tasks 6–10 wire REST + private WS.

#include "engine/engine_config.h"
#include "execution/trade_tape_shadow_adapter.h"
#include "orderbook/orderbook.h"
#include "providers/bitget/bitget_combined_transport.h"
#include "providers/bitget/bitget_endpoints.h"
#include "providers/bitget/bitget_hybrid_executor.h"
#include "providers/bitget/bitget_parser.h"
#include "providers/bitget/bitget_transport.h"
#include "providers/provider.h"
#include "risk/futures_risk_check.h"
#include "risk/maintenance_margin_table.h"

#include <cctype>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

class BitgetFuturesProvider : public IProvider
{
public:
    BitgetFuturesProvider(
        const std::string& symbol,
        const std::string& stream_type,
        const std::string& api_key = "",
        const std::string& api_secret = "",
        const std::string& api_passphrase = "",
        const std::string& host = "ws.bitget.com",
        const std::string& port = "443")
        : symbol_(symbol)
        , stream_type_(stream_type)
        , api_key_(api_key)
        , api_secret_(api_secret)
        , api_passphrase_(api_passphrase)
        , host_(host)
        , port_(port)
        , endpoints_(bitget::uta_mainnet())
    {
        if (!host.empty()) endpoints_.ws_public_host = host;
        if (!port.empty()) endpoints_.ws_port = port;
    }

    void set_depth_stream(const std::string& depth_stream)
    {
        depth_stream_ = depth_stream;
    }

    const std::string& depth_stream() const { return depth_stream_; }

    void set_category(std::string category)
    {
        category_ = std::move(category);
    }

    void set_api_surface(std::string surface)
    {
        api_surface_ = std::move(surface);
    }

    // Operator-set advisory inputs (stored for live Tasks 9+).
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

    // Position-based pre-trade risk caps. 0 disables each; all-zero means
    // no FuturesRiskCheck is constructed.
    void set_max_notional_usdt(double v)             { rc_cfg_.max_notional_usdt = v; }
    void set_max_leverage(double v)                  { rc_cfg_.max_leverage = v; }
    void set_min_liquidation_distance_pct(double v)  { rc_cfg_.min_liquidation_distance_pct = v; }
    void set_maintenance_margin_pct(double v)        { rc_cfg_.maintenance_margin_pct = v; }

    // DMS knobs stored for Task 10; unused in Phase 0 non-live.
    void set_dead_man_countdown_ms(int64_t v)        { dead_man_countdown_ms_ = v; }
    void set_dead_man_heartbeat_ms(int64_t v)        { dead_man_heartbeat_ms_ = v; }
    void set_dms_attempt_position_close(bool v)      { dms_attempt_position_close_ = v; }

    void set_endpoints(bitget::endpoints ep)
    {
        endpoints_ = std::move(ep);
        host_ = endpoints_.ws_public_host;
        port_ = endpoints_.ws_port;
    }

    bool is_demo() const { return endpoints_.is_demo; }

    std::string name() const override { return "bitget-futures"; }

    bool has_data_feed() const override { return true; }
    bool has_execution() const override { return true; }

    lifecycle lifecycle_state() const override { return state_; }

    void configure(const engine_config& cfg) override
    {
        mode_ = cfg.mode;
        fee_model_ = cfg.fee_model;
        fill_model_ = cfg.fill_model;
        wire_latency_model_ = cfg.wire_latency_model;
        queue_model_ = cfg.queue_position_model;
        maker_queue_model_ = cfg.maker_queue_model;
        qty_scale_ = cfg.qty_scale;
        spread_step_factor_ = cfg.spread_step_factor;
        seed_ = cfg.seed;
        dashboard_ = cfg.dashboard;
        if (paper_exec_)
            paper_exec_->set_dashboard(dashboard_);
        configured_ = true;
    }

    bool open() override
    {
        state_ = lifecycle::opening;

        // Risk check before mode dispatch — applies in shadow/paper/live.
        if (rc_cfg_.max_notional_usdt > 0.0
            || rc_cfg_.max_leverage > 0.0
            || rc_cfg_.min_liquidation_distance_pct > 0.0)
        {
            risk_check_ = std::make_shared<FuturesRiskCheck>(rc_cfg_, mm_table_);
        }

        std::cerr << "  BitgetFuturesProvider: "
                  << (endpoints_.is_demo ? "[DEMO] " : "")
                  << "ws=" << endpoints_.ws_public_host << ":" << endpoints_.ws_port
                  << endpoints_.ws_public_path
                  << " rest=" << endpoints_.rest_host << ":" << endpoints_.rest_port
                  << " category=" << category_
                  << "\n";

        std::cerr << "  BitgetFuturesProvider: creating transport (stream="
                  << stream_type_
                  << ", depth=" << (depth_stream_.empty() ? "none" : depth_stream_)
                  << ")\n";

        std::shared_ptr<IDataTransport> live_transport;
        if (depth_stream_.empty())
        {
            bitget_transport_ = std::make_shared<BitgetTransport>(
                symbol_, stream_type_, endpoints_);
            live_transport = bitget_transport_;
        }
        else
        {
            std::vector<std::string> streams;
            streams.reserve(2);
            streams.push_back(stream_type_);
            streams.push_back(depth_stream_);
            bitget_combined_transport_ = std::make_shared<BitgetCombinedTransport>(
                symbol_, streams, endpoints_);
            live_transport = bitget_combined_transport_;
        }

        apply_halt_cb_to_transports();

        // Backfill skipped in Phase 0 (BitgetBackfill lands with REST Task 6).
        transport_ = live_transport;

        paper_exec_ = std::make_shared<BitgetPaperExecutor>();
        paper_exec_->set_symbol(symbol_);
        if (auto dash = dashboard_.lock())
            paper_exec_->set_dashboard(dashboard_);
        if (fee_model_)
            paper_exec_->set_fee_model(fee_model_);

        // Live + credentials: refuse until Tasks 6–10 wire REST/private WS.
        // Live without keys falls through to hybrid (match BinanceFuturesProvider).
        if (mode_ == engine_mode::live && !api_key_.empty())
        {
            std::cerr << "BitgetFuturesProvider: Bitget live path not yet "
                         "wired (Task 6-10)\n";
            state_ = lifecycle::error;
            return false;
        }
        else if (mode_ == engine_mode::shadow)
        {
            shadow_exec_ = std::make_shared<TradeTapeShadowAdapter>(
                wire_latency_model_, fee_model_);
            if (queue_model_)
                shadow_exec_->set_queue_model(queue_model_);
            executor_ = shadow_exec_;
        }
        else
        {
            // paper / backtest / live-without-keys
            auto book = std::make_shared<orderbook>();
            hybrid_exec_ = std::make_shared<BitgetHybridExecutor>(
                paper_exec_, book, fee_model_, fill_model_,
                qty_scale_, spread_step_factor_, wire_latency_model_,
                maker_queue_model_);
            executor_ = hybrid_exec_;
        }

        if (!live_transport->open())
        {
            state_ = lifecycle::error;
            return false;
        }

        state_ = lifecycle::open;
        return true;
    }

    void close() override
    {
        if (transport_) transport_->close();
        state_ = lifecycle::closed;
    }

    std::shared_ptr<IDataTransport> get_transport() override
    {
        return transport_;
    }

    std::shared_ptr<IExecutionAdapter> get_execution_adapter() override
    {
        return executor_;
    }

    void on_mid_price(const std::string& /*symbol*/, double mid_price) override
    {
        if (hybrid_exec_)
            hybrid_exec_->on_mid_price(mid_price);
        else if (paper_exec_)
            paper_exec_->set_last_price(mid_price);
    }

    std::shared_ptr<IRiskCheck> get_risk_check() override { return risk_check_; }

    void set_halt_callback(
        std::function<void(std::string_view reason)> cb) override
    {
        halt_cb_ = std::move(cb);
        apply_halt_cb_to_transports();
    }

    void set_event_publisher(
        std::function<void(std::shared_ptr<event>)> fn) override
    {
        event_publisher_ = std::move(fn);
    }

    bool supports_event_stream() const override
    {
        return !depth_stream_.empty();
    }

    // Phase 0: BitgetCombinedParser returns first trade only per publicTrade
    // frame (multi-trade data[] → see bitget::parse_all_trades). Acceptable
    // for Phase 0; Task 5+ may batch-emit if needed.
    std::shared_ptr<IDataParser<provider::event>> get_event_parser() override
    {
        if (depth_stream_.empty()) return nullptr;
        return std::make_shared<BitgetCombinedParser>();
    }

    const std::string& symbol() const { return symbol_; }
    const std::string& stream_type() const { return stream_type_; }
    const std::string& category() const { return category_; }
    const std::string& api_passphrase() const { return api_passphrase_; }

private:
    std::string symbol_;
    std::string stream_type_;
    std::string api_key_;
    std::string api_secret_;
    std::string api_passphrase_; // stored for live Tasks 6+; unused non-live
    std::string host_;
    std::string port_;
    bitget::endpoints endpoints_;
    std::string category_ = "USDT-FUTURES";
    std::string api_surface_ = "uta";

    bool configured_ = false;
    engine_mode mode_ = engine_mode::backtest;
    std::shared_ptr<IFeeModel> fee_model_;
    std::shared_ptr<IFillModel> fill_model_;
    std::shared_ptr<ILatencyModel> wire_latency_model_;
    std::shared_ptr<IQueuePositionModel> queue_model_;
    std::shared_ptr<IQueueModel> maker_queue_model_;
    double qty_scale_ = 1e8;
    double spread_step_factor_ = 0.0001;

    lifecycle state_ = lifecycle::closed;

    std::shared_ptr<IDataTransport> transport_;
    std::shared_ptr<BitgetTransport> bitget_transport_;
    std::shared_ptr<BitgetCombinedTransport> bitget_combined_transport_;

    std::shared_ptr<BitgetPaperExecutor> paper_exec_;
    std::shared_ptr<BitgetHybridExecutor> hybrid_exec_;
    std::shared_ptr<TradeTapeShadowAdapter> shadow_exec_;
    std::shared_ptr<IExecutionAdapter> executor_;

    std::uint64_t seed_ = 0;
    std::string depth_stream_;
    std::weak_ptr<truetest::ui::ConsoleDashboard> dashboard_;

    std::string expected_margin_type_;
    double      liquidation_warn_pct_ = 0.05;
    bool        margin_type_strict_   = false;

    FuturesRiskCheck::config rc_cfg_{};
    std::shared_ptr<IRiskCheck> risk_check_;
    std::shared_ptr<truetest::risk::MaintenanceMarginTable> mm_table_;

    int64_t dead_man_countdown_ms_ = 0;
    int64_t dead_man_heartbeat_ms_ = 0;
    bool    dms_attempt_position_close_ = false;

    std::function<void(std::string_view)> halt_cb_;
    std::function<void(std::shared_ptr<event>)> event_publisher_;

    void apply_halt_cb_to_transports()
    {
        if (!halt_cb_) return;
        if (bitget_transport_)
            bitget_transport_->set_fatal_disconnect_callback(halt_cb_);
        if (bitget_combined_transport_)
            bitget_combined_transport_->set_fatal_disconnect_callback(halt_cb_);
    }
};

#endif // HAS_BITGET
