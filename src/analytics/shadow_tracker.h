#pragma once

#include "../core/event.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

enum class shadow_ingest_error : std::uint8_t
{
    none,
    invalid_order_id,
    invalid_symbol,
    invalid_side,
    invalid_quantity,
    invalid_price,
    invalid_timestamp,
    identity_mismatch,
    quantity_overflow,
    arithmetic_overflow,
    slice_count_overflow
};

struct ShadowIngestResult
{
    shadow_ingest_error error = shadow_ingest_error::none;

    explicit operator bool() const noexcept { return error == shadow_ingest_error::none; }
};

struct shadow_fill  // NOLINT(readability-identifier-naming): retained public API
{
    std::uint64_t order_id = 0;
    std::string symbol;
    order_side side = order_side::buy;

    double sim_price = 0.0;
    double sim_quantity = 0.0;
    std::chrono::system_clock::time_point sim_first_timestamp{};
    std::chrono::system_clock::time_point sim_timestamp{};
    std::uint64_t sim_slice_count = 0;
    bool sim_filled = false;

    double exchange_price = 0.0;
    double exchange_quantity = 0.0;
    std::chrono::system_clock::time_point exchange_first_timestamp{};
    std::chrono::system_clock::time_point exchange_timestamp{};
    std::uint64_t exchange_slice_count = 0;
    bool exchange_filled = false;

    [[nodiscard]] static bool quantities_equivalent(double lhs, double rhs) noexcept
    {
        if (!std::isfinite(lhs) || !std::isfinite(rhs) || lhs <= 0.0 || rhs <= 0.0) return false;
        const double scale = std::max(lhs, rhs);
        const double tolerance = 8.0 * std::numeric_limits<double>::epsilon() * scale;
        return std::abs(lhs - rhs) <= tolerance;
    }

    [[nodiscard]] bool quantities_match() const noexcept
    {
        if (!sim_filled || !exchange_filled) return false;
        return quantities_equivalent(sim_quantity, exchange_quantity);
    }

    [[nodiscard]] double quantity_divergence() const noexcept
    {
        return exchange_quantity - sim_quantity;
    }

    [[nodiscard]] std::optional<double> comparable_slippage() const noexcept
    {
        if (!quantities_match()) return std::nullopt;
        return exchange_price - sim_price;
    }

    // Compatibility accessor. A zero result is also used for an unavailable
    // comparison; new reporting code must use comparable_slippage().
    [[nodiscard]] double slippage() const noexcept { return comparable_slippage().value_or(0.0); }

    [[nodiscard]] std::optional<double> comparable_adverse_slippage() const noexcept
    {
        const auto raw = comparable_slippage();
        if (!raw) return std::nullopt;
        return side == order_side::buy ? *raw : -*raw;
    }

    [[nodiscard]] double slippage_bps() const noexcept
    {
        const auto raw = comparable_slippage();
        if (!raw || sim_price <= 0.0) return 0.0;
        return (*raw / sim_price) * 10000.0;
    }

    [[nodiscard]] std::optional<double> comparable_adverse_slippage_bps() const noexcept
    {
        const auto adverse = comparable_adverse_slippage();
        if (!adverse || sim_price <= 0.0) return std::nullopt;
        const double result = (*adverse / sim_price) * 10000.0;
        if (!std::isfinite(result)) return std::nullopt;
        return result;
    }

    [[nodiscard]] std::optional<std::int64_t> first_fill_event_time_delta_ms() const noexcept
    {
        return event_time_delta_ms(exchange_first_timestamp, sim_first_timestamp);
    }

    [[nodiscard]] std::optional<std::int64_t> last_fill_event_time_delta_ms() const noexcept
    {
        return event_time_delta_ms(exchange_timestamp, sim_timestamp);
    }

    // Compatibility accessor: this is a last-fill event-time divergence, not
    // network or venue latency. New code should use the explicitly named API.
    [[nodiscard]] std::int64_t latency_ms() const noexcept
    {
        return last_fill_event_time_delta_ms().value_or(0);
    }

private:
    [[nodiscard]] std::optional<std::int64_t>
    event_time_delta_ms(std::chrono::system_clock::time_point exchange,
                        std::chrono::system_clock::time_point simulated) const noexcept
    {
        if (!sim_filled || !exchange_filled) return std::nullopt;

        using floating_milliseconds = std::chrono::duration<long double, std::milli>;
        const long double exchange_ms = floating_milliseconds(exchange.time_since_epoch()).count();
        const long double simulated_ms =
            floating_milliseconds(simulated.time_since_epoch()).count();
        const long double delta = exchange_ms - simulated_ms;
        constexpr auto minimum = static_cast<long double>(std::numeric_limits<std::int64_t>::min());
        constexpr auto maximum = static_cast<long double>(std::numeric_limits<std::int64_t>::max());
        if (!std::isfinite(delta) || delta < minimum || delta > maximum) return std::nullopt;
        return static_cast<std::int64_t>(delta);
    }
};

