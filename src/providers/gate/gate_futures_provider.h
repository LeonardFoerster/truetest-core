#pragma once
#ifdef HAS_GATE

// Gate.io USDT-M futures provider.
// Phase 1: public market data + HybridExecutor / TradeTapeShadowAdapter.
// Phase 2: REST client + clock skew + contract probe → instrument_spec.
// Phase 3+: live safety / execution bridge.

#include "engine/engine_config.h"
#include "execution/instrument.h"
#include "execution/trade_tape_shadow_adapter.h"
#include "orderbook/orderbook.h"
#include "providers/gate/gate_backfill.h"
#include "providers/gate/gate_combined_parser.h"
#include "providers/gate/gate_combined_transport.h"
#include "providers/gate/gate_endpoints.h"
#include "providers/gate/gate_hybrid_executor.h"
#include "providers/gate/gate_parser.h"
#include "providers/gate/gate_rest_client.h"
#include "providers/gate/gate_time_sync.h"
#include "providers/gate/gate_transport.h"
#include "providers/prepend_transport.h"
#include "providers/provider.h"
#include "risk/futures_risk_check.h"
#include "risk/maintenance_margin_table.h"

#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

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

    lifecycle lifecycle_state() const override { return lifecycle_; }

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
        backfill_bars_ = cfg.backfill_bars;
        backfill_interval_ = cfg.backfill_interval;
        backfill_host_override_ = cfg.backfill_host;
        seed_ = cfg.seed;
        dashboard_ = cfg.dashboard;
        if (paper_exec_)
            paper_exec_->set_dashboard(dashboard_);
        configured_ = true;
    }

    bool open() override
    {
        lifecycle_ = lifecycle::opening;

        // Risk check before mode dispatch — applies in shadow/paper/live.
        if (max_notional_usdt_ > 0.0 || max_leverage_ > 0.0
            || min_liquidation_distance_pct_ > 0.0)
        {
            FuturesRiskCheck::config rc_cfg;
            rc_cfg.max_notional_usdt = max_notional_usdt_;
            rc_cfg.max_leverage = max_leverage_;
            rc_cfg.min_liquidation_distance_pct =
                min_liquidation_distance_pct_;
            rc_cfg.maintenance_margin_pct = maintenance_margin_pct_;
            risk_check_ =
                std::make_shared<FuturesRiskCheck>(rc_cfg, mm_table_);
        }

        std::cerr << "  GateFuturesProvider: "
                  << (endpoints_.is_testnet ? "[TESTNET] " : "")
                  << "ws=" << endpoints_.ws_host << ":" << endpoints_.ws_port
                  << endpoints_.ws_path
                  << " rest=" << endpoints_.rest_host << ":"
                  << endpoints_.rest_port << " symbol=" << symbol_ << "\n";

        std::cerr << "  GateFuturesProvider: creating transport (stream="
                  << stream_type_ << ", depth="
                  << (depth_spec_.empty() ? "none" : depth_spec_) << ")\n";

        std::shared_ptr<IDataTransport> live_transport;
        if (depth_spec_.empty())
        {
            gate_transport_ = std::make_shared<GateTransport>(
                symbol_, stream_type_, endpoints_);
            live_transport = gate_transport_;
        }
        else
        {
            std::vector<std::string> streams;
            streams.reserve(2);
            streams.push_back(stream_type_);
            streams.push_back(depth_spec_);
            gate_combined_transport_ =
                std::make_shared<GateCombinedTransport>(symbol_, streams,
                                                        endpoints_);
            live_transport = gate_combined_transport_;
        }

        apply_halt_cb_to_transports();

        // REST candle backfill for kline streams → PrependTransport.
        std::vector<std::string> prepend;
        if (backfill_bars_ > 0 && is_kline_stream())
        {
            std::string interval = backfill_interval_;
            if (interval.empty())
                interval = kline_interval_from_stream();
            if (interval.empty())
                interval = "1m";
            interval = gate::normalize_candle_interval(interval);

            gate::endpoints bf_ep = endpoints_;
            if (!backfill_host_override_.empty())
                bf_ep.rest_host = backfill_host_override_;

            gate::GateBackfill backfiller(bf_ep);
            std::cerr << "  GateFuturesProvider: backfilling "
                      << backfill_bars_ << " bars for " << symbol_ << " ("
                      << interval << ") via " << bf_ep.rest_host << "...\n";
            auto bars =
                backfiller.fetch(symbol_, interval, backfill_bars_);
            std::cerr << "  GateFuturesProvider: backfill loaded "
                      << bars.size() << " bars\n";
            prepend = gate::GateBackfill::to_prepend_frames(
                bars, symbol_, interval);
        }

        if (!prepend.empty())
            transport_ = std::make_shared<PrependTransport>(
                live_transport, std::move(prepend));
        else
            transport_ = live_transport;

        paper_exec_ = std::make_shared<GatePaperExecutor>();
        paper_exec_->set_symbol(symbol_);
        if (auto dash = dashboard_.lock())
        {
            (void)dash;
            paper_exec_->set_dashboard(dashboard_);
        }
        if (fee_model_)
            paper_exec_->set_fee_model(fee_model_);

        if (mode_ == engine_mode::live && !api_key_.empty())
        {
            // Phase 2: REST client + clock skew + contract instrument probe.
            // Safety stack (reconciler/kill/DMS) and ExecutionBridge land in
            // Phase 3–4 — refuse live open after instrument cache is ready.
            if (!open_live_rest_and_instrument())
            {
                lifecycle_ = lifecycle::error;
                return false;
            }
            std::cerr << "  GateFuturesProvider: live execution/safety not "
                         "yet wired (Phase 3–4). Instrument probe ok "
                         "(quanto=" << quanto_multiplier_
                      << "); use shadow/paper until later phases.\n";
            lifecycle_ = lifecycle::error;
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
            hybrid_exec_ = std::make_shared<GateHybridExecutor>(
                paper_exec_, book, fee_model_, fill_model_, qty_scale_,
                spread_step_factor_, wire_latency_model_,
                maker_queue_model_);
            executor_ = hybrid_exec_;
        }

        if (!live_transport->open())
        {
            close();
            lifecycle_ = lifecycle::error;
            return false;
        }

        lifecycle_ = lifecycle::open;
        return true;
    }

    void close() override
    {
        if (transport_)
            transport_->close();
        if (gate_transport_)
            gate_transport_->close();
        if (gate_combined_transport_)
            gate_combined_transport_->close();
        lifecycle_ = lifecycle::closed;
    }

    std::shared_ptr<IDataTransport> get_transport() override
    {
        return transport_;
    }

    std::shared_ptr<IExecutionAdapter> get_execution_adapter() override
    {
        return executor_;
    }

    void on_mid_price(const std::string& /*symbol*/,
                      double mid_price) override
    {
        if (hybrid_exec_)
            hybrid_exec_->on_mid_price(mid_price);
        else if (paper_exec_)
            paper_exec_->set_last_price(mid_price);
    }

    std::shared_ptr<IRiskCheck> get_risk_check() override
    {
        return risk_check_;
    }

    void set_halt_callback(
        std::function<void(std::string_view reason)> cb) override
    {
        halt_cb_ = std::move(cb);
        apply_halt_cb_to_transports();
    }

    // Always advertise event stream so main.inc uses provider-agnostic
    // DataBridge path (no HAS_GATE branch required in main.inc).
    bool supports_event_stream() const override { return true; }

    std::shared_ptr<IDataParser<provider::event>>
    get_event_parser() override
    {
        return std::make_shared<GateCombinedParser>();
    }

    std::optional<instrument_spec>
    get_instrument(const std::string& symbol) const override
    {
        if (!instrument_spec_) return std::nullopt;
        if (!symbol.empty())
        {
            const auto want = gate::normalize_contract_symbol(symbol);
            const auto have =
                gate::normalize_contract_symbol(instrument_spec_->symbol);
            if (want != have) return std::nullopt;
        }
        return *instrument_spec_;
    }

    // Linear notional: |size| * quanto_multiplier * mark_price (USDT).
    double quanto_multiplier() const { return quanto_multiplier_; }

    // ── Config surface ────────────────────────────────────────────────────

    void set_endpoints(gate::endpoints ep) { endpoints_ = std::move(ep); }
    const gate::endpoints& endpoints() const { return endpoints_; }
    bool is_testnet() const { return endpoints_.is_testnet; }

    const std::string& symbol() const { return symbol_; }
    const std::string& stream_type() const { return stream_type_; }
    const std::string& api_key() const { return api_key_; }
    const std::string& api_secret() const { return api_secret_; }
    const std::string& user_id() const { return user_id_; }

    void set_depth_spec(std::string spec) { depth_spec_ = std::move(spec); }
    const std::string& depth_spec() const { return depth_spec_; }

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
    bool is_kline_stream() const
    {
        return stream_type_.find("kline") != std::string::npos
            || stream_type_.find("candle") != std::string::npos
            || stream_type_ == "candlesticks";
    }

    std::string kline_interval_from_stream() const
    {
        auto m = gate::map_stream_to_channel(stream_type_);
        return m.interval;
    }

    void apply_halt_cb_to_transports()
    {
        if (!halt_cb_)
            return;
        if (gate_transport_)
            gate_transport_->set_fatal_disconnect_callback(halt_cb_);
        if (gate_combined_transport_)
            gate_combined_transport_->set_fatal_disconnect_callback(
                halt_cb_);
    }

    // Phase 2 live checklist: REST + clock + contract → instrument_spec.
    // Returns false → caller sets lifecycle::error (fail-closed).
    bool open_live_rest_and_instrument()
    {
        if (api_secret_.empty())
        {
            std::cerr << "GateFuturesProvider: refusing to go live — "
                         "api_secret is required\n";
            return false;
        }

        rest_ = std::make_shared<GateRestClient>(
            endpoints_, api_key_, api_secret_);
        rest_->set_per_call_timeout(std::chrono::milliseconds(3000));

        if (!rest_->resync_clock_now())
        {
            std::cerr << "GateFuturesProvider: refusing to go live — "
                         "clock resync failed\n";
            return false;
        }

        auto check = gate::verify_clock_skew(*rest_);
        if (!check.ok)
        {
            std::cerr << "GateFuturesProvider: refusing to go live — "
                      << check.note << "\n";
            return false;
        }
        std::cerr << "  GateFuturesProvider: clock offset "
                  << check.offset_ms << " ms\n";

        const std::string sym = gate::normalize_contract_symbol(symbol_);
        const std::string path = gate::contract_path(endpoints_, sym);
        auto info = rest_->get_unsigned(path, "");
        if (!gate::is_http_success(info.status))
        {
            std::cerr << "GateFuturesProvider: refusing to go live — "
                         "contract probe HTTP " << info.status << " path="
                      << path << ": "
                      << gate::redact_for_log(
                             gate::truncate_for_log(info.body))
                      << "\n";
            return false;
        }

        auto probe = gate::parse_contract_response(info.body, sym);
        if (!probe.ok)
        {
            std::cerr << "GateFuturesProvider: refusing to go live — "
                      << probe.note << "\n";
            return false;
        }
        if (probe.spec.tick_size <= 0.0 || probe.spec.lot_size <= 0.0
            || probe.quanto_multiplier <= 0.0)
        {
            std::cerr << "GateFuturesProvider: refusing to go live — "
                         "instrument tick/lot/quanto must be > 0 "
                         "(tick=" << probe.spec.tick_size
                      << " lot=" << probe.spec.lot_size
                      << " quanto=" << probe.quanto_multiplier << ")\n";
            return false;
        }

        instrument_spec_ = probe.spec;
        if (instrument_spec_->symbol.empty())
            instrument_spec_->symbol = sym;
        quanto_multiplier_ = probe.quanto_multiplier;
        std::cerr << "  GateFuturesProvider: contract " << sym
                  << " tick=" << instrument_spec_->tick_size
                  << " lot=" << instrument_spec_->lot_size
                  << " min_qty=" << instrument_spec_->min_qty
                  << " quanto=" << quanto_multiplier_ << "\n";
        return true;
    }

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

    bool configured_ = false;
    engine_mode mode_ = engine_mode::backtest;
    std::shared_ptr<IFeeModel> fee_model_;
    std::shared_ptr<IFillModel> fill_model_;
    std::shared_ptr<ILatencyModel> wire_latency_model_;
    std::shared_ptr<IQueuePositionModel> queue_model_;
    std::shared_ptr<IQueueModel> maker_queue_model_;
    double qty_scale_ = 1e8;
    double spread_step_factor_ = 0.0001;
    int backfill_bars_ = 0;
    std::string backfill_interval_;
    std::string backfill_host_override_;
    std::uint64_t seed_ = 0;
    std::weak_ptr<truetest::ui::ConsoleDashboard> dashboard_;

    lifecycle lifecycle_ = lifecycle::closed;

    std::shared_ptr<IDataTransport> transport_;
    std::shared_ptr<GateTransport> gate_transport_;
    std::shared_ptr<GateCombinedTransport> gate_combined_transport_;

    std::shared_ptr<GatePaperExecutor> paper_exec_;
    std::shared_ptr<GateHybridExecutor> hybrid_exec_;
    std::shared_ptr<TradeTapeShadowAdapter> shadow_exec_;
    std::shared_ptr<IExecutionAdapter> executor_;

    std::shared_ptr<MaintenanceMarginTable> mm_table_;
    std::shared_ptr<IRiskCheck> risk_check_;

    std::shared_ptr<GateRestClient> rest_;
    std::optional<instrument_spec> instrument_spec_;
    double quanto_multiplier_ = 0.0;

    std::function<void(std::string_view)> halt_cb_;
};

#endif // HAS_GATE
