#pragma once

#include "../core/event.h"

#include <chrono>
#include <string>
#include <string_view>
#include <vector>

// Server-side state snapshot pushed via the user-data WebSocket without
// being tied to one of our own orders — futures `ACCOUNT_UPDATE` is the
// canonical example (liquidations, funding settlements, ADL closeouts,
// margin-mode flips). Distinct from `parsed_exec`: those describe an
// order's lifecycle, these describe what the venue currently believes
// about positions and balances.
struct parsed_position_snapshot
{
    enum class reason
    {
        unknown,
        order,            // a fill we likely already know about via parsed_exec
        funding_fee,
        adjustment,
        deposit,
        withdraw,
        margin_transfer,
        margin_type_change,
        liquidation,
        admin,
        other,
    };

    struct position_row
    {
        std::string symbol;
        double      qty = 0.0;       // signed: long > 0, short < 0
        std::string margin_type;     // canonicalized: "ISOLATED" / "CROSSED"
        std::string position_side;   // "BOTH" in one-way, "LONG"/"SHORT" in hedge
    };

    struct balance_row
    {
        std::string asset;
        double      wallet_balance = 0.0;
        double      balance_change = 0.0;
    };

    reason r = reason::unknown;
    std::chrono::system_clock::time_point ts{};
    std::vector<position_row> positions;
    std::vector<balance_row>  balances;
};

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

    // Optional: parse server-pushed position/balance snapshots not tied
    // to a specific order (e.g. futures ACCOUNT_UPDATE). Default returns
    // false — spot's user-data parser doesn't surface snapshots through
    // this channel. Callers should invoke this only after `parse()`
    // declines, since one event can be relevant to one path or the
    // other but not both.
    virtual bool parse_position_snapshot(std::string_view /*raw*/,
                                         parsed_position_snapshot& /*out*/)
    {
        return false;
    }
};
