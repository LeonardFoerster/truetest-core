#pragma once
#ifdef HAS_BINANCE

#include "../../execution/order_transport.h"
#include "binance_parser.h"
#include "binance_rest_client.h"
#include "providers/recovery_payload.h"

#include <functional>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
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
        bool request_written = false;
        bool fatal = false;
    };

    using request_fn = std::function<response(std::string_view endpoint,
                                              std::string_view params)>;

    BinanceRestOrderTransport(
        request_fn post, request_fn del,
        std::shared_ptr<std::atomic<bool>> cancelled =
            std::make_shared<std::atomic<bool>>(false))
        : post_(std::move(post)), del_(std::move(del)),
          cancelled_(std::move(cancelled))
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
            r.error = "BinanceRestOrderTransport: no post callable";
            return r;
        }
        return map_response(post_(endpoint, wire_payload), wire_payload,
                            operation::submit);
    }

    result cancel(std::string_view endpoint,
                  std::string_view wire_payload) override
    {
        std::lock_guard<std::mutex> lock(operation_mu_);
        if (cancelled_->load(std::memory_order_acquire) || !del_)
        {
            result r;
            r.error = "BinanceRestOrderTransport: no del callable";
            return r;
        }
        return map_response(del_(endpoint, wire_payload), wire_payload,
                            operation::cancel);
    }

private:
    request_fn post_;
    request_fn del_;
    std::shared_ptr<std::atomic<bool>> cancelled_;
    std::mutex operation_mu_;

    enum class operation { submit, cancel };

    static std::string_view query_value(std::string_view query,
                                        std::string_view key)
    {
        std::size_t pos = 0;
        while (pos <= query.size())
        {
            const auto end = query.find('&', pos);
            const auto part = query.substr(
                pos, end == std::string_view::npos ? query.size() - pos
                                                   : end - pos);
            const auto eq = part.find('=');
            if (eq != std::string_view::npos && part.substr(0, eq) == key)
                return part.substr(eq + 1);
            if (end == std::string_view::npos) break;
            pos = end + 1;
        }
        return {};
    }

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
        if (resp.status >= 200 && resp.status < 300)
        {
            if (!provider_recovery::is_authoritative_object(resp.body))
                return ambiguous_success(resp, "malformed response envelope");

            std::uint64_t parsed_id = 0;
            if (!provider_recovery::top_level_positive_u64(
                    resp.body, "orderId", parsed_id))
                return ambiguous_success(resp, "missing or invalid orderId");
            const std::string id = std::to_string(parsed_id);

            if (op == operation::submit)
            {
                const auto expected_client =
                    query_value(wire_payload, "newClientOrderId");
                std::string_view returned_client;
                if (!expected_client.empty()
                    && (!provider_recovery::top_level_plain_string(
                            resp.body, "clientOrderId", returned_client)
                        || returned_client != expected_client))
                    return ambiguous_success(
                        resp, "clientOrderId does not match submitted order");
            }
            else
            {
                const auto expected_id = query_value(wire_payload, "orderId");
                std::string_view status;
                if ((!expected_id.empty() && id != expected_id)
                    || !provider_recovery::top_level_plain_string(
                        resp.body, "status", status)
                    || status != "CANCELED")
                    return ambiguous_success(
                        resp, "cancel response does not prove target cancellation");
            }

            r.ok = true;
            r.exchange_order_id = id;
        }
        else
        {
            r.ok = false;
            r.error = (r.uncertain ? "ambiguous post-write outcome; HTTP " : "HTTP ")
                    + std::to_string(resp.status) + ": "
                    + binance::redact_for_log(resp.body, 240);
        }
        return r;
    }
};

inline std::shared_ptr<BinanceRestOrderTransport>
make_binance_rest_order_transport(
    std::shared_ptr<BinanceRestClient> client,
    std::shared_ptr<std::atomic<bool>> cancelled =
        std::make_shared<std::atomic<bool>>(false))
{
    // Orders share the bounded, single-attempt safety lane. That lane owns a
    // dedicated keep-alive connection, so shutdown serialization does not add
    // DNS/TCP/TLS setup to every order.
    constexpr auto request_deadline = std::chrono::milliseconds{1500};
    auto post = [client, cancelled](std::string_view ep, std::string_view p)
        -> BinanceRestOrderTransport::response
    {
        if (!client->ensure_clock_fresh_for_order(
                std::chrono::milliseconds{500}))
            return {0, "clock refresh failed before order mutation", false, true};
        auto resp = client->safety_post(
            std::string(ep), std::string(p), request_deadline, cancelled.get());
        return {resp.status, resp.body, resp.request_written, false};
    };
    auto del = [client, cancelled](std::string_view ep, std::string_view p)
        -> BinanceRestOrderTransport::response
    {
        if (!client->ensure_clock_fresh_for_order(
                std::chrono::milliseconds{500}))
            return {0, "clock refresh failed before order mutation", false, true};
        auto resp = client->safety_del(
            std::string(ep), std::string(p), request_deadline, cancelled.get());
        return {resp.status, resp.body, resp.request_written, false};
    };
    return std::make_shared<BinanceRestOrderTransport>(
        std::move(post), std::move(del), std::move(cancelled));
}

#endif // HAS_BINANCE
