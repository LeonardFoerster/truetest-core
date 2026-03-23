#pragma once
#ifdef HAS_BINANCE

#include "execution/execution_adapter.h"
#include "providers/binance/binance_auth.h"
#include "providers/binance/binance_rest_client.h"
#include "providers/binance/binance_parser.h"

#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// BinanceExecutor: execution adapter for Binance.
//
// Modes:
// - Paper (default): orders logged, fills simulated from last price.
// - Live: orders submitted via REST API, fills received via polling or
//   user data stream. Requires API key + secret.
//
// Paper mode tracks simulated fills using a simple model:
// - Market orders fill immediately at the last known price
// - Limit orders are not supported in paper mode (would need book simulation)
class BinanceExecutor : public IExecutionAdapter
{
public:
    BinanceExecutor(const std::string& api_key = "",
                    const std::string& api_secret = "",
                    const std::string& rest_host = "api.binance.com",
                    const std::string& rest_port = "443")
        : api_key_(api_key)
        , api_secret_(api_secret)
        , rest_host_(rest_host)
        , rest_port_(rest_port)
    {}

    void set_live_trading(bool enabled)
    {
        if (enabled && (api_key_.empty() || api_secret_.empty()))
        {
            std::cerr << "BinanceExecutor: cannot enable live trading without API keys\n";
            return;
        }
        live_trading_enabled_ = enabled;

        if (enabled && !rest_client_)
        {
            rest_client_ = std::make_unique<BinanceRestClient>(
                api_key_, api_secret_, rest_host_, rest_port_);
        }
    }

    bool is_live() const { return live_trading_enabled_; }

    void set_last_price(double price) { last_price_ = price; }

    void set_symbol(const std::string& sym) { symbol_ = sym; }

    void submit_order(const order_event& o) override
    {
        if (live_trading_enabled_)
        {
            submit_live_order(o);
            return;
        }

        // Paper mode: log and simulate fill for market orders
        std::cout << "  [PAPER] " << (o.get_side() == order_side::buy ? "BUY" : "SELL")
                  << " " << o.get_quantity() << " " << o.get_symbol()
                  << " @ " << (last_price_ > 0 ? last_price_ : o.get_price()) << "\n";

        if (o.get_order_type() == order_type::market && last_price_ > 0)
        {
            pending_fills_.emplace_back(
                o.get_earliest_eligible_ts(),
                o.get_symbol(),
                o.get_order_id(),
                o.get_side(),
                o.get_quantity(),
                last_price_,
                0.0  // no commission in paper mode
            );
        }
    }

    bool poll_fills(std::vector<fill_event>& out) override
    {
        if (live_trading_enabled_)
        {
            return poll_live_fills(out);
        }

        // Paper mode: return simulated fills
        if (pending_fills_.empty())
            return false;

        out.insert(out.end(),
                   std::make_move_iterator(pending_fills_.begin()),
                   std::make_move_iterator(pending_fills_.end()));
        pending_fills_.clear();
        return true;
    }

private:
    std::string api_key_;
    std::string api_secret_;
    std::string rest_host_;
    std::string rest_port_;
    std::string symbol_;
    bool live_trading_enabled_ = false;
    double last_price_ = 0.0;
    std::vector<fill_event> pending_fills_;
    std::unique_ptr<BinanceRestClient> rest_client_;

    // Track pending live orders for fill polling
    struct pending_order
    {
        uint64_t engine_order_id;
        std::string exchange_order_id;
        std::string symbol;
        order_side side;
    };
    std::vector<pending_order> pending_live_orders_;

