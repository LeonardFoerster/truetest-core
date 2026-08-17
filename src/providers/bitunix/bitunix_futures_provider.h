#pragma once
#ifdef HAS_BITUNIX

// Bitunix USDT-M futures — Phase 0–1: market data + paper/shadow execution.
// Live order routing is intentionally refused until Phase 2–4.

#include "providers/provider.h"
#include "providers/bitunix/bitunix_endpoints.h"
#include "providers/bitunix/bitunix_parser.h"
#include "providers/bitunix/bitunix_transport.h"
#include "execution/execution_adapter.h"
#include "execution/trade_tape_shadow_adapter.h"
#include "engine/engine_config.h"
#include "orderbook/orderbook.h"

#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <utility>

class BitunixFuturesProvider : public IProvider
{
public:
    BitunixFuturesProvider(
        const std::string& symbol,
        const std::string& stream_type,
        const std::string& api_key = "",
        const std::string& api_secret = "",
        const std::string& host = "",
        const std::string& port = "")
        : symbol_(bitunix::normalize_symbol(symbol))
        , stream_type_(stream_type.empty() ? "trade" : stream_type)
        , api_key_(api_key)
        , api_secret_(api_secret)
        , endpoints_(bitunix::mainnet())
    {
        if (!host.empty())
            endpoints_.ws_public_host = host;
        if (!port.empty())
            endpoints_.ws_port = port;
    }

    void set_endpoints(bitunix::endpoints ep)
    {
        endpoints_ = std::move(ep);
    }

    std::string name() const override { return "bitunix-futures"; }

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
        qty_scale_ = cfg.qty_scale;
        seed_ = cfg.seed;
        configured_ = true;
    }

    bool open() override
    {
        state_ = lifecycle::opening;

        // Phase 0–1: refuse live. MD/shadow/backtest only.
        if (mode_ == engine_mode::live)
        {
            std::cerr << "BitunixFuturesProvider: refusing live open — "
                         "Phase 0–1 is market-data + paper/shadow only. "
                         "Live orders (encoder, private WS, DMS) are deferred "
                         "to Phase 2–4. Use engine_shadow or backtest mode.\n";
            state_ = lifecycle::error;
            return false;
        }

        // The public transport can subscribe to ticker/depth names, but this
        // provider currently emits only trade events. Refuse unsupported
        // streams before a socket is opened rather than silently discarding
        // every received frame in the trade-only parser.
        if (!is_trade_stream())
        {
            std::cerr << "BitunixFuturesProvider: refusing unsupported public "
                         "stream '" << stream_type_ << "' — only trade is "
                         "implemented by the event parser\n";
            state_ = lifecycle::error;
            return false;
        }

        std::cerr << "  BitunixFuturesProvider: ws="
                  << endpoints_.ws_public_host << ":" << endpoints_.ws_port
                  << endpoints_.ws_public_path
                  << " stream=" << stream_type_
                  << " symbol=" << symbol_ << "\n";

        transport_ = std::make_shared<BitunixTransport>(
            symbol_, stream_type_, endpoints_);
        if (halt_cb_)
            transport_->set_fatal_disconnect_callback(halt_cb_);

        event_parser_ = std::make_shared<BitunixCombinedParser>();

        if (mode_ == engine_mode::shadow)
        {
            shadow_exec_ = std::make_shared<TradeTapeShadowAdapter>(
                wire_latency_model_, fee_model_);
            if (queue_model_)
                shadow_exec_->set_queue_model(queue_model_);
            executor_ = shadow_exec_;
        }
        else
        {
            // Paper / backtest: shared book + LocalBookAdapter.
            auto book = std::make_shared<orderbook>();
            paper_exec_ = std::make_shared<LocalBookAdapter>(
                book, fee_model_, fill_model_,
                static_cast<unsigned>(seed_ ? seed_ : 42u),
                /*market_aggression=*/1.1,
                qty_scale_,
                wire_latency_model_);
            executor_ = paper_exec_;
        }

        if (!transport_->open())
        {
            std::cerr << "BitunixFuturesProvider: transport open failed\n";
            state_ = lifecycle::error;
            return false;
        }

        state_ = lifecycle::open;
        return true;
    }

    void close() override
    {
        if (transport_)
            transport_->request_stop();
        transport_.reset();
        executor_.reset();
        paper_exec_.reset();
        shadow_exec_.reset();
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

    bool supports_event_stream() const override { return is_trade_stream(); }

    std::shared_ptr<IDataParser<provider::event>> get_event_parser() override
    {
        return event_parser_;
    }

    std::optional<MarketDataFeed> get_market_data_feed() override
    {
        auto feed = IProvider::get_market_data_feed();
        if (!feed)
            return std::nullopt;

        // `open()` has already rejected every other channel before creating
        // the public transport. This declares only what the current parser
        // can actually emit; it is not a claim about unimplemented venue
        // channels such as depth or kline.
        feed->request = market_data_request{
            .symbol = symbol_,
            .channels = {{market_data_channel_kind::trades}},
        };
        feed->capabilities = market_data_capabilities{
            .trades = true,
            .candles = false,
            .l2_snapshots = false,
            .l2_deltas = false,
            .max_l2_depth = 0,
            .event_order_is_receive_order = true,
        };
        return feed;
    }

    void set_halt_callback(
        std::function<void(std::string_view reason)> cb) override
    {
        halt_cb_ = std::move(cb);
        if (transport_)
            transport_->set_fatal_disconnect_callback(halt_cb_);
    }

    void on_mid_price(const std::string& /*symbol*/, double mid) override
    {
        if (paper_exec_)
            paper_exec_->set_mid_price(mid);
        if (shadow_exec_)
            shadow_exec_->set_mid_price(mid);
    }

private:
    bool is_trade_stream() const
    {
        return bitunix::map_stream_to_channel(stream_type_) == "trade";
    }

    std::string symbol_;
    std::string stream_type_;
    std::string api_key_;
    std::string api_secret_;
    bitunix::endpoints endpoints_;

    engine_mode mode_ = engine_mode::backtest;
    bool configured_ = false;
    lifecycle state_ = lifecycle::closed;

    std::shared_ptr<IFeeModel> fee_model_;
    std::shared_ptr<IFillModel> fill_model_;
    std::shared_ptr<ILatencyModel> wire_latency_model_;
    std::shared_ptr<IQueuePositionModel> queue_model_;
    double qty_scale_ = 1e8;
    std::uint64_t seed_ = 0;

    std::shared_ptr<BitunixTransport> transport_;
    std::shared_ptr<BitunixCombinedParser> event_parser_;
    std::shared_ptr<IExecutionAdapter> executor_;
    std::shared_ptr<LocalBookAdapter> paper_exec_;
    std::shared_ptr<TradeTapeShadowAdapter> shadow_exec_;
    std::function<void(std::string_view)> halt_cb_;
};

#endif // HAS_BITUNIX
