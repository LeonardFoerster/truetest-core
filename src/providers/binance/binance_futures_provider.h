#pragma once
#ifdef HAS_BINANCE

#include "engine/engine_config.h"
#include "execution/client_order_id.h"
#include "execution/execution_bridge.h"
#include "execution/rate_limiter.h"
#include "execution/trade_tape_shadow_adapter.h"
#include "risk/futures_risk_check.h"
#include "providers/provider.h"
#include "providers/prepend_transport.h"
#include "providers/binance/binance_combined_parser.h"
#include "providers/binance/binance_combined_transport.h"
#include "providers/binance/binance_transport.h"
#include "providers/binance/binance_executor.h"
#include "providers/binance/binance_backfill.h"
#include "providers/binance/binance_endpoints.h"
#include "providers/binance/binance_futures_bracket_adapter.h"
#include "providers/binance/binance_futures_dead_mans_switch.h"
#include "providers/binance/binance_futures_kill_switch.h"
#include "providers/binance/binance_futures_order_encoder.h"
#include "providers/binance/binance_futures_reconciler.h"
#include "providers/binance/binance_futures_safety.h"
#include "providers/binance/binance_futures_user_data_parser.h"
#include "providers/binance/binance_rest_client.h"
#include "providers/binance/binance_rest_order_transport.h"
#include "providers/binance/binance_time_sync.h"
#include "providers/binance/binance_user_data_transport.h"
#include "providers/binance/hybrid_executor.h"
#include "orderbook/orderbook.h"

#include <cctype>
#include <cstdio>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// USDT-M futures sibling of BinanceProvider. This step (PR 2) wires up
// streaming data + paper/shadow execution only. Live order routing is
// gated off and refused at open() until the next step adds the futures
// REST client, reconciler, kill switch and bracket adapter.
class BinanceFuturesProvider : public IProvider
{
public:
    BinanceFuturesProvider(
        const std::string& symbol,
        const std::string& stream_type,
        const std::string& api_key = "",
        const std::string& api_secret = "",
        const std::string& host = "fstream.binance.com",
        const std::string& port = "9443")
        : symbol_(symbol)
        , stream_type_(stream_type)
        , api_key_(api_key)
        , api_secret_(api_secret)
        , host_(host)
        , port_(port)
        , endpoints_(binance::usdm_mainnet())
    {
        if (!host.empty()) endpoints_.ws_host = host;
        if (!port.empty()) endpoints_.ws_port = port;
    }

    void set_depth_stream(const std::string& depth_stream_suffix)
    {
        depth_stream_ = depth_stream_suffix;
    }

    const std::string& depth_stream() const { return depth_stream_; }

    // Operator-set advisory inputs. Empty / non-positive disables the
    // matching check; defaults run the liquidation-distance check at
    // 5% so it's opt-out, not opt-in (drowning is a harder condition
    // to opt back into noticing).
    void set_expected_margin_type(std::string mt)
    {
        expected_margin_type_ = std::move(mt);
    }
    void set_liquidation_warn_pct(double pct)
    {
        liquidation_warn_pct_ = pct;
    }
    // When true, a margin-mode mismatch is escalated from advisory
    // (warning) to refusal (open() returns false). For shops that have
    // made a deliberate margin-type choice, the unannounced flip from
    // ISOLATED → CROSSED in the Binance UI is a real config error
    // worth halting on.
    void set_margin_type_strict(bool strict)
    {
        margin_type_strict_ = strict;
    }

    // Position-based pre-trade risk caps. 0 disables each individually;
    // all-zero (the default) means no FuturesRiskCheck is constructed
    // and get_risk_check() returns nullptr (engine skips the check).
    void set_max_notional_usdt(double v)             { rc_cfg_.max_notional_usdt = v; }
    void set_max_leverage(double v)                  { rc_cfg_.max_leverage = v; }
    void set_min_liquidation_distance_pct(double v)  { rc_cfg_.min_liquidation_distance_pct = v; }
    void set_maintenance_margin_pct(double v)        { rc_cfg_.maintenance_margin_pct = v; }

