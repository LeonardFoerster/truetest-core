#pragma once

#include "order_audit_sink.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// Manifest schema v1 uses one fixed startup allocation. Exhaustion is a hard
// run failure; deterministic modes never grow or silently fall back to a
// reduced/hash-only lifecycle on the event path.
inline constexpr std::size_t kDeterministicLifecycleRecordCapacityV1 =
    131'072U;

class DeterministicLifecycleSink final : public IOrderAuditSink
{
public:
    explicit DeterministicLifecycleSink(std::size_t capacity);

    void record_order_submitted(const order_event& order,
                                const char* initial_status) override;
    void record_status_transition(std::uint64_t order_id,
                                  order_status old_status,
                                  order_status new_status,
                                  const char* reason) override;
    void record_fill(const fill_event& fill,
                     std::uint64_t opener_order_id,
                     const char* strategy_name,
                     const char* source) override;
    void record_rejection(const order_event& order,
                          const char* category,
                          const char* detail) override;
    void record_cancellation(std::uint64_t order_id,
                             const char* symbol,
                             const char* strategy_name,
                             const char* reason) override;
    void record_amendment(std::uint64_t order_id,
                          const char* symbol,
                          double old_price, double new_price,
                          double old_quantity, double new_quantity,
                          std::chrono::system_clock::time_point timestamp) override;
    void record_funding(const funding_event& event, const char* run_tag) override;
    void record_event(const char* event_type,
                      const char* symbol,
                      const char* strategy_name,
                      std::uint64_t order_id,
                      const char* severity,
                      const char* message,
                      const char* details_json) override;
    void record_exit_lifecycle(const exit_lifecycle_record& record) override;

    [[nodiscard]] std::size_t total_rejections() const override
    {
        return total_rejections_;
    }
    [[nodiscard]] bool overflowed() const noexcept { return overflowed_; }
    [[nodiscard]] std::size_t record_count() const noexcept
    {
        return records_.size();
    }
    [[nodiscard]] static constexpr std::size_t record_size_bytes() noexcept;

    [[nodiscard]] std::string canonical_json_lines() const;
    [[nodiscard]] std::string write_atomic_and_hash(
        const std::filesystem::path& path,
        bool replace_existing = false) const;

private:
    struct Record
    {
        std::uint64_t sequence = 0;
        std::uint64_t order_id = 0;
        std::uint64_t opener_order_id = 0;
        std::uint64_t fill_id = 0;
        std::uint64_t signal_id = 0;
        std::int64_t timestamp_ns = 0;
        std::int64_t submit_timestamp_ns = 0;
        std::int64_t eligible_timestamp_ns = 0;
        std::int64_t fill_timestamp_ns = 0;
        double quantity = 0.0;
        double price = 0.0;
        double commission = 0.0;
        double remaining_quantity = 0.0;
        double old_price = 0.0;
        double old_quantity = 0.0;
        order_status old_status = order_status::unknown;
        order_status new_status = order_status::unknown;
        order_exit_reason exit_reason = order_exit_reason::none;
        std::array<char, 32> kind{};
        std::array<char, 64> symbol{};
        std::array<char, 64> strategy{};
        std::array<char, 64> status_or_source{};
        std::array<char, 160> reason{};
        std::array<char, 384> details{};
    };

    [[nodiscard]] Record* append(std::string_view kind) noexcept;

    template <std::size_t N>
    bool copy_text(std::array<char, N>& destination,
                   std::string_view source) noexcept
    {
        if (source.size() >= N)
        {
            overflowed_ = true;
            return false;
        }
        std::copy(source.begin(), source.end(), destination.begin());
        destination[source.size()] = '\0';
        return true;
    }

    std::vector<Record> records_;
    std::size_t total_rejections_{0};
    bool overflowed_{false};
};

constexpr std::size_t
DeterministicLifecycleSink::record_size_bytes() noexcept
{
    return sizeof(Record);
}
