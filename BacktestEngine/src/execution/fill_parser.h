#pragma once

#include "../core/event.h"

#include <chrono>
#include <string>
#include <string_view>

struct parsed_exec
{
    enum class kind
    {
        ack,
        partial_fill,
        full_fill,
        canceled,
        rejected,
        expired,
        other
    };

    kind k = kind::other;

    std::string client_order_id;
    std::string exchange_order_id;
    std::string symbol;
    order_side  side = order_side::buy;

    double last_fill_qty   = 0.0;
    double last_fill_price = 0.0;
    double cumulative_qty  = 0.0;
    double commission      = 0.0;
    std::string commission_asset;

    std::chrono::system_clock::time_point ts{};

    std::string error;
};

class IFillParser
{
public:
    virtual ~IFillParser() = default;

    virtual bool parse(std::string_view raw, parsed_exec& out) = 0;
};
