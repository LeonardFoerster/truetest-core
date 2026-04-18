#pragma once

#include "../core/event.h"

#include <string>
#include <string_view>

struct encoded_order
{
    std::string endpoint;
    std::string wire_payload;
    std::string client_order_id;
};

class IOrderEncoder
{
public:
    virtual ~IOrderEncoder() = default;

    virtual encoded_order encode_submit(const order_event& o,
                                        std::string_view client_order_id) = 0;

    virtual encoded_order encode_cancel(std::string_view symbol,
                                        std::string_view exchange_order_id,
                                        std::string_view client_order_id) = 0;
};
