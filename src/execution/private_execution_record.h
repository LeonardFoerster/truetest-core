#pragma once

#include "../core/event.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>

// Fixed-size private-account record passed from a provider-owned private
// reader to the engine event loop.  The private reader is the sole producer;
// it must never allocate an engine event or mutate engine-owned state.
//
// This deliberately carries both economic fills and non-economic lifecycle
// truth in one FIFO.  Keeping funding in the same stream prevents a fill and
// a cash settlement received by the same private reader from being observed in
// opposite order by strategy/risk code.
struct private_execution_record
{
    enum class kind : std::uint8_t
    {
        ack,
        partial_fill,
        full_fill,
        canceled,
        rejected,
        expired,
        funding,
        unknown_lifecycle,
        fatal,
        bracket_group_active,
        bracket_group_completed,
    };

    static constexpr std::size_t symbol_capacity = 31;
    static constexpr std::size_t client_id_capacity = 95;
    static constexpr std::size_t exchange_id_capacity = 95;
    static constexpr std::size_t execution_id_capacity = 95;
    static constexpr std::size_t commission_asset_capacity = 15;
    static constexpr std::size_t group_id_capacity = 95;
    static constexpr std::size_t error_capacity = 127;

    std::uint64_t sequence = 0;
    std::uint64_t engine_order_id = 0;
    std::int64_t event_time_ms = 0;

    double last_fill_qty = 0.0;
    double last_fill_price = 0.0;
    double cumulative_qty = 0.0;
    double commission = 0.0;
    double remaining_qty = 0.0;
    double cash_delta = 0.0;

    std::array<char, symbol_capacity + 1> symbol{};
    std::array<char, client_id_capacity + 1> client_order_id{};
    std::array<char, exchange_id_capacity + 1> exchange_order_id{};
    std::array<char, execution_id_capacity + 1> execution_id{};
    std::array<char, commission_asset_capacity + 1> commission_asset{};
    std::array<char, group_id_capacity + 1> group_id{};
    std::array<char, error_capacity + 1> error{};

    std::uint8_t symbol_size = 0;
    std::uint8_t client_order_id_size = 0;
    std::uint8_t exchange_order_id_size = 0;
    std::uint8_t execution_id_size = 0;
    std::uint8_t commission_asset_size = 0;
    std::uint8_t group_id_size = 0;
    std::uint8_t error_size = 0;

    order_side side = order_side::buy;
    kind k = kind::fatal;
    bool cumulative_reported = false;
    bool lifecycle_only = false;

    [[nodiscard]] constexpr bool is_economic_fill() const noexcept
    {
        return k == kind::partial_fill || k == kind::full_fill;
    }

    [[nodiscard]] constexpr bool is_terminal() const noexcept
    {
        return (k == kind::full_fill && !lifecycle_only)
            || k == kind::canceled
            || k == kind::rejected || k == kind::expired;
    }

    [[nodiscard]] std::string_view symbol_view() const noexcept
    {
        return {symbol.data(), symbol_size};
    }
    [[nodiscard]] std::string_view client_order_id_view() const noexcept
    {
        return {client_order_id.data(), client_order_id_size};
    }
    [[nodiscard]] std::string_view exchange_order_id_view() const noexcept
    {
        return {exchange_order_id.data(), exchange_order_id_size};
    }
    [[nodiscard]] std::string_view execution_id_view() const noexcept
    {
        return {execution_id.data(), execution_id_size};
    }
    [[nodiscard]] std::string_view commission_asset_view() const noexcept
    {
        return {commission_asset.data(), commission_asset_size};
    }
    [[nodiscard]] std::string_view group_id_view() const noexcept
    {
        return {group_id.data(), group_id_size};
    }
    [[nodiscard]] std::string_view error_view() const noexcept
    {
        return {error.data(), error_size};
    }

    template <std::size_t N>
    static bool copy_text(std::array<char, N>& destination,
                          std::uint8_t& size,
                          std::string_view text) noexcept
    {
        static_assert(N > 1);
        if (text.empty() || text.size() >= N) return false;
        std::copy(text.begin(), text.end(), destination.begin());
        destination[text.size()] = '\0';
        size = static_cast<std::uint8_t>(text.size());
        return true;
    }

    template <std::size_t N>
    static bool copy_optional_text(std::array<char, N>& destination,
                                   std::uint8_t& size,
                                   std::string_view text) noexcept
    {
        static_assert(N > 1);
        if (text.size() >= N) return false;
        if (!text.empty())
            std::copy(text.begin(), text.end(), destination.begin());
        destination[text.size()] = '\0';
        size = static_cast<std::uint8_t>(text.size());
        return true;
    }

    [[nodiscard]] bool valid_shape() const noexcept
    {
        if (sequence == 0 || event_time_ms <= 0) return false;
        if (symbol_size > symbol_capacity
            || client_order_id_size > client_id_capacity
            || exchange_order_id_size > exchange_id_capacity
            || execution_id_size > execution_id_capacity
            || commission_asset_size > commission_asset_capacity
            || group_id_size > group_id_capacity
            || error_size > error_capacity)
            return false;

        switch (k)
        {
        case kind::ack:
        case kind::partial_fill:
        case kind::full_fill:
        case kind::canceled:
        case kind::rejected:
        case kind::expired:
        case kind::funding:
        case kind::unknown_lifecycle:
        case kind::fatal:
        case kind::bracket_group_active:
        case kind::bracket_group_completed:
            break;
        default:
            return false;
        }

        if (k == kind::fatal || k == kind::unknown_lifecycle)
            return error_size != 0;
        if (k == kind::funding)
        {
            return symbol_size != 0 && std::isfinite(cash_delta)
                && cash_delta != 0.0;
        }

        if (k == kind::bracket_group_active
            || k == kind::bracket_group_completed)
            return symbol_size != 0 && group_id_size != 0;

        if (symbol_size == 0 || !std::isfinite(cumulative_qty)
            || !std::isfinite(commission))
            return false;
        if (client_order_id_size == 0 && exchange_order_id_size == 0)
            return false;

        if (is_economic_fill())
        {
            if (!cumulative_reported) return false;
            if (lifecycle_only)
            {
                return last_fill_qty == 0.0 && last_fill_price == 0.0
                    && commission == 0.0;
            }
            return execution_id_size != 0
                && last_fill_qty > 0.0 && std::isfinite(last_fill_qty)
                && last_fill_price > 0.0 && std::isfinite(last_fill_price)
                && remaining_qty >= 0.0 && std::isfinite(remaining_qty);
        }

        // Non-fill lifecycle reports never carry a new economic increment.
        // A terminal lifecycle is nevertheless an assertion that no unseen
        // economic delta remains.  It must therefore carry the venue's
        // cumulative total explicitly: omission is not evidence that the
        // local total is still correct.
        if ((k == kind::canceled || k == kind::rejected || k == kind::expired)
            && !cumulative_reported)
            return false;
        return last_fill_qty == 0.0 && last_fill_price == 0.0
            && commission == 0.0;
    }
};

static_assert(std::is_trivially_copyable_v<private_execution_record>);