    void submit_live_order(const order_event& o)
    {
        if (!rest_client_)
        {
            std::cerr << "BinanceExecutor: REST client not initialized\n";
            return;
        }

        // Map order type to Binance type string
        std::string type_str;
        switch (o.get_order_type())
        {
        case order_type::market: type_str = "MARKET"; break;
        case order_type::limit:  type_str = "LIMIT"; break;
        case order_type::stop:   type_str = "STOP_LOSS_LIMIT"; break;
        case order_type::stop_limit: type_str = "STOP_LOSS_LIMIT"; break;
        }

        std::string side_str = (o.get_side() == order_side::buy) ? "BUY" : "SELL";

        // Map time-in-force
        std::string tif_str;
        switch (o.get_tif())
        {
        case time_in_force::gtc: tif_str = "GTC"; break;
        case time_in_force::ioc: tif_str = "IOC"; break;
        case time_in_force::fok: tif_str = "FOK"; break;
        case time_in_force::day: tif_str = "GTC"; break; // Binance has no DAY
        }

        // Build query string
        std::string sym = o.get_symbol().empty() ? symbol_ : o.get_symbol();
        // Binance requires uppercase symbol
        std::string upper_sym = sym;
        for (auto& c : upper_sym)
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

        std::string params = "symbol=" + upper_sym
            + "&side=" + side_str
            + "&type=" + type_str
            + "&quantity=" + std::to_string(o.get_quantity());

        // LIMIT and STOP orders need price + timeInForce
        if (o.get_order_type() != order_type::market)
        {
            params += "&price=" + std::to_string(o.get_price());
            params += "&timeInForce=" + tif_str;
        }

        // Stop orders need stopPrice
        if (o.get_order_type() == order_type::stop ||
            o.get_order_type() == order_type::stop_limit)
        {
            params += "&stopPrice=" + std::to_string(o.get_stop_price());
        }

        auto resp = rest_client_->post("/api/v3/order", params);

        if (resp.status == 200)
        {
            // Extract orderId from response
            auto order_id_str = binance::extract_number(resp.body, "orderId");
            std::cout << "  [LIVE] Order submitted: " << side_str << " "
                      << o.get_quantity() << " " << upper_sym
                      << " → exchange order " << order_id_str << "\n";

            pending_live_orders_.push_back({
                o.get_order_id(),
                order_id_str,
                upper_sym,
                o.get_side()
            });
        }
        else
        {
            std::cerr << "  [LIVE] Order REJECTED: HTTP " << resp.status
                      << " " << resp.body << "\n";
        }
    }

    bool poll_live_fills(std::vector<fill_event>& out)
    {
        if (!rest_client_ || pending_live_orders_.empty())
            return false;

        bool got_fills = false;

        // Poll each pending order for status
        auto it = pending_live_orders_.begin();
        while (it != pending_live_orders_.end())
        {
            std::string params = "symbol=" + it->symbol
                + "&orderId=" + it->exchange_order_id;

            auto resp = rest_client_->get("/api/v3/order", params);

            if (resp.status == 200)
            {
                auto status = binance::extract_string(resp.body, "status");

                if (status == "FILLED" || status == "PARTIALLY_FILLED")
                {
                    auto exec_qty_str = binance::extract_string(resp.body, "executedQty");
                    auto avg_price_str = binance::extract_string(resp.body, "avgPrice");

                    // Fallback: use cummulativeQuoteQty / executedQty for avg price
                    if (avg_price_str.empty() || avg_price_str == "0.00000000")
                    {
                        auto quote_str = binance::extract_string(resp.body, "cummulativeQuoteQty");
                        if (!quote_str.empty() && !exec_qty_str.empty())
                        {
                            double quote = std::stod(quote_str);
                            double qty = std::stod(exec_qty_str);
                            if (qty > 0) avg_price_str = std::to_string(quote / qty);
                        }
                    }

                    int filled_qty = exec_qty_str.empty() ? 0
                        : static_cast<int>(std::stod(exec_qty_str));
                    double avg_price = avg_price_str.empty() ? 0.0
                        : std::stod(avg_price_str);

                    if (filled_qty > 0)
                    {
                        out.emplace_back(
                            std::chrono::system_clock::now(),
                            it->symbol,
                            it->engine_order_id,
                            it->side,
                            filled_qty,
                            avg_price,
                            0.0  // commission parsed separately if needed
                        );
                        got_fills = true;
                    }

                    if (status == "FILLED")
                    {
                        it = pending_live_orders_.erase(it);
                        continue;
                    }
                }
                else if (status == "CANCELED" || status == "REJECTED" || status == "EXPIRED")
                {
                    it = pending_live_orders_.erase(it);
                    continue;
                }
            }
            ++it;
        }

        return got_fills;
    }
};

#endif // HAS_BINANCE
