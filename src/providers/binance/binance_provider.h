#pragma once
#ifdef HAS_BINANCE

#include "engine/engine_config.h"
#include "execution/client_order_id.h"
#include "execution/execution_bridge.h"
#include "execution/rate_limiter.h"
#include "execution/trade_tape_shadow_adapter.h"
#include "providers/provider.h"
#include "providers/prepend_transport.h"
#include "providers/binance/binance_kill_switch.h"
#include "providers/binance/binance_oco_bracket_adapter.h"
#include "providers/binance/binance_reconciler.h"
#include "providers/binance/binance_combined_parser.h"
#include "providers/binance/binance_combined_transport.h"
#include "providers/binance/binance_transport.h"
#include "providers/binance/binance_executor.h"
#include "providers/binance/binance_backfill.h"
#include "providers/binance/binance_endpoints.h"
#include "providers/binance/binance_order_encoder.h"
#include "providers/binance/binance_rest_client.h"
#include "providers/binance/binance_rest_order_transport.h"
#include "providers/binance/binance_time_sync.h"
#include "providers/binance/binance_user_data_parser.h"
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

class BinanceProvider : public IProvider
{
public:
    BinanceProvider(
        const std::string& symbol,
        const std::string& stream_type,
        const std::string& api_key = "",
        const std::string& api_secret = "",
        const std::string& host = "stream.binance.com",
        const std::string& port = "9443")
        : symbol_(symbol)
        , stream_type_(stream_type)
        , api_key_(api_key)
        , api_secret_(api_secret)
        , host_(host)
        , port_(port)
        , endpoints_(binance::from_host(host))
    {
        if (!port.empty()) endpoints_.ws_port = port;
    }

    // Combined WS: primary stream + depth (e.g. "depth20@100ms"). Empty =
    // single-stream, paper-seeded book in shadow.
    void set_depth_stream(const std::string& depth_stream_suffix)
    {
        depth_stream_ = depth_stream_suffix;
    }

    const std::string& depth_stream() const { return depth_stream_; }

    void set_endpoints(binance::endpoints ep)
    {
        endpoints_ = std::move(ep);
        host_ = endpoints_.ws_host;
        port_ = endpoints_.ws_port;
    }

    bool is_testnet() const { return endpoints_.is_testnet; }

    std::string name() const override { return "binance"; }

    bool has_data_feed() const override { return true; }
    bool has_execution() const override { return true; }

    lifecycle lifecycle_state() const override { return state_; }

    void configure(const engine_config& cfg) override
    {
        mode_ = cfg.mode;
        fee_model_ = cfg.fee_model;
        fill_model_ = cfg.fill_model;
        wire_latency_model_ = cfg.wire_latency_model;
        qty_scale_ = cfg.qty_scale;
        spread_step_factor_ = cfg.spread_step_factor;
        backfill_bars_ = cfg.backfill_bars;
        backfill_interval_ = cfg.backfill_interval;
        backfill_host_override_ = cfg.backfill_host;
        seed_ = cfg.seed;
        dashboard_ = cfg.dashboard;
        // main.inc configures after open(); forward so paper orders don't
        // fall through to stdout and scroll the TUI.
        if (binance_exec_)
            binance_exec_->set_dashboard(dashboard_);
        configured_ = true;
    }