    // Dead-man's switch. countdown_ms == 0 disables. heartbeat_ms == 0
    // means "use countdown / 3" so a single missed beat tolerates one
    // network flap without the venue auto-cancelling.
    void set_dead_man_countdown_ms(int64_t v)        { dead_man_countdown_ms_ = v; }
    void set_dead_man_heartbeat_ms(int64_t v)        { dead_man_heartbeat_ms_ = v; }

    void set_endpoints(binance::endpoints ep)
    {
        endpoints_ = std::move(ep);
        host_ = endpoints_.ws_host;
        port_ = endpoints_.ws_port;
    }

    bool is_testnet() const { return endpoints_.is_testnet; }

    std::string name() const override { return "binance-futures"; }

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
        backfill_bars_ = cfg.backfill_bars;
        backfill_interval_ = cfg.backfill_interval;
        backfill_host_override_ = cfg.backfill_host;
        seed_ = cfg.seed;
        dashboard_ = cfg.dashboard;
        if (binance_exec_)
            binance_exec_->set_dashboard(dashboard_);
        configured_ = true;
    }

    bool open() override
    {
        state_ = lifecycle::opening;

        // Construct the futures risk check before mode dispatch so it
        // applies in shadow / paper / live alike. The engine queries
        // get_risk_check() once after configure() — we need this set
        // by the time open() returns.
        if (rc_cfg_.max_notional_usdt > 0.0
            || rc_cfg_.max_leverage > 0.0
            || rc_cfg_.min_liquidation_distance_pct > 0.0)
        {
            risk_check_ = std::make_shared<FuturesRiskCheck>(rc_cfg_);
        }

        std::cerr << "  BinanceFuturesProvider: "
                  << (endpoints_.is_testnet ? "[TESTNET] " : "")
                  << "ws=" << endpoints_.ws_host << ":" << endpoints_.ws_port
                  << " rest=" << endpoints_.rest_host << ":" << endpoints_.rest_port
                  << "\n";

        std::shared_ptr<IDataTransport> live_transport;
        if (depth_stream_.empty())
        {
            binance_transport_ = std::make_shared<BinanceTransport>(
                symbol_, stream_type_, endpoints_.ws_host, endpoints_.ws_port);
            live_transport = binance_transport_;
        }
        else
        {
            const std::string sym_lower = lower(symbol_);
            std::vector<std::string> streams;
            streams.reserve(2);
            streams.push_back(sym_lower + "@" + stream_type_);
            streams.push_back(sym_lower + "@" + depth_stream_);
            binance_combined_transport_ = std::make_shared<BinanceCombinedTransport>(
                streams, endpoints_.ws_host, endpoints_.ws_port);
            live_transport = binance_combined_transport_;
        }
        apply_halt_cb_to_transports();

        std::string rest_host = rest_host_for_stream();

        std::vector<std::string> prepend;
        if (backfill_bars_ > 0 && is_kline_stream())
        {
            std::string interval = backfill_interval_;
            if (interval.empty())
                interval = kline_interval_from_stream();
            if (interval.empty())
                interval = "1m";

            BinanceBackfill backfiller(rest_host, "443", "/fapi/v1/klines");
            std::cerr << "  BinanceFuturesProvider: backfilling "
                      << backfill_bars_ << " bars for " << symbol_
                      << " (" << interval << ")...\n";

            auto bars = backfiller.fetch(symbol_, interval, backfill_bars_);
            std::cerr << "  BinanceFuturesProvider: backfill loaded "
                      << bars.size() << " bars\n";

            prepend.reserve(bars.size());
            for (const auto& b : bars)
                prepend.push_back(encode_kline_json(b, interval));
        }

        if (!prepend.empty())
            transport_ = std::make_shared<PrependTransport>(
                live_transport, std::move(prepend));
        else
            transport_ = live_transport;

        binance_exec_ = std::make_shared<BinanceExecutor>();
        binance_exec_->set_symbol(symbol_);
        if (auto dash = dashboard_.lock())
            binance_exec_->set_dashboard(dashboard_);
        if (fee_model_)
            binance_exec_->set_fee_model(fee_model_);

        if (mode_ == engine_mode::live && !api_key_.empty())
        {
            // Time path is the only routing-critical wiring difference
            // from spot — every other endpoint is hardcoded inside the
            // futures-specific bracket components.
            rest_ = std::make_shared<BinanceRestClient>(
                api_key_, api_secret_, rest_host, endpoints_.rest_port,
                "/fapi/v1/time");

            (void)rest_->resync_clock_now();

            auto check = binance::verify_clock_skew(*rest_);
            if (!check.ok)
            {
                std::cerr << "BinanceFuturesProvider: refusing to go live — "
                          << check.note << "\n";
                state_ = lifecycle::error;
                return false;
            }

            // Symbol existence probe; futures testnet's symbol set drifts
            // from prod and a typo otherwise surfaces as -1121 mid-stream.
            {
                auto info = rest_->get_unsigned(
                    "/fapi/v1/exchangeInfo", "symbol=" + upper(symbol_));
                if (info.status < 200 || info.status >= 300)
                {
                    std::cerr << "BinanceFuturesProvider: refusing to go "
                                 "live — symbol '" << upper(symbol_)
                              << "' not found on "
                              << (endpoints_.is_testnet ? "testnet " : "")
                              << "exchangeInfo (HTTP " << info.status
                              << "): " << info.body.substr(0, 160) << "\n";
                    state_ = lifecycle::error;
                    return false;
                }
            }

            // Hedge mode is the cleanest place to refuse: every order
            // would otherwise need a `positionSide` argument the encoder
            // does not emit, and the engine's lot bookkeeping is built
            // around netted (one-way) positions. Operators must flip the
            // account back to one-way mode in the Binance UI.
            {
                auto resp = rest_->get("/fapi/v1/positionSide/dual", "");
                if (resp.status < 200 || resp.status >= 300)
                {
                    std::cerr << "BinanceFuturesProvider: refusing to go "
                                 "live — /fapi/v1/positionSide/dual HTTP "
                              << resp.status << ": "
                              << resp.body.substr(0, 160) << "\n";
                    state_ = lifecycle::error;
                    return false;
                }
                auto dual = binance::extract_sv_optional_bool(
                    resp.body, "dualSidePosition");
                if (!dual.has_value())
                {
                    // Malformed / missing field: refuse rather than
                    // proceed assuming one-way mode. A truncated WAF
                    // response or schema change would otherwise sneak
                    // a hedge-mode account past the gate silently.
                    std::cerr << "BinanceFuturesProvider: refusing to go "
                                 "live — /fapi/v1/positionSide/dual "
                                 "response missing or malformed "
                                 "dualSidePosition field: "
                              << resp.body.substr(0, 160) << "\n";
                    state_ = lifecycle::error;
                    return false;
                }
                if (*dual)
                {
                    std::cerr << "BinanceFuturesProvider: refusing to go "
                                 "live — account is in hedge mode "
                                 "(dualSidePosition=true). Switch to "
                                 "one-way mode in the Binance UI.\n";
                    state_ = lifecycle::error;
                    return false;
                }
            }

            // Advisories (warnings, not refusals): margin-mode mismatch
            // and liquidation-distance. Filtered to this provider's
            // symbol — multi-symbol cross-margin awareness is out of
            // scope. The reconciler will read positionRisk again later;
            // this call is the startup-time advisory pass.
            {
                auto pr = rest_->get("/fapi/v2/positionRisk",
                                     "symbol=" + upper(symbol_));
                if (pr.status >= 200 && pr.status < 300)
                {
                    auto advisories = binance::futures::compute_advisories(
                        pr.body, expected_margin_type_,
                        liquidation_warn_pct_);
                    for (const auto& a : advisories)
                        std::cerr << "  [ADVISORY] " << a.note << "\n";

                    if (auto refuse = binance::futures::first_strict_refusal(
                            advisories, margin_type_strict_))
                    {
                        std::cerr << "BinanceFuturesProvider: refusing to "
                                     "go live — --margin-type-strict and "
                                  << *refuse << "\n";
                        state_ = lifecycle::error;
                        return false;
                    }
                }
                else
                {
                    std::cerr << "BinanceFuturesProvider: positionRisk "
                                 "advisory probe HTTP " << pr.status
                              << " — skipping margin/liquidation checks "
                                 "(reconciler will retry)\n";
                }
            }

            minter_ = std::make_shared<ClientOrderIdMinter>("tt", seed_);

            // Spot's order rate is 50/10s; futures is more permissive
            // (~300/10s) but the conservative bucket avoids surprises and
            // matches what live spot has been validated against.
            order_rate_limiter_ = std::make_shared<TokenBucketRateLimiter>(
                /*capacity=*/50.0, /*refill_per_sec=*/5.0);

            reconciler_ = std::make_shared<BinanceFuturesReconciler>(
                rest_, upper(symbol_), endpoints_.is_testnet);
            kill_switch_ = std::make_shared<BinanceFuturesKillSwitch>(
                rest_, upper(symbol_), minter_);
            bracket_adapter_ = make_binance_futures_bracket_adapter(rest_);

            ExecutionBridge::deps d;
            d.order_tx = make_binance_rest_order_transport(rest_);
            binance_user_data_ = std::make_shared<BinanceUserDataTransport>(
                             rest_, endpoints_.ws_host, endpoints_.ws_port,
                             binance_keepalive_policy{},
                             "/fapi/v1/listenKey");
            d.fill_tx = binance_user_data_;
            apply_halt_cb_to_transports();
            d.encoder  = std::make_shared<BinanceFuturesOrderEncoder>(symbol_);
            d.parser   = std::make_shared<BinanceFuturesUserDataParser>();
            d.order_rate_limiter = order_rate_limiter_;
            d.client_id_fn = [m = minter_](uint64_t) { return m->next(); };
            // ACCOUNT_UPDATE handler: log to stderr, scoped to this
            // provider's symbol. Intentionally does NOT mutate the
            // local portfolio yet — snap-to-venue races with in-flight
            // orders the engine hasn't confirmed; that needs a design
            // pass before it becomes safe to take action automatically.
            // Surfacing the events is the value-add here.
            d.position_snapshot_handler =
                [sym = upper(symbol_)](const parsed_position_snapshot& s) {
                    log_position_snapshot(s, sym);
                };

            bridge_ = std::make_shared<ExecutionBridge>(std::move(d));
            if (!bridge_->open())
            {
                std::cerr << "BinanceFuturesProvider: ExecutionBridge open "
                             "failed: " << bridge_->last_error() << "\n";
                state_ = lifecycle::error;
                return false;
            }
            executor_ = bridge_;

            // Dead-man's switch — last to arm, first to disarm. The
            // venue countdown should bracket the live order routing so
            // a crash between bridge open and now doesn't go unprotected.
            if (dead_man_countdown_ms_ > 0)
            {
                const int64_t hb = dead_man_heartbeat_ms_ > 0
                    ? dead_man_heartbeat_ms_
                    : dead_man_countdown_ms_ / 3;
                dms_ = make_binance_futures_dead_mans_switch(
                    rest_, upper(symbol_),
                    dead_man_countdown_ms_, hb);
                if (!dms_->start())
                {
                    std::cerr << "BinanceFuturesProvider: dead-man's switch "
                                 "failed to arm — refusing to go live.\n";
                    bridge_->close();
                    state_ = lifecycle::error;
                    return false;
                }
                std::cerr << "  BinanceFuturesProvider: dead-man's switch "
                             "armed (countdown=" << dead_man_countdown_ms_
                          << "ms, heartbeat=" << hb << "ms)\n";
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
            auto book = std::make_shared<orderbook>();
            hybrid_exec_ = std::make_shared<HybridExecutor>(
                binance_exec_, book, fee_model_, fill_model_,
                qty_scale_, spread_step_factor_, wire_latency_model_,
                maker_queue_model_);
            executor_ = hybrid_exec_;
        }

        if (!live_transport->open()) {
            state_ = lifecycle::error;
            return false;
        }

        state_ = lifecycle::open;
        return true;
    }

    void close() override
    {
        // Stop the heartbeat thread first so it doesn't refresh the
        // countdown while we're trying to disarm. Then disarm — best
        // effort; failure logs but doesn't block shutdown, since the
        // server-side timer will expire on its own anyway.
        if (dms_)
        {
            dms_->stop();
            if (!dms_->disarm())
                std::cerr << "BinanceFuturesProvider: dead-man's-switch "
                             "disarm failed; relying on countdown to "
                             "expire server-side.\n";
        }
        if (transport_) transport_->close();
        if (bridge_)    bridge_->close();
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
        else if (binance_exec_)
            binance_exec_->set_last_price(mid_price);
    }

    std::shared_ptr<IReconciler> get_reconciler() override { return reconciler_; }
    std::shared_ptr<IKillSwitch> get_kill_switch() override { return kill_switch_; }
    std::shared_ptr<truetest::exits::IBracketAdapter> get_bracket_adapter() override
    {
        return bracket_adapter_;
    }
    std::shared_ptr<IRiskCheck> get_risk_check() override { return risk_check_; }

    void set_halt_callback(
        std::function<void(std::string_view reason)> cb) override
    {
        halt_cb_ = std::move(cb);
        apply_halt_cb_to_transports();
    }

    std::vector<liveness_source> get_liveness_sources() override
    {
        std::vector<liveness_source> out;
        if (dms_)
        {
            // Deadline = 3 × heartbeat: tolerate a single missed cycle
            // before halting the engine. Smaller multipliers spuriously
            // halt under transient network jitter; larger multipliers
            // let the heartbeat hang silently for longer than the venue
            // countdown, which is the very failure mode this exists to
            // catch.
            liveness_source s;
            s.name = "binance-futures-dms-heartbeat";
            s.last_alive_ms = &dms_->liveness_ts();
            s.deadline_ms = dms_->heartbeat_interval_ms() * 3;
            out.push_back(std::move(s));
        }
        return out;
    }

    bool supports_event_stream() const override
    {
        return !depth_stream_.empty();
    }

    std::shared_ptr<IDataParser<provider::event>> get_event_parser() override
    {
        if (depth_stream_.empty()) return nullptr;
        return std::make_shared<BinanceCombinedParser>();
    }

    const std::string& symbol() const { return symbol_; }
    const std::string& stream_type() const { return stream_type_; }

private:
    std::string symbol_;
    std::string stream_type_;
    std::string api_key_;
    std::string api_secret_;
    std::string host_;
    std::string port_;
    binance::endpoints endpoints_;

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

    lifecycle state_ = lifecycle::closed;

    std::shared_ptr<IDataTransport> transport_;

    std::shared_ptr<BinanceExecutor> binance_exec_;
    std::shared_ptr<HybridExecutor> hybrid_exec_;
    std::shared_ptr<TradeTapeShadowAdapter> shadow_exec_;
    std::shared_ptr<ExecutionBridge> bridge_;
    std::shared_ptr<IExecutionAdapter> executor_;

    // Live-only; null in shadow/backtest so accessors return nullptr
    // and the engine installs its Noop defaults.
    std::shared_ptr<BinanceRestClient> rest_;
    std::shared_ptr<ClientOrderIdMinter> minter_;
    std::shared_ptr<TokenBucketRateLimiter> order_rate_limiter_;
    std::shared_ptr<BinanceFuturesReconciler> reconciler_;
    std::shared_ptr<BinanceFuturesKillSwitch> kill_switch_;
    std::shared_ptr<BinanceFuturesBracketAdapter> bracket_adapter_;

    std::uint64_t seed_ = 0;
    std::string depth_stream_;
    std::weak_ptr<truetest::ui::ConsoleDashboard> dashboard_;

    std::string expected_margin_type_;        // "" disables check
    double      liquidation_warn_pct_ = 0.05; // 5%; <=0 disables check
    bool        margin_type_strict_   = false;

    FuturesRiskCheck::config rc_cfg_{};        // all zero = check skipped
    std::shared_ptr<IRiskCheck> risk_check_;   // null until open() if any cap > 0

    int64_t dead_man_countdown_ms_ = 0;        // 0 disables DMS
    int64_t dead_man_heartbeat_ms_ = 0;        // 0 = countdown / 3
    std::shared_ptr<BinanceFuturesDeadMansSwitch> dms_;

    // Concrete handles so set_halt_callback can route the engine's halt
    // hook into our WS transports — see BinanceProvider for the same
    // pattern.
    std::shared_ptr<BinanceTransport>          binance_transport_;
    std::shared_ptr<BinanceCombinedTransport>  binance_combined_transport_;
    std::shared_ptr<BinanceUserDataTransport>  binance_user_data_;
    std::function<void(std::string_view)>      halt_cb_;

    void apply_halt_cb_to_transports()
    {
        if (!halt_cb_) return;
        if (binance_transport_)
            binance_transport_->set_fatal_disconnect_callback(halt_cb_);
        if (binance_combined_transport_)
            binance_combined_transport_->set_fatal_disconnect_callback(halt_cb_);
        if (binance_user_data_)
            binance_user_data_->set_fatal_disconnect_callback(halt_cb_);
    }

    static std::string upper(const std::string& s)
    {
        std::string out(s);
        for (auto& c : out)
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return out;
    }

    static const char* reason_str(parsed_position_snapshot::reason r)
    {
        using R = parsed_position_snapshot::reason;
        switch (r)
        {
        case R::order:              return "ORDER";
        case R::funding_fee:        return "FUNDING_FEE";
        case R::adjustment:         return "ADJUSTMENT";
        case R::deposit:            return "DEPOSIT";
        case R::withdraw:           return "WITHDRAW";
        case R::margin_transfer:    return "MARGIN_TRANSFER";
        case R::margin_type_change: return "MARGIN_TYPE_CHANGE";
        case R::liquidation:        return "LIQUIDATION";
        case R::admin:              return "ADMIN";
        case R::unknown:            return "UNKNOWN";
        case R::other:              return "OTHER";
        }
        return "UNKNOWN";
    }

    // Logs the parts of an ACCOUNT_UPDATE that concern this provider's
    // symbol. ORDER-reason snapshots are quieter — they overlap with
    // ORDER_TRADE_UPDATE and would be noise if logged at the same level.
    static void log_position_snapshot(const parsed_position_snapshot& s,
                                      const std::string& provider_symbol)
    {
        for (const auto& p : s.positions)
        {
            if (p.symbol != provider_symbol) continue;
            if (s.r == parsed_position_snapshot::reason::order)
            {
                // Same fill we'll see via ORDER_TRADE_UPDATE; suppress
                // the position-row log here to keep the channel low-noise.
                continue;
            }
            std::fprintf(stderr,
                "  [POSITION-SNAPSHOT] %s reason=%s qty=%.8f margin=%s side=%s\n",
                p.symbol.c_str(), reason_str(s.r), p.qty,
                p.margin_type.c_str(), p.position_side.c_str());
        }
        for (const auto& b : s.balances)
        {
            // Balance changes for funding/adjustment/etc. are always
            // worth surfacing; ORDER-reason balance changes are the
            // commission side of a fill we already track.
            if (s.r == parsed_position_snapshot::reason::order) continue;
            std::fprintf(stderr,
                "  [POSITION-SNAPSHOT] %s reason=%s balance=%.8f delta=%.8f\n",
                b.asset.c_str(), reason_str(s.r),
                b.wallet_balance, b.balance_change);
        }
    }

    static std::string lower(const std::string& s)
    {
        std::string out(s);
        for (auto& c : out)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return out;
    }

    std::string rest_host_for_stream() const
    {
        if (!backfill_host_override_.empty())
            return backfill_host_override_;
        return endpoints_.rest_host;
    }

    bool is_kline_stream() const
    {
        return stream_type_.rfind("kline", 0) == 0;
    }

    std::string kline_interval_from_stream() const
    {
        auto pos = stream_type_.find('_');
        if (pos == std::string::npos) return "";
        return stream_type_.substr(pos + 1);
    }

    static std::string encode_kline_json(const backfill_bar& b,
                                         const std::string& interval)
    {
        char buf[1024];
        std::snprintf(buf, sizeof(buf),
            "{\"e\":\"kline\",\"E\":%lld,\"s\":\"\",\"k\":{"
            "\"t\":%lld,\"T\":%lld,\"s\":\"\",\"i\":\"%s\","
            "\"o\":\"%.8f\",\"c\":\"%.8f\",\"h\":\"%.8f\",\"l\":\"%.8f\","
            "\"v\":\"%.8f\",\"x\":true}}",
            static_cast<long long>(b.open_time),
            static_cast<long long>(b.open_time),
            static_cast<long long>(b.open_time),
            interval.c_str(),
            b.open, b.close, b.high, b.low, b.volume);
        return std::string(buf);
    }
};

#endif // HAS_BINANCE
