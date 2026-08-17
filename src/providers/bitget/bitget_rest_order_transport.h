#pragma once
#ifdef HAS_BITGET

#include "execution/order_transport.h"
#include "providers/bitget/bitget_parser.h"
#include "providers/bitget/bitget_rest_client.h"
#include "providers/recovery_payload.h"

#include <functional>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
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
        bool request_written = false;
        bool fatal = false;
    };

    // Injected post callable (tests inject a fake; factory binds RestClient).
    using post_fn = std::function<response(std::string_view endpoint,
                                           std::string_view body)>;

    explicit BitgetRestOrderTransport(
        post_fn post,
        std::shared_ptr<std::atomic<bool>> cancelled =
            std::make_shared<std::atomic<bool>>(false))
        : post_(std::move(post)), cancelled_(std::move(cancelled))
    {}

    bool open() override { return true; }
    void close() override {}
    void quiesce() override
    {
        cancelled_->store(true, std::memory_order_release);
        std::lock_guard<std::mutex> lock(operation_mu_);
    }

    result submit(std::string_view endpoint,
                  std::string_view wire_payload) override
    {
        std::lock_guard<std::mutex> lock(operation_mu_);
        if (cancelled_->load(std::memory_order_acquire) || !post_)
        {
            result r;
            r.error = "BitgetRestOrderTransport: no post callable";
            return r;
        }
        return map_response(post_(endpoint, wire_payload), wire_payload,
                            operation::submit);
    }

    // Cancel is POST on Bitget (same post_fn as submit) — never DELETE.
    result cancel(std::string_view endpoint,
                  std::string_view wire_payload) override
    {
        std::lock_guard<std::mutex> lock(operation_mu_);
        if (cancelled_->load(std::memory_order_acquire) || !post_)
        {
            result r;
            r.error = "BitgetRestOrderTransport: no post callable";
            return r;
        }
        return map_response(post_(endpoint, wire_payload), wire_payload,
                            operation::cancel);
    }

private:
    post_fn post_;
    std::shared_ptr<std::atomic<bool>> cancelled_;
    std::mutex operation_mu_;

    enum class operation { submit, cancel };

    static result ambiguous_success(const response& resp,
                                    std::string_view reason)
    {
        result r;
        r.raw_response = resp.body;
        r.ok = false;
        r.uncertain = true;
        r.fatal = true;
        r.error = "ambiguous HTTP 2xx order mutation: ";
        r.error.append(reason);
        return r;
    }

    static result map_response(const response& resp,
                               std::string_view wire_payload,
                               operation op)
    {
        result r;
        r.raw_response = resp.body;
        r.uncertain = resp.request_written
            && (resp.status == 0 || resp.status >= 500);
        r.fatal = resp.fatal || r.uncertain;

        if (resp.status < 200 || resp.status >= 300)
        {
            r.ok = false;
            r.error = (r.uncertain ? "ambiguous post-write outcome; HTTP " : "HTTP ")
                    + std::to_string(resp.status) + ": "
                    + bitget::truncate_for_log(resp.body, 240);
            return r;
        }

        if (!provider_recovery::is_authoritative_object(resp.body))
            return ambiguous_success(resp, "malformed response envelope");

        const auto code = bitget::extract_business_code(resp.body);
        if (code.empty())
            return ambiguous_success(resp, "missing or invalid business code");
        if (code != "00000")
        {
            r.ok = false;
            r.error = "business code ";
            r.error.append(code);
            r.error.append(": ");
            r.error.append(bitget::truncate_for_log(resp.body, 240));
            return r;
        }

        std::string_view data;
        if (!provider_recovery::top_level_member(resp.body, "data", data)
            || !provider_recovery::is_authoritative_object(data))
            return ambiguous_success(resp, "missing or malformed data object");

        std::uint64_t parsed_id = 0;
        if (!provider_recovery::top_level_positive_u64(
                data, "orderId", parsed_id))
            return ambiguous_success(resp, "missing or invalid orderId");
        const std::string id = std::to_string(parsed_id);

        const auto expected_id = bitget::extract_sv_string(
            wire_payload, "orderId");
        const auto expected_client = bitget::extract_sv_string(
            wire_payload, "clientOid");
        std::string_view returned_client;
        const bool has_returned_client =
            provider_recovery::top_level_plain_string(
                data, "clientOid", returned_client);
        if (op == operation::submit)
        {
            if (!expected_client.empty()
                && (!has_returned_client || returned_client != expected_client))
                return ambiguous_success(
                    resp, "clientOid does not match submitted order");
        }
        else if ((!expected_id.empty() && id != expected_id)
                 || (!expected_client.empty() && expected_id.empty()
                     && (!has_returned_client
                         || returned_client != expected_client)))
        {
            return ambiguous_success(
                resp, "cancel response does not prove target cancellation");
        }

        r.ok = true;
        r.exchange_order_id = id;
        return r;
    }
};

inline std::shared_ptr<BitgetRestOrderTransport>
make_bitget_rest_order_transport(
    std::shared_ptr<BitgetRestClient> client,
    std::shared_ptr<std::atomic<bool>> cancelled =
        std::make_shared<std::atomic<bool>>(false))
{
    // Orders share the bounded, single-attempt safety lane. That lane owns a
    // dedicated keep-alive connection, so shutdown serialization does not add
    // DNS/TCP/TLS setup to every order.
    constexpr auto request_deadline = std::chrono::milliseconds{1500};
    auto post = [client, cancelled](std::string_view ep, std::string_view body)
        -> BitgetRestOrderTransport::response
    {
        if (!client->ensure_clock_fresh_for_order(
                std::chrono::milliseconds{500}))
            return {0, "clock refresh failed before order mutation", false, true};
        auto resp = client->safety_post_json(
            std::string(ep), std::string(body), request_deadline,
            cancelled.get());
        return {resp.status, std::move(resp.body), resp.request_written, false};
    };
    return std::make_shared<BitgetRestOrderTransport>(
        std::move(post), std::move(cancelled));
}

#endif // HAS_BITGET
