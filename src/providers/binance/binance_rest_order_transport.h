#pragma once
#ifdef HAS_BINANCE

#include "../../execution/order_transport.h"
#include "binance_parser.h"
#include "binance_rest_client.h"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

class BinanceRestOrderTransport : public IOrderTransport
{
public:
    struct response
    {
        int status = 0;
        std::string body;
    };

    using request_fn = std::function<response(std::string_view endpoint,
                                              std::string_view params)>;

    BinanceRestOrderTransport(request_fn post, request_fn del)
        : post_(std::move(post)), del_(std::move(del))
    {}

    bool open() override { return true; }
    void close() override {}

    result submit(std::string_view endpoint,
                  std::string_view wire_payload) override
    {
        if (!post_)
        {
            result r;
            r.error = "BinanceRestOrderTransport: no post callable";
            return r;
        }
        return map_response(post_(endpoint, wire_payload));
    }

    result cancel(std::string_view endpoint,
                  std::string_view wire_payload) override
    {
        if (!del_)
        {
            result r;
            r.error = "BinanceRestOrderTransport: no del callable";
            return r;
        }
        return map_response(del_(endpoint, wire_payload));
    }

private:
    request_fn post_;
    request_fn del_;

    static result map_response(const response& resp)
    {
        result r;
        r.raw_response = resp.body;
        if (resp.status >= 200 && resp.status < 300)
        {
            r.ok = true;
            auto id = binance::extract_number(resp.body, "orderId");
            if (id.empty())
                id = binance::extract_string(resp.body, "orderId");
            r.exchange_order_id = std::move(id);
        }
        else
        {
            r.ok = false;
            r.error = "HTTP " + std::to_string(resp.status) + ": "
                    + binance::redact_for_log(resp.body, 240);
        }
        return r;
    }
};

inline std::shared_ptr<BinanceRestOrderTransport>
make_binance_rest_order_transport(std::shared_ptr<BinanceRestClient> client)
{
    auto post = [client](std::string_view ep, std::string_view p)
        -> BinanceRestOrderTransport::response
    {
        auto resp = client->post(std::string(ep), std::string(p));
        return {resp.status, resp.body};
    };
    auto del = [client](std::string_view ep, std::string_view p)
        -> BinanceRestOrderTransport::response
    {
        auto resp = client->del(std::string(ep), std::string(p));
        return {resp.status, resp.body};
    };
    return std::make_shared<BinanceRestOrderTransport>(std::move(post), std::move(del));
}

#endif // HAS_BINANCE