class ShadowTracker
{
public:
    void on_simulated_fill(const fill_event& fill)
    {
        static_cast<void>(try_on_simulated_fill(fill));
    }

    void on_exchange_fill(const fill_event& fill) { static_cast<void>(try_on_exchange_fill(fill)); }

    ShadowIngestResult try_on_simulated_fill(const fill_event& fill)
    {
        return ingest(fill, leg::simulated);
    }

    ShadowIngestResult try_on_exchange_fill(const fill_event& fill)
    {
        return ingest(fill, leg::exchange);
    }

    [[nodiscard]] bool valid() const noexcept { return valid_; }
    [[nodiscard]] std::uint64_t error_count() const noexcept { return error_count_; }
    [[nodiscard]] shadow_ingest_error last_error() const noexcept { return last_error_; }

    void print_report() const
    {
        StreamStateGuard stream_state(std::cout);
        std::cout << "\n";
        std::cout << "  ============================================\n";
        std::cout << "    Shadow Mode Report\n";
        std::cout << "  ============================================\n";

        if (!valid_) {
            std::cout << "    INVALID: rejected fill inputs: " << error_count_ << "\n";
            std::cout << "    No comparison metrics are authoritative.\n";
            std::cout << "  ============================================\n\n";
            return;
        }

        std::vector<std::uint64_t> order_ids;
        order_ids.reserve(fills_.size());
        for (const auto& [order_id, fill] : fills_) {
            static_cast<void>(fill);
            order_ids.push_back(order_id);
        }
        std::sort(order_ids.begin(), order_ids.end());

        std::size_t both_filled = 0;
        std::size_t sim_only = 0;
        std::size_t exchange_only = 0;
        std::size_t quantity_divergent = 0;
        RunningMean adverse_bps;
        RunningMean absolute_adverse_bps;
        RunningMean first_event_delta_ms;
        RunningMean last_event_delta_ms;

        for (const std::uint64_t order_id : order_ids) {
            const auto& fill = fills_.at(order_id);
            if (fill.sim_filled && fill.exchange_filled) {
                ++both_filled;
                const auto adverse = fill.comparable_adverse_slippage_bps();
                if (!adverse) {
                    ++quantity_divergent;
                    continue;
                }
                adverse_bps.add(*adverse);
                absolute_adverse_bps.add(std::abs(*adverse));
                if (const auto first = fill.first_fill_event_time_delta_ms())
                    first_event_delta_ms.add(static_cast<double>(*first));
                if (const auto last = fill.last_fill_event_time_delta_ms())
                    last_event_delta_ms.add(static_cast<double>(*last));
            } else if (fill.sim_filled) {
                ++sim_only;
            } else if (fill.exchange_filled) {
                ++exchange_only;
            }
        }

        const std::size_t total = fills_.size();
        std::cout << "    Observed order IDs:      " << total << "\n";
        std::cout << "    Both legs observed:      " << both_filled << "\n";
        std::cout << "    Quantity divergent:      " << quantity_divergent << "\n";
        std::cout << "    Sim leg observed only:   " << sim_only << "\n";
        std::cout << "    Exchange leg observed only: " << exchange_only << "\n";

        if (adverse_bps.count > 0) {
            std::cout << std::fixed << std::setprecision(6);
            std::cout << "    Avg adverse slippage (bps): " << adverse_bps.value << "\n";
            std::cout << "    Avg |adverse| (bps):        " << absolute_adverse_bps.value << "\n";
            std::cout << "    Avg first-fill event delta (ms): " << first_event_delta_ms.value
                      << "\n";
            std::cout << "    Avg last-fill event delta (ms):  " << last_event_delta_ms.value
                      << "\n";
        }

        if (total > 0) {
            const double total_count = static_cast<double>(total);
            const double sim_leg_presence =
                static_cast<double>(both_filled + sim_only) / total_count * 100.0;
            const double exchange_leg_presence =
                static_cast<double>(both_filled + exchange_only) / total_count * 100.0;
            std::cout << std::fixed << std::setprecision(1);
            std::cout << "    Sim leg presence (observed IDs):      " << sim_leg_presence << "%\n";
            std::cout << "    Exchange leg presence (observed IDs): " << exchange_leg_presence
                      << "%\n";
        }

        std::cout << "  ============================================\n\n";
    }

