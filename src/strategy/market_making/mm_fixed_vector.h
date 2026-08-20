#pragma once

#include <array>
#include <cstddef>
#include <type_traits>

namespace truetest::mm
{

// Fixed-capacity sequence container for the quote decision payload.
//
// The repository has no shared static_vector and the hot path forbids heap
// growth (AGENTS.md R1/R2), so the strategy's quote and reason lists live in
// a value type whose storage is part of the decision object. Deliberately
// minimal: push_back reports refusal instead of growing, matching the
// "exhaust → fail closed" rule rather than silently dropping.
template <typename T, std::size_t Capacity>
class fixed_vector
{
    static_assert(std::is_trivially_copyable_v<T>,
                  "fixed_vector stores hot-path PODs only");

public:
    using value_type = T;
    using iterator = T*;
    using const_iterator = const T*;

    static constexpr std::size_t capacity() noexcept { return Capacity; }

    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
    [[nodiscard]] bool full() const noexcept { return size_ == Capacity; }

    void clear() noexcept { size_ = 0; }

    // Returns false when full. Callers on the hot path must handle refusal
    // explicitly; there is no growth path.
    bool push_back(const T& value) noexcept
    {
        if (size_ >= Capacity)
            return false;
        data_[size_] = value;
        ++size_;
        return true;
    }

    // Idempotent insert used for reason codes: the same reason must not
    // appear twice and must not consume capacity needed by a later, more
    // severe reason.
    bool push_unique(const T& value) noexcept
    {
        if (contains(value))
            return true;
        return push_back(value);
    }

    [[nodiscard]] bool contains(const T& value) const noexcept
    {
        for (std::size_t i = 0; i < size_; ++i)
            if (data_[i] == value)
                return true;
        return false;
    }

    T& operator[](std::size_t i) noexcept { return data_[i]; }
    const T& operator[](std::size_t i) const noexcept { return data_[i]; }

    iterator begin() noexcept { return data_.data(); }
    iterator end() noexcept { return data_.data() + size_; }
    const_iterator begin() const noexcept { return data_.data(); }
    const_iterator end() const noexcept { return data_.data() + size_; }

private:
    std::array<T, Capacity> data_{};
    std::size_t size_ = 0;
};

} // namespace truetest::mm
