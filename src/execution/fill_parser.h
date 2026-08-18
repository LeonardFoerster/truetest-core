#pragma once

#include "../core/event.h"

#include <chrono>
#include <cstdint>
#include <limits>
#include <ratio>
#include <string>
#include <string_view>
#include <vector>

// Server-side state snapshot pushed via the user-data WebSocket without
// being tied to one of our own orders - futures `ACCOUNT_UPDATE` is the
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
    // Venue-native immutable execution/trade identifier.  Required for
    // economic replay proof whenever the venue supplies it; a duplicate ID
    // with changed economics is a contradiction, never a benign duplicate.
    std::string execution_id;
    std::string symbol;
    order_side  side = order_side::buy;

    double last_fill_qty   = 0.0;
    double last_fill_price = 0.0;
    double cumulative_qty  = 0.0;
    double commission      = 0.0;
    std::string commission_asset;

    std::chrono::system_clock::time_point ts{};

    // A present cumulative value of zero is materially different from an
    // omitted field: the former can contradict an already-booked partial fill.
    bool has_cumulative_qty = false;

    // Some venue order channels mirror a fill/terminal state without carrying
    // an economic delta.  They are typed confirmations, not a second fill.
    bool lifecycle_only = false;

    std::string error;
};

enum class funding_parse_result : std::uint8_t
{
    not_funding,
    valid,
    invalid,
};

// Parsing private order/fill traffic is a safety boundary, not a best-effort
// classifier.  In particular, a malformed message for a known execution
// envelope must never be conflated with an unrelated control or account
// message: the former closes live order admission, the latter may be routed
// to another parser.
enum class execution_parse_result : std::uint8_t
{
    unrelated,
    valid,
    malformed,
};

// `system_clock::duration` is commonly nanoseconds.  Constructing a time
// point directly from an arbitrary positive int64 millisecond count can then
// overflow during the implicit duration conversion.  Validate the exact
// conversion domain before any parser creates the timestamp.
inline bool system_clock_millis_is_representable(
    std::int64_t milliseconds) noexcept
{
    using target_duration = std::chrono::system_clock::duration;
    using target_rep = target_duration::rep;
    using scale = std::ratio_divide<std::chrono::milliseconds::period,
                                    target_duration::period>;
    const long double target_ticks =
        static_cast<long double>(milliseconds)
        * static_cast<long double>(scale::num)
        / static_cast<long double>(scale::den);
    return target_ticks
        >= static_cast<long double>(std::numeric_limits<target_rep>::lowest())
        && target_ticks
        <= static_cast<long double>(std::numeric_limits<target_rep>::max());
}

// Allocation-free handoff from the venue parser to the provider-owned SPSC
// ingress. Venue parsers must return invalid for funding-like frames whose
// envelope, timestamp, asset, or delta is not authoritative.
struct parsed_funding_update
{
    std::int64_t event_time_ms = 0;
    double cash_delta = 0.0;
};

class IFillParser
{
public:
    virtual ~IFillParser() = default;

    [[nodiscard]] virtual execution_parse_result
    parse(std::string_view raw, parsed_exec& out) = 0;

    virtual funding_parse_result parse_funding_update(
        std::string_view /*raw*/, parsed_funding_update& /*out*/) noexcept
    {
        return funding_parse_result::not_funding;
    }

    // `unrelated` is only safe to ignore on the ordered private-account path
    // when the parser can prove that the entire raw frame is an exact,
    // documented transport/control envelope.  The conservative default is
    // false: account snapshots, unknown authenticated events, and a future
    // parser's accidental catch-all must close admission rather than vanish
    // between the private reader and engine FIFO.
    virtual bool is_harmless_private_control(
        std::string_view /*raw*/) const noexcept
    {
        return false;
    }

    // Optional: parse server-pushed position/balance snapshots not tied
    // to a specific order (e.g. futures ACCOUNT_UPDATE). Default returns
    // false - spot's user-data parser doesn't surface snapshots through
    // this channel. Callers invoke this only after `parse()` returns
    // execution_parse_result::unrelated; malformed known execution
    // envelopes are terminal and must not be reinterpreted as snapshots.
    virtual bool parse_position_snapshot(std::string_view /*raw*/,
                                         parsed_position_snapshot& /*out*/)
    {
        return false;
    }
};
