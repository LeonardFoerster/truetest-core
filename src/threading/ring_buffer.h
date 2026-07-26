#pragma once

#include <atomic>
#include <array>
#include <cassert>
#include <cstddef>
#include <functional>
#include <stdexcept>
#include <type_traits>

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

    bool try_push(const T& item)
    {
        const auto wp = write_pos_.load(std::memory_order_relaxed);
        const auto rp = read_pos_.load(std::memory_order_acquire);

        if (wp - rp >= N)
            return false;

        data_[wp & mask_] = item;
        write_pos_.store(wp + 1, std::memory_order_release);

        const auto occ = wp + 1 - rp;
        auto cur_hw = high_watermark_.load(std::memory_order_relaxed);
        while (occ > cur_hw &&
               !high_watermark_.compare_exchange_weak(cur_hw, occ, std::memory_order_relaxed))
        {}

        // Watermark callback surface (currently unused in tree).
        // Install ONLY at startup before producers (see on_watermark contract).
        // Use acquire to pair with release-store in on_watermark.
        const auto thresh = watermark_threshold_.load(std::memory_order_acquire);
        if (thresh > 0 && occ >= thresh && watermark_cb_)
            watermark_cb_(occ);

        return true;
    }

    bool try_pop(T& item)
    {
        const auto rp = read_pos_.load(std::memory_order_relaxed);
        const auto wp = write_pos_.load(std::memory_order_acquire);

        if (rp >= wp)
            return false;

        item = std::move(data_[rp & mask_]);
        data_[rp & mask_] = T{};
        read_pos_.store(rp + 1, std::memory_order_release);
        return true;
    }

    void push(const T& item)
    {
        if constexpr (Policy::should_spin)
        {
            while (!try_push(item))
            {
            }
        }
        else if constexpr (Policy::should_drop)
        {
            if (!try_push(item))
            {
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

    std::size_t size() const
    {
        const auto wp = write_pos_.load(std::memory_order_relaxed);
        const auto rp = read_pos_.load(std::memory_order_relaxed);
        return wp - rp;
    }

    bool empty() const { return size() == 0; }
    bool full() const { return size() >= N; }
    static constexpr std::size_t capacity() { return N; }

    std::size_t occupancy() const
    {
        return size();
    }

    std::size_t high_watermark() const
    {
        return high_watermark_.load(std::memory_order_relaxed);
    }

    std::size_t drop_count() const
    {
        return drop_count_.load(std::memory_order_relaxed);
    }

    void reset_metrics()
    {
        high_watermark_.store(0, std::memory_order_relaxed);
        drop_count_.store(0, std::memory_order_relaxed);
    }

    void on_watermark(std::size_t threshold, std::function<void(std::size_t)> cb)
    {
        // Contract: call only during startup (single-threaded, before any
        // producers can invoke try_push). The cb is plain std::function;
        // concurrent mutation + invoke is data race / UB.
        // Threshold is atomic to reduce (but not eliminate) races on the flag.
        watermark_cb_ = std::move(cb);
        watermark_threshold_.store(threshold, std::memory_order_release);
    }

private:
    static constexpr std::size_t mask_ = N - 1;

    std::array<T, N> data_;

    alignas(64) std::atomic<std::size_t> write_pos_;
    alignas(64) std::atomic<std::size_t> read_pos_;

    alignas(64) std::atomic<std::size_t> high_watermark_;
    std::atomic<std::size_t> drop_count_;

    // Made atomic for the threshold to reduce (but not eliminate) data-race
    // surface when on_watermark is used concurrently with try_push.
    // The callback itself remains a plain std::function; install it only
    // at startup from a single thread before any producers, or accept
    // best-effort semantics.
    std::atomic<std::size_t> watermark_threshold_{0};
    std::function<void(std::size_t)> watermark_cb_;
};
