#pragma once
#ifdef HAS_BINANCE

#include "engine/engine_config.h"
#include "providers/provider.h"
#include "providers/prepend_transport.h"
#include "providers/binance/binance_combined_parser.h"
#include "providers/binance/binance_combined_transport.h"
#include "providers/binance/binance_transport.h"
#include "providers/binance/binance_executor.h"
#include "providers/binance/binance_backfill.h"
#include "providers/binance/binance_endpoints.h"
#include "providers/binance/hybrid_executor.h"
#include "execution/trade_tape_shadow_adapter.h"
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

        // Step 3 wires the futures REST client, reconciler and kill switch.
        // Until then a live attempt against this provider would silently
        // fall through to the paper path, which is exactly the kind of
        // confusion the live binary's compile-time gate exists to prevent.
        if (mode_ == engine_mode::live)
        {
            std::cerr << "BinanceFuturesProvider: live execution not "
                         "implemented yet (futures step 3); refusing to "
                         "open. Use --mode shadow or backtest meanwhile.\n";
            state_ = lifecycle::error;
            return false;
        }

        std::cerr << "  BinanceFuturesProvider: "
                  << (endpoints_.is_testnet ? "[TESTNET] " : "")
                  << "ws=" << endpoints_.ws_host << ":" << endpoints_.ws_port
                  << " rest=" << endpoints_.rest_host << ":" << endpoints_.rest_port
                  << "\n";

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

        if (mode_ == engine_mode::shadow)
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
    std::shared_ptr<IExecutionAdapter> executor_;

    std::uint64_t seed_ = 0;
    std::string depth_stream_;
    std::weak_ptr<truetest::ui::ConsoleDashboard> dashboard_;

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
