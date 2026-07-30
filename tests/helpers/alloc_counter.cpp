#include "alloc_counter.h"

#include <atomic>
#include <cstdlib>
#include <new>

namespace truetest::test::alloc {

namespace {

std::atomic<std::uint64_t> g_count{0};
std::atomic<std::uint64_t> g_bytes{0};

void record(std::size_t n) noexcept
{
    g_count.fetch_add(1, std::memory_order_relaxed);
    g_bytes.fetch_add(static_cast<std::uint64_t>(n), std::memory_order_relaxed);
}

} // namespace

void reset() noexcept
{
    g_count.store(0, std::memory_order_relaxed);
    g_bytes.store(0, std::memory_order_relaxed);
}

snapshot read() noexcept
{
    return {g_count.load(std::memory_order_relaxed),
            g_bytes.load(std::memory_order_relaxed)};
}

} // namespace truetest::test::alloc

// Global new/delete overrides for hotpath alloc counting. ASan/LSan may
// report process-exit noise from this harness path; that is intentional.
void* operator new(std::size_t n)
{
    truetest::test::alloc::record(n);
    if (void* p = std::malloc(n))
        return p;
    throw std::bad_alloc();
}

void* operator new(std::size_t n, const std::nothrow_t&) noexcept
{
    truetest::test::alloc::record(n);
    return std::malloc(n);
}

void* operator new[](std::size_t n)
{
    truetest::test::alloc::record(n);
    if (void* p = std::malloc(n))
        return p;
    throw std::bad_alloc();
}

void* operator new[](std::size_t n, const std::nothrow_t&) noexcept
{
    truetest::test::alloc::record(n);
    return std::malloc(n);
}

void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }