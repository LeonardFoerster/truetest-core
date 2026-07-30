#pragma once
#ifdef HAS_BYBIT

// ============================================================
// Bybit V5 linear USDT perpetual provider.
// Phase 1: public WS + TradeTapeShadowAdapter / hybrid paper.
// Phase 2: REST orders + private WS fills + ExecutionBridge.
// Phase 3: reconciler + kill-switch + local DMS / optional DCP.
// Phase 4: conditional SL/TP brackets + CLI env + testnet ritual.
// ============================================================

#include "core/event.h"
#include "engine/engine_config.h"
#include "execution/execution_bridge.h"
#include "execution/rate_limiter.h"
#include "execution/trade_tape_shadow_adapter.h"
#include "exits/bracket_adapter.h"
#include "orderbook/orderbook.h"
#include "providers/bybit/bybit_backfill.h"
#include "providers/bybit/bybit_combined_parser.h"
#include "providers/bybit/bybit_combined_transport.h"
#include "providers/bybit/bybit_endpoints.h"
#include "providers/bybit/bybit_futures_bracket_adapter.h"
#include "providers/bybit/bybit_futures_dead_mans_switch.h"
#include "providers/bybit/bybit_futures_kill_switch.h"
#include "providers/bybit/bybit_futures_order_encoder.h"
#include "providers/bybit/bybit_futures_reconciler.h"
#include "providers/bybit/bybit_futures_safety.h"
#include "providers/bybit/bybit_futures_user_data_parser.h"
#include "providers/bybit/bybit_hybrid_executor.h"
#include "providers/bybit/bybit_parser.h"
#include "providers/bybit/bybit_rest_client.h"
#include "providers/bybit/bybit_rest_order_transport.h"
#include "providers/bybit/bybit_time_sync.h"
#include "providers/bybit/bybit_transport.h"
#include "providers/bybit/bybit_user_data_transport.h"
#include "providers/prepend_transport.h"
#include "providers/provider.h"
#include "risk/futures_risk_check.h"
#include "risk/maintenance_margin_table.h"

