#pragma once

#include <string>
#include <string_view>

class IOrderTransport
{
public:
    virtual ~IOrderTransport() = default;

    virtual bool open() = 0;
    virtual void close() = 0;
    // Non-blocking admission stop. Implementations must ensure a request that
    // has not started its venue operation cannot start after this returns.
    virtual void quiesce() {}

    struct result
    {
        bool ok = false;
        // The venue may have accepted the mutation, but no authoritative
        // response was received. Callers must keep local tracking and halt;
        // treating this as a rejection permits duplicate exposure.
        bool uncertain = false;
        bool fatal = false;
        std::string exchange_order_id;
        std::string raw_response;
        std::string error;
    };

    virtual result submit(std::string_view endpoint,
                          std::string_view wire_payload) = 0;

    virtual result cancel(std::string_view endpoint,
                          std::string_view wire_payload) = 0;
};
