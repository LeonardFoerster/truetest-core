#pragma once
#ifdef HAS_BYBIT

#include "execution/order_transport.h"
#include "providers/bybit/bybit_auth.h"
#include "providers/bybit/bybit_parser.h"
#include "providers/bybit/bybit_rest_client.h"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

// Bybit V5 linear order transport: both place and cancel are POST + JSON body.
// Critical quirk vs Binance: cancel is POST, not DELETE.
//
// Response ok iff HTTP 2xx AND retCode == 0.
// exchange_order_id from result.orderId (string or number).
class BybitRestOrderTransport : public IOrderTransport
{
public:
    struct response
    {
        int status = 0;
        std::string body;
    };

    // Injected post callable (tests inject a fake; factory binds RestClient).
    using post_fn = std::function<response(std::string_view endpoint,
                                           std::string_view body)>;

    explicit BybitRestOrderTransport(post_fn post)
        : post_(std::move(post))
    {}

    bool open() override { return true; }
    void close() override {}

    result submit(std::string_view endpoint,
                  std::string_view wire_payload) override
    {
        if (!post_)
        {
            result r;
            r.error = "BybitRestOrderTransport: no post callable";
            return r;
        }
        return map_response(post_(endpoint, wire_payload));
    }

    // Cancel is POST on Bybit (same post_fn as submit) — never DELETE.
    result cancel(std::string_view endpoint,
                  std::string_view wire_payload) override
    {
        if (!post_)
        {
            result r;
            r.error = "BybitRestOrderTransport: no post callable";
            return r;
        }
        return map_response(post_(endpoint, wire_payload));
    }

private:
    post_fn post_;

    static result map_response(const response& resp)
    {
        result r;
        r.raw_response = resp.body;

        if (!bybit::is_business_success(resp.status, resp.body))
        {
            r.ok = false;
            auto code = bybit::extract_ret_code(resp.body);
            if (resp.status < 200 || resp.status >= 300)
            {
                r.error = "HTTP " + std::to_string(resp.status) + " retCode=";
                r.error.append(code.empty() ? "<missing>" : code);
                r.error.append(": ");
                r.error.append(bybit::redact_for_log(
                    bybit::truncate_for_log(resp.body, 240)));
            }
            else
            {
                r.error = "retCode ";
                r.error.append(code.empty() ? "<missing>" : code);
                r.error.append(": ");
                r.error.append(bybit::redact_for_log(
                    bybit::truncate_for_log(resp.body, 240)));
            }
            return r;
        }

        r.ok = true;
        // Prefer result.orderId; fall back to first orderId needle.
        auto result_obj = bybit::detail::extract_object(resp.body, "result");
        auto id = bybit::extract_sv_string(result_obj, "orderId");
        if (id.empty())
            id = bybit::extract_sv_number(result_obj, "orderId");
        if (id.empty())
            id = bybit::extract_sv_string(resp.body, "orderId");
        if (id.empty())
            id = bybit::extract_sv_number(resp.body, "orderId");
        r.exchange_order_id.assign(id.data(), id.size());
        return r;
    }
};

inline std::shared_ptr<BybitRestOrderTransport>
make_bybit_rest_order_transport(std::shared_ptr<BybitRestClient> client)
{
    auto post = [client](std::string_view ep, std::string_view body)
        -> BybitRestOrderTransport::response
    {
        auto resp = client->post_json(std::string(ep), std::string(body));
        return {resp.status, std::move(resp.body)};
    };
    return std::make_shared<BybitRestOrderTransport>(std::move(post));
}

#endif // HAS_BYBIT
