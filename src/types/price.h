#pragma once
#include <cstdint>
#include <cmath>
#include <limits>
#include <string>

class Price {
    int64_t raw_;
public:
    static constexpr int64_t SCALE = 10000;

    constexpr Price() : raw_(0) {}
    constexpr explicit Price(int64_t raw) : raw_(raw) {}

    static bool try_from_double(double d, Price& out) noexcept {
        if (!std::isfinite(d)) return false;
        const long double scaled = static_cast<long double>(d)
            * static_cast<long double>(SCALE);
        const long double rounded = std::round(scaled);
        if (rounded < static_cast<long double>(
                          std::numeric_limits<int64_t>::min())
            || rounded > static_cast<long double>(
                          std::numeric_limits<int64_t>::max()))
            return false;
        out = Price(static_cast<int64_t>(rounded));
        return true;
    }

    static bool is_representable(double d) noexcept {
        Price ignored;
        return try_from_double(d, ignored);
    }

    static Price from_double(double d) {
        return Price(static_cast<int64_t>(std::llround(
            static_cast<long double>(d) * static_cast<long double>(SCALE))));
    }

    double to_double() const { return static_cast<double>(raw_) / SCALE; }
    int64_t raw() const { return raw_; }

    Price operator+(Price o) const { return Price(raw_ + o.raw_); }
    Price operator-(Price o) const { return Price(raw_ - o.raw_); }
    Price operator-() const { return Price(-raw_); }

    Price& operator+=(Price o) { raw_ += o.raw_; return *this; }
    Price& operator-=(Price o) { raw_ -= o.raw_; return *this; }

    bool operator==(Price o) const { return raw_ == o.raw_; }
    bool operator!=(Price o) const { return raw_ != o.raw_; }
    bool operator<(Price o) const { return raw_ < o.raw_; }
    bool operator>(Price o) const { return raw_ > o.raw_; }
    bool operator<=(Price o) const { return raw_ <= o.raw_; }
    bool operator>=(Price o) const { return raw_ >= o.raw_; }
};
