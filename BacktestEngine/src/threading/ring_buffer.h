#pragma once

#include <atomic>
#include <array>
#include <cassert>
#include <cstddef>
#include <functional>
#include <stdexcept>
#include <type_traits>

// Backpressure policies for when the ring buffer is full
struct SpinWait
{
    static constexpr bool should_spin = true;
    static constexpr bool should_drop = false;
};

struct DropOldest
{
    static constexpr bool should_spin = false;
    static constexpr bool should_drop = true;
};

struct AssertFull
{
    static constexpr bool should_spin = false;
    static constexpr bool should_drop = false;
};

template <typename T, std::size_t N, typename Policy = SpinWait>
class RingBuffer
{
    static_assert((N & (N - 1)) == 0, "RingBuffer capacity must be a power of 2");
    static_assert(N > 0, "RingBuffer capacity must be positive");

public:
    RingBuffer() : write_pos_(0), read_pos_(0), high_watermark_(0), drop_count_(0) {}

    RingBuffer(const RingBuffer&) = delete;
    RingBuffer& operator=(const RingBuffer&) = delete;

    // Non-blocking push. Returns false if full.
    bool try_push(const T& item)
    {
        const auto wp = write_pos_.load(std::memory_order_relaxed);
        const auto rp = read_pos_.load(std::memory_order_acquire);

        if (wp - rp >= N)
            return false;

        data_[wp & mask_] = item;
        write_pos_.store(wp + 1, std::memory_order_release);

        // Update high watermark (cheap: one relaxed CAS on the hot path)
        const auto occ = wp + 1 - rp;
        auto cur_hw = high_watermark_.load(std::memory_order_relaxed);
        while (occ > cur_hw &&
               !high_watermark_.compare_exchange_weak(cur_hw, occ, std::memory_order_relaxed))
        {}

        // Fire watermark callback if threshold exceeded
        if (watermark_threshold_ > 0 && occ >= watermark_threshold_ && watermark_cb_)
            watermark_cb_(occ);

        return true;
    }

    // Non-blocking pop. Returns false if empty.
    bool try_pop(T& item)
    {
        const auto rp = read_pos_.load(std::memory_order_relaxed);
        const auto wp = write_pos_.load(std::memory_order_acquire);

        if (rp >= wp)
            return false;

        item = std::move(data_[rp & mask_]);
        data_[rp & mask_] = T{};  // clear slot to release resources
        read_pos_.store(rp + 1, std::memory_order_release);
        return true;
    }

    // Policy-aware push: spins, drops oldest, or asserts depending on Policy
    void push(const T& item)
    {
        if constexpr (Policy::should_spin)
        {
            while (!try_push(item))
            {
                // spin
            }
        }
        else if constexpr (Policy::should_drop)
        {
            if (!try_push(item))
            {
                // Advance read position to make room (drop oldest)
                T discard;
                try_pop(discard);
                drop_count_.fetch_add(1, std::memory_order_relaxed);
                try_push(item);
            }
        }
        else
        {
            if (!try_push(item))
                throw std::runtime_error("RingBuffer full (AssertFull policy)");
        }
    }

    // Approximate count (relaxed atomics)
    std::size_t size() const
    {
        const auto wp = write_pos_.load(std::memory_order_relaxed);
        const auto rp = read_pos_.load(std::memory_order_relaxed);
        return wp - rp;
    }

    bool empty() const { return size() == 0; }
    bool full() const { return size() >= N; }
    static constexpr std::size_t capacity() { return N; }

    // Current occupancy (always available)
    std::size_t occupancy() const
    {
        return size();
    }

    // Maximum observed occupancy since construction or last reset
    std::size_t high_watermark() const
    {
        return high_watermark_.load(std::memory_order_relaxed);
    }

    // Number of events dropped (DropOldest policy only)
    std::size_t drop_count() const
    {
        return drop_count_.load(std::memory_order_relaxed);
    }

    // Reset high watermark and drop count
    void reset_metrics()
    {
        high_watermark_.store(0, std::memory_order_relaxed);
        drop_count_.store(0, std::memory_order_relaxed);
    }

    // Set a callback that fires when occupancy exceeds the given threshold.
    // The callback receives the current occupancy. Thread-safe to set before
    // the producer starts; changing while running is a data race on the
    // std::function but the threshold check itself is safe.
    void on_watermark(std::size_t threshold, std::function<void(std::size_t)> cb)
    {
        watermark_threshold_ = threshold;
        watermark_cb_ = std::move(cb);
    }

private:
    static constexpr std::size_t mask_ = N - 1;

    std::array<T, N> data_;

    alignas(64) std::atomic<std::size_t> write_pos_;
    alignas(64) std::atomic<std::size_t> read_pos_;

    // Metrics (always-on, low overhead)
    alignas(64) std::atomic<std::size_t> high_watermark_;
    std::atomic<std::size_t> drop_count_;

    // Optional watermark callback
    std::size_t watermark_threshold_ = 0;
    std::function<void(std::size_t)> watermark_cb_;
};
