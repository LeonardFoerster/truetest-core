#pragma once
#ifdef HAS_BINANCE

#include "providers/provider.h"
#include "providers/binance/binance_transport.h"
#include "providers/binance/binance_executor.h"

#include <memory>
#include <string>

// BinanceProvider: live exchange provider for Binance spot market data + execution.
//
// Data feed: WebSocket streaming via BinanceTransport
// Execution: Paper mode by default via BinanceExecutor
//
// Usage:
//   auto provider = std::make_shared<BinanceProvider>("btcusdt", "trade");
//   provider->open();
//   auto transport = provider->get_transport();  // streaming WebSocket
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
    bool has_execution() const override { return !api_key_.empty(); }

    bool open() override
    {
        transport_ = std::make_shared<BinanceTransport>(
            symbol_, stream_type_, host_, port_);

        if (!api_key_.empty())
        {
            // Map stream host to REST host
            std::string rest_host = "api.binance.com";
            std::string rest_port = "443";
            if (host_.find("testnet") != std::string::npos)
            {
                rest_host = "testnet.binance.vision";
                rest_port = "443";
            }

            executor_ = std::make_shared<BinanceExecutor>(
                api_key_, api_secret_, rest_host, rest_port);
            executor_->set_symbol(symbol_);
            // Paper mode by default — user must explicitly enable live trading
        }

        return transport_->open();
    }

    void close() override
    {
        if (transport_) transport_->close();
    }

    std::shared_ptr<IDataTransport> get_transport() override
    {
        return transport_;
    }

    std::shared_ptr<IExecutionAdapter> get_execution_adapter() override
    {
        return executor_;
    }

    const std::string& symbol() const { return symbol_; }
    const std::string& stream_type() const { return stream_type_; }

    // Enable live trading on the executor (requires API keys)
    void set_live_trading(bool enabled)
    {
        if (executor_)
            executor_->set_live_trading(enabled);
    }

private:
    std::string symbol_;
    std::string stream_type_;
    std::string api_key_;
    std::string api_secret_;
    std::string host_;
    std::string port_;

    std::shared_ptr<BinanceTransport> transport_;
    std::shared_ptr<BinanceExecutor> executor_;
};

#endif // HAS_BINANCE