#include <cctype>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

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
        fee_model_ = cfg.fee_model;
        fill_model_ = cfg.fill_model;
        wire_latency_model_ = cfg.wire_latency_model;
        queue_model_ = cfg.queue_position_model;
        maker_queue_model_ = cfg.maker_queue_model;
        qty_scale_ = cfg.qty_scale;
        spread_step_factor_ = cfg.spread_step_factor;
        seed_ = cfg.seed;
        dashboard_ = cfg.dashboard;
        backfill_bars_ = cfg.backfill_bars;
        backfill_interval_ = cfg.backfill_interval;
        backfill_host_override_ = cfg.backfill_host;
        if (paper_exec_)
            paper_exec_->set_dashboard(dashboard_);
        configured_ = true;
    }

    bool open() override
    {
        state_ = lifecycle::opening;

        if (rc_cfg_.max_notional_usdt > 0.0
            || rc_cfg_.max_leverage > 0.0
            || rc_cfg_.min_liquidation_distance_pct > 0.0)
        {
            risk_check_ = std::make_shared<FuturesRiskCheck>(rc_cfg_, mm_table_);
        }

        std::cerr << "  BybitFuturesProvider: "
                  << (endpoints_.is_demo ? "[DEMO] "
                      : endpoints_.is_testnet ? "[TESTNET] " : "")
                  << "ws=" << endpoints_.ws_public_host << ":" << endpoints_.ws_port
                  << endpoints_.ws_public_path
                  << " rest=" << endpoints_.rest_host
                  << " symbol=" << symbol_
                  << " stream=" << stream_type_
                  << " depth=" << (depth_stream_.empty() ? "none" : depth_stream_)
                  << "\n";

        std::shared_ptr<IDataTransport> live_transport;
        if (depth_stream_.empty())
        {
            bybit_transport_ = std::make_shared<BybitTransport>(
                symbol_, stream_type_, endpoints_);
            live_transport = bybit_transport_;
        }
        else
        {
            std::vector<std::string> streams;
            streams.reserve(2);
            streams.push_back(stream_type_);
            streams.push_back(depth_stream_);
            bybit_combined_transport_ =
                std::make_shared<BybitCombinedTransport>(
                    symbol_, streams, endpoints_);
            live_transport = bybit_combined_transport_;
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
                interval = "1";
            interval = bybit::normalize_kline_interval(interval);
            if (interval.empty())
                interval = "1";

            bybit::endpoints bf_ep = endpoints_;
            if (!backfill_host_override_.empty())
                bf_ep.rest_host = backfill_host_override_;

            bybit::BybitBackfill backfiller(bf_ep);
            std::cerr << "  BybitFuturesProvider: backfilling "
                      << backfill_bars_ << " bars for " << symbol_ << " ("
                      << interval << ") via " << bf_ep.rest_host << "...\n";
            auto bars =
                backfiller.fetch(symbol_, interval, backfill_bars_);
            std::cerr << "  BybitFuturesProvider: backfill loaded "
                      << bars.size() << " bars\n";
            prepend = bybit::BybitBackfill::to_prepend_frames(
                bars, symbol_, interval);
        }

        if (!prepend.empty())
            transport_ = std::make_shared<PrependTransport>(
                live_transport, std::move(prepend));
        else
            transport_ = live_transport;

        paper_exec_ = std::make_shared<BybitPaperExecutor>();
        paper_exec_->set_symbol(symbol_);
        if (auto dash = dashboard_.lock())
            paper_exec_->set_dashboard(dashboard_);
        if (fee_model_)
            paper_exec_->set_fee_model(fee_model_);

        if (mode_ == engine_mode::live && !api_key_.empty())
        {
            if (!open_live_path())
            {
                // Partial live setup must not leak: main.inc only installs
                // the close-guard after open() returns true.
                close();
                state_ = lifecycle::error;
                return false;
            }
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
            hybrid_exec_ = std::make_shared<BybitHybridExecutor>(
                paper_exec_, book, fee_model_, fill_model_,
                qty_scale_, spread_step_factor_, wire_latency_model_,
                maker_queue_model_);
            executor_ = hybrid_exec_;
        }

        if (!live_transport->open())
        {
            close();
            state_ = lifecycle::error;
            return false;
        }

        state_ = lifecycle::open;
        return true;
    }

    void close() override
    {
        // Stop heartbeat first so it cannot refresh while we disarm.
        // Disarm is best-effort; failure leaves any DCP timer to expire.
        if (dms_)
        {
            dms_->stop();
            if (!dms_->disarm())
                std::cerr << "BybitFuturesProvider: dead-man's-switch "
                             "disarm failed; relying on DCP countdown to "
                             "expire server-side (if armed).\n";
        }
        if (bridge_)
            bridge_->close();
        if (bybit_user_data_)
            bybit_user_data_->close();
        if (transport_)
            transport_->close();
        risk_check_.reset();
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

    std::shared_ptr<IRiskCheck> get_risk_check() override
    {
        return risk_check_;
    }

    std::shared_ptr<IReconciler> get_reconciler() override { return reconciler_; }

    std::shared_ptr<IKillSwitch> get_kill_switch() override { return kill_switch_; }

    std::shared_ptr<truetest::exits::IBracketAdapter>
    get_bracket_adapter() override
    {
        return bracket_adapter_;
    }

    std::vector<liveness_source> get_liveness_sources() override
    {
        std::vector<liveness_source> out;
        if (dms_)
        {
            // last_alive_ms points into dms_; WorkerWatchdog must stop
            // before provider close/destruction (engine shutdown order).
            // Deadline = 3 × heartbeat: tolerate one missed cycle before halt.
            liveness_source s;
            s.name = "bybit-futures-dms-heartbeat";
            s.last_alive_ms = &dms_->liveness_ts();
            s.deadline_ms = dms_->heartbeat_interval_ms() * 3;
            out.push_back(std::move(s));
        }
        return out;
    }

    std::optional<instrument_spec>
    get_instrument(const std::string& symbol) const override
    {
        if (!instrument_spec_) return std::nullopt;
        if (!symbol.empty() && upper(symbol) != upper(instrument_spec_->symbol)
            && upper(symbol) != upper(symbol_))
            return std::nullopt;
        return *instrument_spec_;
    }

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

    // Always advertise event stream so main.inc uses the provider-agnostic
    // DataBridge<path> without a HAS_BYBIT branch. Combined parser routes
    // publicTrade / orderbook / kline and multi-emits data[].
    bool supports_event_stream() const override { return true; }

    std::shared_ptr<IDataParser<provider::event>> get_event_parser() override
    {
        return std::make_shared<BybitCombinedParser>();
    }

private:
    static std::string upper(std::string s)
    {
        for (auto& c : s)
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return s;
    }

    void apply_halt_cb_to_transports()
    {
        if (!halt_cb_) return;
        if (bybit_transport_)
            bybit_transport_->set_fatal_disconnect_callback(halt_cb_);
        if (bybit_combined_transport_)
            bybit_combined_transport_->set_fatal_disconnect_callback(halt_cb_);
        if (bybit_user_data_)
            bybit_user_data_->set_fatal_disconnect_callback(halt_cb_);
    }

    bool is_kline_stream() const
    {
        const auto mapped = bybit::map_stream_to_topic(stream_type_);
        return mapped.topic.size() >= 5
            && mapped.topic.substr(0, 5) == "kline";
    }

    // "kline.1" / "kline1m" → interval token for REST / WS topic.
    std::string kline_interval_from_stream() const
    {
        const auto mapped = bybit::map_stream_to_topic(stream_type_);
        if (mapped.topic.size() > 6 && mapped.topic.substr(0, 6) == "kline.")
            return mapped.topic.substr(6);
        return {};
    }

    static void log_position_snapshot(const parsed_position_snapshot& s,
                                      const std::string& sym)
    {
        if (s.r == parsed_position_snapshot::reason::order)
            return; // fill-redundant
        for (const auto& p : s.positions)
        {
            if (!sym.empty() && p.symbol != sym && !p.symbol.empty())
                continue;
            std::cerr << "  [POSITION] " << (p.symbol.empty() ? sym : p.symbol)
                      << " qty=" << p.qty
                      << " side=" << p.position_side
                      << " margin=" << p.margin_type << "\n";
        }
    }

    // Live refuse checklist (Phase 2+3). Returns false → caller sets state=error.
    bool open_live_path()
    {
        if (api_secret_.empty())
        {
            std::cerr << "BybitFuturesProvider: refusing to go live — "
                         "api_secret is required\n";
            return false;
        }

        rest_ = std::make_shared<BybitRestClient>(
            api_key_, api_secret_, endpoints_);

        // Bound shared REST I/O so kill/DMS cannot stall forever.
        rest_->set_per_call_timeout(std::chrono::milliseconds(3000));

        if (!rest_->resync_clock_now())
        {
            std::cerr << "BybitFuturesProvider: refusing to go live — "
                         "clock resync failed\n";
            return false;
        }

        auto check = bybit::verify_clock_skew(*rest_);
        if (!check.ok)
        {
            std::cerr << "BybitFuturesProvider: refusing to go live — "
                      << check.note << "\n";
            return false;
        }

        const std::string sym = upper(symbol_);

        // Instruments probe — refuse if not ok OR tick/lot <= 0.
        {
            auto info = rest_->get_unsigned(
                bybit::paths::instruments_info,
                bybit::instruments_query("linear", sym));
            if (info.status < 200 || info.status >= 300)
            {
                std::cerr << "BybitFuturesProvider: refusing to go live — "
                             "instruments HTTP " << info.status << ": "
                          << bybit::truncate_for_log(info.body) << "\n";
                return false;
            }
            auto probe = bybit::parse_instruments_response(info.body, sym);
            if (!probe.ok)
            {
                std::cerr << "BybitFuturesProvider: refusing to go live — "
                          << probe.note << "\n";
                return false;
            }
            if (probe.spec.tick_size <= 0.0 || probe.spec.lot_size <= 0.0)
            {
                std::cerr << "BybitFuturesProvider: refusing to go live — "
                             "instrument tick_size/lot_size must be > 0 "
                             "(tick=" << probe.spec.tick_size
                          << " lot=" << probe.spec.lot_size << ")\n";
                return false;
            }
            instrument_spec_ = probe.spec;
            if (instrument_spec_->symbol.empty())
                instrument_spec_->symbol = sym;
        }

        // One-way mode gate + margin/liq advisories via position list.
        {
            const std::string pos_q = "category=linear&symbol=" + sym;
            auto pr = rest_->get(bybit::paths::position_list, pos_q);
            if (pr.status < 200 || pr.status >= 300
                || (!pr.business_ok
                    && !bybit::is_business_success(pr.status, pr.body)))
            {
                std::cerr << "BybitFuturesProvider: refusing to go live — "
                             "position/list probe HTTP " << pr.status << ": "
                          << bybit::truncate_for_log(pr.body) << "\n";
                return false;
            }

            auto hedge_err =
                bybit::futures::check_one_way_position_mode(pr.body, sym);
            if (!hedge_err.empty())
            {
                std::cerr << "BybitFuturesProvider: refusing to go live — "
                          << hedge_err << "\n";
                return false;
            }

            auto advisories = bybit::futures::compute_advisories(
                pr.body, sym, expected_margin_type_, liquidation_warn_pct_);
            for (const auto& a : advisories)
                std::cerr << "  [ADVISORY] " << a.note << "\n";
            if (auto refuse = bybit::futures::first_strict_refusal(
                    advisories, margin_type_strict_))
            {
                std::cerr << "BybitFuturesProvider: refusing to go live — "
                          << *refuse << "\n";
                return false;
            }
        }

        minter_ = std::make_shared<bybit::ShortOrderLinkIdMinter>(seed_);

        // Conservative place-order rate: capacity 10, refill 10/s (~10/s).
        order_rate_limiter_ = std::make_shared<TokenBucketRateLimiter>(
            /*capacity=*/10.0, /*refill_per_sec=*/10.0);

        reconciler_ = std::make_shared<BybitFuturesReconciler>(
            rest_, sym, "linear", endpoints_.is_demo);

        kill_switch_ = make_bybit_futures_kill_switch(
            rest_, sym, minter_, "linear");

        bracket_adapter_ = make_bybit_futures_bracket_adapter(rest_, "linear");

        ExecutionBridge::deps d;
        d.order_tx = make_bybit_rest_order_transport(rest_);
        bybit_user_data_ = std::make_shared<BybitUserDataTransport>(
            api_key_, api_secret_, endpoints_);
        bybit_user_data_->set_time_offset_ms(rest_->clock_offset_ms());
        d.fill_tx = bybit_user_data_;
        apply_halt_cb_to_transports();

        d.encoder = std::make_shared<BybitFuturesOrderEncoder>(symbol_);
        d.parser = std::make_shared<BybitFuturesUserDataParser>();
        d.order_rate_limiter = order_rate_limiter_;
        d.client_id_fn = [m = minter_](uint64_t) { return m->next(); };

        // Position/wallet snapshots: log + funding → event_publisher.
        d.position_snapshot_handler =
            [this, sym](const parsed_position_snapshot& s) {
                log_position_snapshot(s, sym);
                if (s.r != parsed_position_snapshot::reason::funding_fee)
                    return;
                for (const auto& b : s.balances)
                {
                    if (b.asset != "USDT" && b.asset != "usdt")
                        continue;
                    if (b.balance_change == 0.0)
                        continue;
                    auto fe = std::make_shared<funding_event>(
                        s.ts, sym, 0.0, b.balance_change, "FUNDING_FEE");
                    if (event_publisher_)
                        event_publisher_(fe);
                    else
                        std::cerr << "  [FUNDING] " << sym
                                  << " cash_delta=" << b.balance_change
                                  << " (no publisher wired yet)\n";
                }
            };

        bridge_ = std::make_shared<ExecutionBridge>(std::move(d));
        if (!bridge_->open())
        {
            std::cerr << "BybitFuturesProvider: ExecutionBridge open "
                         "failed: " << bridge_->last_error() << "\n";
            return false;
        }
        executor_ = bridge_;

        // Dead-man's switch — last to arm, first to disarm. Local-only by
        // default (DCP is institutional); caveats logged inside DMS ctor.
        if (dead_man_countdown_ms_ > 0)
        {
            const int64_t hb = dead_man_heartbeat_ms_;

            BybitFuturesDeadMansSwitch::close_position_fn closer = nullptr;
            if (dms_attempt_position_close_)
            {
                closer = [rest = rest_,
                          minter = minter_,
                          sym]()
                {
                    if (!rest) return;
                    auto ks = make_bybit_futures_kill_switch(
                        rest, sym, minter, "linear");
                    // Short deadline: DMS last-resort, not orderly kill.
                    if (ks->cancel_all_and_flatten(
                            std::chrono::milliseconds(4000)))
                    {
                        std::cerr << "  [DMS-CLOSE] cancel+flatten OK "
                                  << sym << "\n";
                    }
                    else
                    {
                        std::cerr << "  [DMS-CLOSE] cancel+flatten FAILED "
                                  << sym << "\n";
                    }
                };
            }

            dms_ = make_bybit_futures_dead_mans_switch(
                rest_, dead_man_countdown_ms_, hb,
                /*attempt_close=*/dms_attempt_position_close_,
                /*closer=*/std::move(closer),
                /*enable_dcp=*/false);
            if (!dms_->start())
            {
                std::cerr << "BybitFuturesProvider: dead-man's switch "
                             "failed to arm — refusing to go live.\n";
                bridge_->close();
                return false;
            }
            std::cerr << "  BybitFuturesProvider: dead-man's switch armed "
                         "(countdown=" << dms_->countdown_sec()
                      << "s, heartbeat=" << dms_->heartbeat_interval_ms()
                      << "ms";
            if (dms_attempt_position_close_)
                std::cerr << ", position-close=ON";
            if (dms_->dcp_armed())
                std::cerr << ", DCP=armed";
            else
                std::cerr << ", DCP=off/local";
            std::cerr << ")\n";
        }

        std::cerr << "  BybitFuturesProvider: live path ready "
                  << (endpoints_.is_demo ? "[DEMO] "
                      : endpoints_.is_testnet ? "[TESTNET] " : "")
                  << "symbol=" << symbol_
                  << " tick=" << (instrument_spec_ ? instrument_spec_->tick_size : 0.0)
                  << " lot=" << (instrument_spec_ ? instrument_spec_->lot_size : 0.0)
                  << " (kill-switch ready"
                  << (dms_ ? ", DMS armed" : ", DMS off") << ")\n";
        return true;
    }

    std::string symbol_;
    std::string stream_type_;
    std::string api_key_;
    std::string api_secret_;
    std::string depth_stream_;
    bybit::endpoints endpoints_;

    engine_mode mode_ = engine_mode::backtest;
    std::shared_ptr<IFeeModel> fee_model_;
    std::shared_ptr<IFillModel> fill_model_;
    std::shared_ptr<ILatencyModel> wire_latency_model_;
    std::shared_ptr<IQueuePositionModel> queue_model_;
    std::shared_ptr<IQueueModel> maker_queue_model_;
    double qty_scale_ = 1e8;
    double spread_step_factor_ = 0.0001;
    std::uint64_t seed_ = 0;
    bool configured_ = false;
    std::weak_ptr<truetest::ui::ConsoleDashboard> dashboard_;

    int backfill_bars_ = 0;
    std::string backfill_interval_;
    std::string backfill_host_override_;

    std::string expected_margin_type_;
    double liquidation_warn_pct_ = 5.0;
    bool margin_type_strict_ = false;

    FuturesRiskCheck::config rc_cfg_{};
    std::shared_ptr<truetest::risk::MaintenanceMarginTable> mm_table_;
    std::shared_ptr<IRiskCheck> risk_check_;

    std::int64_t dead_man_countdown_ms_ = 0;
    std::int64_t dead_man_heartbeat_ms_ = 0;
    bool dms_attempt_position_close_ = false;

    lifecycle state_ = lifecycle::closed;

    std::shared_ptr<IDataTransport> transport_;
    std::shared_ptr<BybitTransport> bybit_transport_;
    std::shared_ptr<BybitCombinedTransport> bybit_combined_transport_;

    std::shared_ptr<BybitPaperExecutor> paper_exec_;
    std::shared_ptr<BybitHybridExecutor> hybrid_exec_;
    std::shared_ptr<TradeTapeShadowAdapter> shadow_exec_;
    std::shared_ptr<ExecutionBridge> bridge_;
    std::shared_ptr<IExecutionAdapter> executor_;

    // Live-only
    std::shared_ptr<BybitRestClient> rest_;
    std::shared_ptr<bybit::ShortOrderLinkIdMinter> minter_;
    std::shared_ptr<TokenBucketRateLimiter> order_rate_limiter_;
    std::shared_ptr<BybitUserDataTransport> bybit_user_data_;
    std::optional<instrument_spec> instrument_spec_;
    std::shared_ptr<BybitFuturesReconciler> reconciler_;
    std::shared_ptr<BybitFuturesKillSwitch> kill_switch_;
    std::shared_ptr<truetest::exits::IBracketAdapter> bracket_adapter_;
    std::shared_ptr<BybitFuturesDeadMansSwitch> dms_;

    std::function<void(std::string_view)> halt_cb_;
    std::function<void(std::shared_ptr<event>)> event_publisher_;
};

#endif // HAS_BYBIT