    bool open() override
    {
        state_ = lifecycle::opening;

        std::shared_ptr<IDataTransport> live_transport;
        if (depth_stream_.empty())
        {
            live_transport = std::make_shared<BinanceTransport>(
                symbol_, stream_type_, endpoints_.ws_host, endpoints_.ws_port);
        }
        else
        {
            const std::string sym_lower = lower(symbol_);
            std::vector<std::string> streams;
            streams.reserve(2);
            streams.push_back(sym_lower + "@" + stream_type_);
            streams.push_back(sym_lower + "@" + depth_stream_);
            live_transport = std::make_shared<BinanceCombinedTransport>(
                streams, endpoints_.ws_host, endpoints_.ws_port);
        }

        std::string rest_host = rest_host_for_stream();

        std::vector<std::string> prepend;
        if (backfill_bars_ > 0 && is_kline_stream())
        {
            std::string interval = backfill_interval_;
            if (interval.empty())
                interval = kline_interval_from_stream();
            if (interval.empty())
                interval = "1m";

            BinanceBackfill backfiller(rest_host);
            std::cerr << "  BinanceProvider: backfilling " << backfill_bars_
                      << " bars for " << symbol_ << " (" << interval << ")...\n";

            auto bars = backfiller.fetch(symbol_, interval, backfill_bars_);
            std::cerr << "  BinanceProvider: backfill loaded "
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
            rest_ = std::make_shared<BinanceRestClient>(
                api_key_, api_secret_, rest_host, endpoints_.rest_port);

            // Prime offset cache so the first signed req doesn't resync.
            // verify_clock_skew below is the authoritative gate.
            (void)rest_->resync_clock_now();

            auto check = binance::verify_clock_skew(*rest_);
            if (!check.ok)
            {
                std::cerr << "BinanceProvider: refusing to go live — "
                          << check.note << "\n";
                state_ = lifecycle::error;
                return false;
            }

            // Limiter tracks Binance spot's 50/10s order-rate rule —
            // NOT covered by the REST client's weight throttle.
            minter_ = std::make_shared<ClientOrderIdMinter>("tt", seed_);
            order_rate_limiter_ = std::make_shared<TokenBucketRateLimiter>(
                /*capacity=*/50.0, /*refill_per_sec=*/5.0);

            auto assets = split_symbol(symbol_);
            reconciler_ = std::make_shared<BinanceReconciler>(
                rest_, upper(symbol_), assets.base, assets.quote);
            kill_switch_ = std::make_shared<BinanceKillSwitch>(
                rest_, upper(symbol_), assets.base, minter_);
            bracket_adapter_ = make_binance_oco_bracket_adapter(rest_);

            ExecutionBridge::deps d;
            d.order_tx = make_binance_rest_order_transport(rest_);
            d.fill_tx  = std::make_shared<BinanceUserDataTransport>(
                             rest_, endpoints_.ws_host, endpoints_.ws_port);
            d.encoder  = std::make_shared<BinanceOrderEncoder>(symbol_);
            d.parser   = std::make_shared<BinanceUserDataParser>();
            d.order_rate_limiter = order_rate_limiter_;
            d.client_id_fn = [m = minter_](uint64_t) { return m->next(); };

            bridge_ = std::make_shared<ExecutionBridge>(std::move(d));
            if (!bridge_->open())
            {
                std::cerr << "BinanceProvider: ExecutionBridge open failed: "
                          << bridge_->last_error() << "\n";
                state_ = lifecycle::error;
                return false;
            }
            executor_ = bridge_;
        }
        else if (mode_ == engine_mode::shadow)
        {
            shadow_exec_ = std::make_shared<TradeTapeShadowAdapter>(
                wire_latency_model_, fee_model_);
            executor_ = shadow_exec_;
        }
        else
        {
            auto book = std::make_shared<orderbook>();
            hybrid_exec_ = std::make_shared<HybridExecutor>(
                binance_exec_, book, fee_model_, fill_model_,
                qty_scale_, spread_step_factor_, wire_latency_model_);
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

    // Only with depth — otherwise the single-stream parser is cheaper.
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
    std::shared_ptr<BinanceReconciler> reconciler_;
    std::shared_ptr<BinanceKillSwitch> kill_switch_;
    std::shared_ptr<BinanceOcoBracketAdapter> bracket_adapter_;

    std::uint64_t seed_ = 0;
    std::string depth_stream_;
    std::weak_ptr<truetest::ui::ConsoleDashboard> dashboard_;

    struct asset_pair { std::string base; std::string quote; };

    // "BTCUSDT" → {BTC, USDT} via the documented quote suffixes. Unknown
    // → 3-char quote fallback; operator can override via cfg.reconciler.
    static asset_pair split_symbol(const std::string& sym)
    {
        std::string u = upper(sym);
        static constexpr const char* kQuotes[] = {
            "USDT", "USDC", "BUSD", "TUSD", "FDUSD", "DAI",
            "BTC",  "ETH",  "BNB",  "EUR",  "TRY",   "USD"
        };
        for (const char* q : kQuotes)
        {
            std::size_t qlen = std::char_traits<char>::length(q);
            if (u.size() > qlen && u.compare(u.size() - qlen, qlen, q) == 0)
                return { u.substr(0, u.size() - qlen), q };
        }
        if (u.size() > 3)
            return { u.substr(0, u.size() - 3), u.substr(u.size() - 3) };
        return { u, {} };
    }

    static std::string upper(const std::string& s)
    {
        std::string out(s);
        for (auto& c : out)
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return out;
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
