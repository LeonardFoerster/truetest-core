#pragma once

// Heap allocation instrumentation for hot-path baseline tests (Phase 0).
// Linked via tests/helpers/alloc_counter.cpp into truetest_tests only.
// Overrides global operator new/delete for the test executable so engine_core
// allocations are counted during measured windows.

#include <cstddef>
#include <cstdint>

namespace truetest::test::alloc {

struct snapshot
{
    std::uint64_t count = 0;
    std::uint64_t bytes = 0;
};

void reset() noexcept;
snapshot read() noexcept;

// Resets the global counter on construction; read() returns the window total.
class measure_window
{
public:
    measure_window() { reset(); }
    ~measure_window() = default;

    measure_window(const measure_window&) = delete;
    measure_window& operator=(const measure_window&) = delete;

    snapshot total() const noexcept { return read(); }
};

} // namespace truetest::test::alloc