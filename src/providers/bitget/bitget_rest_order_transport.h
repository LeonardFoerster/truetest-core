#pragma once
#ifdef HAS_BITGET

#include "execution/order_transport.h"
#include "providers/bitget/bitget_parser.h"
#include "providers/bitget/bitget_rest_client.h"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

// Bitget UTA order transport: both place and cancel are POST + JSON body.
// Critical quirk vs Binance: cancel is POST, not DELETE.
//
// Response ok iff HTTP 2xx AND business code == "00000".
// exchange_order_id from data.orderId (string or number).
class BitgetRestOrderTransport : public IOrderTransport
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

    explicit BitgetRestOrderTransport(post_fn post)
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
            r.error = "BitgetRestOrderTransport: no post callable";
            return r;
        }
        return map_response(post_(endpoint, wire_payload));
    }

    // Cancel is POST on Bitget (same post_fn as submit) — never DELETE.
    result cancel(std::string_view endpoint,
                  std::string_view wire_payload) override
    {
        if (!post_)
        {
            result r;
            r.error = "BitgetRestOrderTransport: no post callable";
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

        if (!bitget::is_business_success(resp.status, resp.body))
        {
            r.ok = false;
            auto code = bitget::extract_business_code(resp.body);
            if (resp.status < 200 || resp.status >= 300)
            {
                r.error = "HTTP " + std::to_string(resp.status) + ": "
                        + bitget::truncate_for_log(resp.body, 240);
            }
            else
            {
                r.error = "business code ";
                r.error.append(code.empty() ? "<missing>" : code);
                r.error.append(": ");
                r.error.append(bitget::truncate_for_log(resp.body, 240));
            }
            return r;
        }

        r.ok = true;
        // data.orderId (string or number); first needle match is fine —
        // success envelopes only carry orderId under data.
        auto id = bitget::extract_sv_string(resp.body, "orderId");
        if (id.empty())
            id = bitget::extract_sv_number(resp.body, "orderId");
        r.exchange_order_id.assign(id.data(), id.size());
        return r;
    }
};

inline std::shared_ptr<BitgetRestOrderTransport>
make_bitget_rest_order_transport(std::shared_ptr<BitgetRestClient> client)
{
    auto post = [client](std::string_view ep, std::string_view body)
        -> BitgetRestOrderTransport::response
    {
        auto resp = client->post_json(std::string(ep), std::string(body));
        return {resp.status, std::move(resp.body)};
    };
    return std::make_shared<BitgetRestOrderTransport>(std::move(post));
}

#endif // HAS_BITGET