    [[nodiscard]] const std::unordered_map<std::uint64_t, shadow_fill>& fills() const
    {
        return fills_;
    }

    // Clear all per-trial shadow evidence before a reused Monte Carlo trial.
    void reset()
    {
        fills_.clear();
        valid_ = true;
        error_count_ = 0;
        last_error_ = shadow_ingest_error::none;
    }

private:
    enum class leg : std::uint8_t
    {
        simulated,
        exchange
    };

    struct LegUpdate
    {
        double quantity = 0.0;
        double vwap = 0.0;
        std::chrono::system_clock::time_point first_timestamp{};
        std::chrono::system_clock::time_point last_timestamp{};
        std::uint64_t slice_count = 0;
    };

    struct RunningMean
    {
        long double value = 0.0L;
        std::size_t count = 0;

        void add(long double sample) noexcept
        {
            ++count;
            value += (sample - value) / static_cast<long double>(count);
        }
    };

    class StreamStateGuard
    {
    public:
        explicit StreamStateGuard(std::ios& stream) noexcept
            : stream_(stream)
            , flags_(stream.flags())
            , precision_(stream.precision())
        {}

        ~StreamStateGuard()
        {
            stream_.flags(flags_);
            stream_.precision(precision_);
        }

        StreamStateGuard(const StreamStateGuard&) = delete;
        StreamStateGuard& operator=(const StreamStateGuard&) = delete;

    private:
        std::ios& stream_;
        std::ios::fmtflags flags_;
        std::streamsize precision_;
    };

    std::unordered_map<std::uint64_t, shadow_fill> fills_;
    bool valid_ = true;
    std::uint64_t error_count_ = 0;
    shadow_ingest_error last_error_ = shadow_ingest_error::none;

    ShadowIngestResult reject(shadow_ingest_error error) noexcept
    {
        valid_ = false;
        last_error_ = error;
        if (error_count_ != std::numeric_limits<std::uint64_t>::max()) ++error_count_;
        return {error};
    }

    [[nodiscard]] static shadow_ingest_error validate(const fill_event& fill) noexcept
    {
        if (fill.get_order_id() == 0) return shadow_ingest_error::invalid_order_id;
        if (fill.get_symbol().empty()) return shadow_ingest_error::invalid_symbol;
        if (fill.get_side() != order_side::buy && fill.get_side() != order_side::sell)
            return shadow_ingest_error::invalid_side;
        const double quantity = fill.get_filled_quantity();
        if (!std::isfinite(quantity) || quantity <= 0.0)
            return shadow_ingest_error::invalid_quantity;
        const double price = fill.get_fill_price();
        if (!std::isfinite(price) || price <= 0.0) return shadow_ingest_error::invalid_price;
        if (fill.get_timestamp().time_since_epoch().count() <= 0)
            return shadow_ingest_error::invalid_timestamp;
        return shadow_ingest_error::none;
    }

    [[nodiscard]] static std::optional<double> updated_vwap(double old_vwap, double old_quantity,
                                                            double price, double quantity,
                                                            double new_quantity) noexcept
    {
        if (old_quantity == 0.0) return price;
        const double weight = quantity / new_quantity;
        if (!std::isfinite(weight) || weight <= 0.0 || weight > 1.0) return std::nullopt;

        const double result = price >= old_vwap ? old_vwap + weight * (price - old_vwap)
                                                : price + (1.0 - weight) * (old_vwap - price);
        if (!std::isfinite(result) || result <= 0.0) return std::nullopt;
        return result;
    }

