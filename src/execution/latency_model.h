#pragma once

#include <chrono>
#include <random>
#include <algorithm>

using latency_duration = std::chrono::microseconds;

class ILatencyModel
{
public:
    virtual ~ILatencyModel() = default;
    virtual latency_duration get_order_latency() = 0;
    virtual latency_duration get_market_data_latency() = 0;

    // Override for venues where cancel ≠ submit (Binance REST: ~2×).
    virtual latency_duration get_cancel_latency() { return get_order_latency(); }
};

class ZeroLatencyModel : public ILatencyModel
{
public:
    latency_duration get_order_latency() override { return latency_duration(0); }
    latency_duration get_market_data_latency() override { return latency_duration(0); }
    latency_duration get_cancel_latency() override { return latency_duration(0); }
};

class FixedLatencyModel : public ILatencyModel
{
public:
    explicit FixedLatencyModel(latency_duration order_latency,
                               latency_duration market_data_latency = latency_duration(0),
                               latency_duration cancel_latency = latency_duration(-1))
        : order_latency_(order_latency),
          market_data_latency_(market_data_latency),
          // -1 = mirror order_latency (preserves the 2-arg ctor behavior).
          cancel_latency_(cancel_latency.count() < 0 ? order_latency : cancel_latency) {}

    latency_duration get_order_latency() override { return order_latency_; }
    latency_duration get_market_data_latency() override { return market_data_latency_; }
    latency_duration get_cancel_latency() override { return cancel_latency_; }

private:
    latency_duration order_latency_;
    latency_duration market_data_latency_;
    latency_duration cancel_latency_;
};

class StochasticLatencyModel : public ILatencyModel
{
public:
    StochasticLatencyModel(double mean_us, double stddev_us, unsigned seed = 42)
        : gen_(seed), order_dist_(mean_us, stddev_us), md_dist_(mean_us, stddev_us),
          cancel_dist_(mean_us, stddev_us) {}

    StochasticLatencyModel(double order_mean_us, double order_stddev_us,
                           double md_mean_us, double md_stddev_us,
                           unsigned seed = 42)
        : gen_(seed), order_dist_(order_mean_us, order_stddev_us),
          md_dist_(md_mean_us, md_stddev_us),
          cancel_dist_(order_mean_us, order_stddev_us) {}

    latency_duration get_order_latency() override
    {
        auto val = static_cast<long long>(std::max(0.0, order_dist_(gen_)));
        return latency_duration(val);
    }

    latency_duration get_market_data_latency() override
    {
        auto val = static_cast<long long>(std::max(0.0, md_dist_(gen_)));
        return latency_duration(val);
    }

    latency_duration get_cancel_latency() override
    {
        auto val = static_cast<long long>(std::max(0.0, cancel_dist_(gen_)));
        return latency_duration(val);
    }

private:
    std::mt19937 gen_;
    std::normal_distribution<double> order_dist_;
    std::normal_distribution<double> md_dist_;
    std::normal_distribution<double> cancel_dist_;
};
