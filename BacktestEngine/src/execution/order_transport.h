#pragma once

#include <string>
#include <string_view>

class IOrderTransport
{
public:
    virtual ~IOrderTransport() = default;

    virtual bool open() = 0;
    virtual void close() = 0;

    struct result
    {
        bool ok = false;
        std::string exchange_order_id;
        std::string raw_response;
        std::string error;
    };

    virtual result submit(std::string_view endpoint,
                          std::string_view wire_payload) = 0;

    virtual result cancel(std::string_view endpoint,
                          std::string_view wire_payload) = 0;
};