    [[nodiscard]] static ShadowIngestResult build_update(const shadow_fill* existing,
                                                         const fill_event& fill, leg target,
                                                         LegUpdate& update) noexcept
    {
        const bool simulated = target == leg::simulated;
        const double old_quantity = existing == nullptr ? 0.0
                                                        : (simulated ? existing->sim_quantity
                                                                     : existing->exchange_quantity);
        const double old_vwap = existing == nullptr
                                    ? 0.0
                                    : (simulated ? existing->sim_price : existing->exchange_price);
        const std::uint64_t old_slice_count =
            existing == nullptr
                ? 0
                : (simulated ? existing->sim_slice_count : existing->exchange_slice_count);
        const double quantity = fill.get_filled_quantity();
        const double new_quantity = old_quantity + quantity;
        if (!std::isfinite(new_quantity) || new_quantity <= old_quantity)
            return {shadow_ingest_error::quantity_overflow};
        if (old_slice_count == std::numeric_limits<std::uint64_t>::max())
            return {shadow_ingest_error::slice_count_overflow};
        const auto vwap =
            updated_vwap(old_vwap, old_quantity, fill.get_fill_price(), quantity, new_quantity);
        if (!vwap) return {shadow_ingest_error::arithmetic_overflow};

        const auto timestamp = fill.get_timestamp();
        const bool first_slice = old_slice_count == 0;
        update.quantity = new_quantity;
        update.vwap = *vwap;
        update.first_timestamp = first_slice
                                     ? timestamp
                                     : std::min(simulated ? existing->sim_first_timestamp
                                                          : existing->exchange_first_timestamp,
                                                timestamp);
        update.last_timestamp = first_slice ? timestamp
                                            : std::max(simulated ? existing->sim_timestamp
                                                                 : existing->exchange_timestamp,
                                                       timestamp);
        update.slice_count = old_slice_count + 1;
        return {};
    }

    [[nodiscard]] static bool comparison_is_representable(const shadow_fill& existing,
                                                          const LegUpdate& update,
                                                          leg target) noexcept
    {
        const bool simulated = target == leg::simulated;
        const bool sim_filled = simulated || existing.sim_filled;
        const bool exchange_filled = !simulated || existing.exchange_filled;
        if (!sim_filled || !exchange_filled) return true;

        const double sim_quantity = simulated ? update.quantity : existing.sim_quantity;
        const double exchange_quantity = simulated ? existing.exchange_quantity : update.quantity;
        if (!shadow_fill::quantities_equivalent(sim_quantity, exchange_quantity)) return true;

        const double sim_price = simulated ? update.vwap : existing.sim_price;
        const double exchange_price = simulated ? existing.exchange_price : update.vwap;
        const double adverse = existing.side == order_side::buy ? exchange_price - sim_price
                                                                : sim_price - exchange_price;
        const double adverse_bps = (adverse / sim_price) * 10000.0;
        return std::isfinite(adverse_bps);
    }

    static void apply_update(shadow_fill& entry, const LegUpdate& update, leg target) noexcept
    {
        if (target == leg::simulated) {
            entry.sim_quantity = update.quantity;
            entry.sim_price = update.vwap;
            entry.sim_first_timestamp = update.first_timestamp;
            entry.sim_timestamp = update.last_timestamp;
            entry.sim_slice_count = update.slice_count;
            entry.sim_filled = true;
        } else {
            entry.exchange_quantity = update.quantity;
            entry.exchange_price = update.vwap;
            entry.exchange_first_timestamp = update.first_timestamp;
            entry.exchange_timestamp = update.last_timestamp;
            entry.exchange_slice_count = update.slice_count;
            entry.exchange_filled = true;
        }
    }

    ShadowIngestResult ingest(const fill_event& fill, leg target)
    {
        const shadow_ingest_error validation_error = validate(fill);
        if (validation_error != shadow_ingest_error::none) return reject(validation_error);

        auto iterator = fills_.find(fill.get_order_id());
        shadow_fill* existing = iterator == fills_.end() ? nullptr : &iterator->second;
        if (existing != nullptr &&
            (existing->symbol != fill.get_symbol() || existing->side != fill.get_side()))
            return reject(shadow_ingest_error::identity_mismatch);

        LegUpdate update;
        const ShadowIngestResult update_result = build_update(existing, fill, target, update);
        if (!update_result) return reject(update_result.error);
        if (existing != nullptr && !comparison_is_representable(*existing, update, target))
            return reject(shadow_ingest_error::arithmetic_overflow);

        if (existing != nullptr) {
            apply_update(*existing, update, target);
            return {};
        }

        shadow_fill candidate;
        candidate.order_id = fill.get_order_id();
        candidate.symbol = fill.get_symbol();
        candidate.side = fill.get_side();
        apply_update(candidate, update, target);
        fills_.emplace(candidate.order_id, std::move(candidate));
        return {};
    }
};
