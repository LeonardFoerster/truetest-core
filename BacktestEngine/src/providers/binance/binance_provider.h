#pragma once
#ifdef HAS_BINANCE

#include "core/engine_config.h"
#include "providers/provider.h"
#include "providers/prepend_transport.h"
#include "providers/binance/binance_transport.h"
#include "providers/binance/binance_executor.h"
#include "providers/binance/binance_backfill.h"
#include "providers/binance/hybrid_executor.h"
#include "orderbook/orderbook.h"

#include <cstdio>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

// BinanceProvider: live exchange provider for Binance spot market data + execution.
//
// Data feed: WebSocket streaming via BinanceTransport. When backfill_bars > 0
//   is configured and the stream is a kline stream, historical bars are
//   prepended to the live stream through a PrependTransport decorator so the
//   engine sees a single continuous feed and never needs to know backfill
//   exists.
// Execution:
//   - Live mode: BinanceExecutor with REST order submission.
//   - Paper/backtest/shadow: HybridExecutor (market → paper BinanceExecutor,
//     limit → local synthetic book). The hybrid executor owns book seeding;
//     on_mid_price() forwards mid-price updates into it.
//
// Usage:
//   auto provider = std::make_shared<BinanceProvider>("btcusdt", "kline_1m");
//   provider->configure(engine_cfg);
//   provider->open();
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
    {}

    std::string name() const override { return "binance"; }

    bool has_data_feed() const override { return true; }
    bool has_execution() const override { return true; }

    lifecycle lifecycle_state() const override { return state_; }

    void configure(const engine_config& cfg) override
    {
        mode_ = cfg.mode;
        fee_model_ = cfg.fee_model;
        fill_model_ = cfg.fill_model;
        qty_scale_ = cfg.qty_scale;
        spread_step_factor_ = cfg.spread_step_factor;
        backfill_bars_ = cfg.backfill_bars;
        backfill_interval_ = cfg.backfill_interval;
        backfill_host_override_ = cfg.backfill_host;
        configured_ = true;
    }

    bool open() override
    {
        state_ = lifecycle::opening;

        // Build the live WebSocket transport
        auto live_transport = std::make_shared<BinanceTransport>(
            symbol_, stream_type_, host_, port_);

        // Map WebSocket host to matching REST host
        std::string rest_host = rest_host_for_stream();

        // Backfill: fetch historical klines and prepend them as JSON lines
        // on the stream. Only supported for kline streams — trade streams
        // would need a different prepended record shape.
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

        // Build the execution adapter. Live mode goes straight to
        // BinanceExecutor with live trading enabled. Every other mode
        // uses a HybridExecutor wrapping a paper BinanceExecutor.
        binance_exec_ = std::make_shared<BinanceExecutor>(
            api_key_, api_secret_, rest_host, "443");
        binance_exec_->set_symbol(symbol_);

        if (mode_ == engine_mode::live && !api_key_.empty())
        {
            binance_exec_->set_live_trading(true);
            executor_ = binance_exec_;
        }
        else
        {
            auto book = std::make_shared<orderbook>();
            hybrid_exec_ = std::make_shared<HybridExecutor>(
                binance_exec_, book, fee_model_, fill_model_,
                qty_scale_, spread_step_factor_);
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

    const std::string& symbol() const { return symbol_; }
    const std::string& stream_type() const { return stream_type_; }

    // Enable live trading on the executor (requires API keys).
    // Normally driven by configure() + mode=live, but kept for
    // compatibility with direct callers.
    void set_live_trading(bool enabled)
    {
        if (binance_exec_)
            binance_exec_->set_live_trading(enabled);
    }

private:
    std::string symbol_;
    std::string stream_type_;
    std::string api_key_;
    std::string api_secret_;
    std::string host_;
    std::string port_;

    // Configuration (from configure())
    bool configured_ = false;
    engine_mode mode_ = engine_mode::backtest;
    std::shared_ptr<IFeeModel> fee_model_;
    std::shared_ptr<IFillModel> fill_model_;
    double qty_scale_ = 1e8;
    double spread_step_factor_ = 0.0001;
    int backfill_bars_ = 0;
    std::string backfill_interval_;
    std::string backfill_host_override_;

    // Lifecycle
    lifecycle state_ = lifecycle::closed;

    // Transport (wraps live transport with an optional PrependTransport
    // for backfilled bars).
    std::shared_ptr<IDataTransport> transport_;

    // Execution: either hybrid_exec_ (paper/shadow/backtest) OR
    // binance_exec_ directly (live). executor_ aliases the active one.
    std::shared_ptr<BinanceExecutor> binance_exec_;
    std::shared_ptr<HybridExecutor> hybrid_exec_;
    std::shared_ptr<IExecutionAdapter> executor_;

    std::string rest_host_for_stream() const
    {
        if (!backfill_host_override_.empty())
            return backfill_host_override_;
        if (host_.find("testnet") != std::string::npos)
            return "testnet.binance.vision";
        return "api.binance.com";
    }

    bool is_kline_stream() const
    {
        return stream_type_.rfind("kline", 0) == 0;
    }

    std::string kline_interval_from_stream() const
    {
        // stream_type like "kline_1m" → "1m"
        auto pos = stream_type_.find('_');
        if (pos == std::string::npos) return "";
        return stream_type_.substr(pos + 1);
    }

    // Format a backfilled bar as a Binance kline WebSocket JSON message
    // so the stream parser (BinanceKlineParser) handles it uniformly.
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
