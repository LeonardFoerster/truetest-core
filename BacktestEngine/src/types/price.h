#pragma once
#include <cstdint>
#include <cmath>
#include <string>

// Fixed-point price with 4 decimal places.
// Internal: int64_t where 1 = 0.0001 (1/10000th)
// Range: +/-922,337,203,685.4775 — sufficient for any asset.
class Price {
    int64_t raw_;  // 1 unit = 0.0001
public:
    static constexpr int64_t SCALE = 10000;

    constexpr Price() : raw_(0) {}
    constexpr explicit Price(int64_t raw) : raw_(raw) {}

    // Convert from double (use only at data ingestion boundaries)
    static Price from_double(double d) {
        return Price(static_cast<int64_t>(std::llround(d * SCALE)));
    }

    double to_double() const { return static_cast<double>(raw_) / SCALE; }
    int64_t raw() const { return raw_; }

    // Arithmetic
    Price operator+(Price o) const { return Price(raw_ + o.raw_); }
    Price operator-(Price o) const { return Price(raw_ - o.raw_); }
    Price operator-() const { return Price(-raw_); }

    Price& operator+=(Price o) { raw_ += o.raw_; return *this; }
    Price& operator-=(Price o) { raw_ -= o.raw_; return *this; }

    // Comparison
    bool operator==(Price o) const { return raw_ == o.raw_; }
    bool operator!=(Price o) const { return raw_ != o.raw_; }
    bool operator<(Price o) const { return raw_ < o.raw_; }
    bool operator>(Price o) const { return raw_ > o.raw_; }
    bool operator<=(Price o) const { return raw_ <= o.raw_; }
    bool operator>=(Price o) const { return raw_ >= o.raw_; }
};
