# HFT Crypto Trading Engine: A Complete Deep Dive
### C/C++ · TimescaleDB · Dedicated Server · 1–5ms RTT Target

> **Status: target architecture, not current reality.** This document is the
> long-form engineering vision the refactor in `../prerequisites.md` and
> `../todo.md` is walking the codebase toward. The authoritative description of
> what the repo *actually* builds today is [`../CLAUDE.md`](../CLAUDE.md);
> operator docs live in [`user-manual.md`](user-manual.md).
>
> **Current-code deviations from this document**:
>
> - **Venue:** current code targets **Binance** spot via `providers/binance/`.
>   The deepdive references Bitstamp throughout — treat exchange-specific
>   pseudocode as venue-agnostic. Bitstamp is a future `IProvider` addition,
>   not a replacement. See CLAUDE.md "Stack decisions".
> - **Persistence:** current default is **SQLite** (`ENABLE_SQLITE=ON`).
>   PostgreSQL + TimescaleDB is opt-in (`ENABLE_POSTGRESQL=ON`) and is not
>   the default path yet.
> - **JSON:** `nlohmann/json` is confined to config-time code
>   (`src/main.cpp`, `src/api/truetest_api.cpp`). The deepdive's simdjson
>   hot-path parser is a Phase 3 item; the current hot path uses
>   hand-rolled snprintf/string-extract.
> - **Language level:** current code is **C++23** (`CMAKE_CXX_STANDARD 23`),
>   ahead of the deepdive's C++20 baseline.
> - **Directory graph:** enforced dependency direction (`core/` → no deps,
>   `engine/` → orchestration) is partly in place; `tt_<module>` OBJECT
>   library isolation (§10) is still pending.
> - **LibTorch / `engine_ml`:** not yet added; ML-pipeline sections are
>   forward-looking.
>
> Revisit this preamble at every phase boundary — when an item above lands,
> delete the matching bullet.

---

## Preface

This document is a complete engineering reference for building a semi-professional High-Frequency Trading engine in C and C++. It covers every layer of the system — from how the OS boots your process, to how a neural network trained on your own fills learns to improve your strategy. Nothing is assumed to be "someone else's problem." Every design decision explained here has a *why*, because understanding the reasoning is what separates an engine you can trust from one you merely hope works.

The target environment is a dedicated root server with 64 GB of RAM, running Linux. The exchange is Bitstamp, communicating via REST and WebSocket. The database is TimescaleDB (a PostgreSQL extension). The language is C++ throughout, with C-style patterns used deliberately in hot paths where zero-overhead abstraction is required. No Python. No Java. No Go. Every microsecond belongs to you.

---

## Part 1 — Project Structure and Build System

### 1.1 Directory Layout

The way you lay out your project on day one shapes how easy or hard it is to reason about the codebase six months later. An HFT engine has several distinct subsystems that have genuinely different concerns — the backtest harness, the live execution engine, the risk layer, the database writer — and co-mingling them creates the kind of spaghetti that causes bugs during market hours.

A layout that scales well looks like this:

```
hft_engine/
├── CMakeLists.txt              # Root build file
├── cmake/                      # Find modules, toolchain files
│   ├── CompilerFlags.cmake
│   └── Dependencies.cmake
├── src/
│   ├── core/                   # Shared kernel: types, allocators, time, IDs
│   │   ├── order.hpp
│   │   ├── pool_allocator.hpp
│   │   ├── ring_buffer.hpp
│   │   └── clock.hpp
│   ├── network/                # WebSocket client, REST client, epoll loop
│   │   ├── websocket_client.cpp
│   │   ├── rest_client.cpp
│   │   └── epoll_reactor.cpp
│   ├── execution/              # Order lifecycle, state machine, send queue
│   │   ├── order_manager.cpp
│   │   └── execution_engine.cpp
│   ├── risk/                   # Pre-trade checks, circuit breakers, kill switch
│   │   ├── risk_manager.cpp
│   │   └── circuit_breaker.cpp
│   ├── strategy/               # Abstract base, concrete strategies
│   │   ├── strategy_base.hpp
│   │   └── strategies/
│   ├── db/                     # TimescaleDB writer, schema, queries
│   │   ├── db_writer.cpp
│   │   └── schema.sql
│   ├── logging/                # Binary ring buffer logger, NDJSON drain
│   │   └── logger.cpp
│   ├── ml/                     # Post-trade DL pipeline (LibTorch C++ API)
│   │   ├── feature_extractor.cpp
│   │   └── model_trainer.cpp
│   └── main.cpp
├── backtest/                   # Backtest harness, data loader
├── tests/                      # Unit + integration tests (Catch2)
├── tools/                      # Latency benchmark, schema migration scripts
└── config/
    ├── backtest.toml
    ├── shadow.toml
    └── live.toml               # Never committed to version control
```

The `core/` directory deserves special attention. This is where your primitive types live — `Order`, `Fill`, `Tick`, `Price` — and they must have zero dependencies on anything else in the project. Everything depends on `core/`, and `core/` depends on nothing. This dependency discipline prevents circular includes and means you can test core types in complete isolation.

### 1.2 CMake Setup and Compiler Flags

CMake is the right choice here because it gives you fine-grained control over per-target compilation flags, which matters enormously when parts of your code (backtest data loader) can tolerate slower builds with debug symbols while other parts (the hot execution path) must be compiled with every optimization the compiler can offer.

```cmake
# CMakeLists.txt (root)
cmake_minimum_required(VERSION 3.22)
project(hft_engine CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Base flags applied to all targets
add_compile_options(
    -Wall -Wextra -Wno-unused-parameter
    -fno-exceptions        # Exceptions have hidden overhead; use error codes instead
    -fno-rtti              # RTTI adds vtable bloat; use explicit type tags
)

# Release-mode flags for the live trading binary
# These are applied only to the 'engine_live' target
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    target_compile_options(engine_live PRIVATE
        -O3                    # Maximum optimization
        -march=native          # Use every instruction your specific CPU supports
        -mtune=native          # Tune scheduling for your specific microarchitecture
        -flto                  # Link-time optimization: inlines across TUs
        -fprofile-use          # Enable PGO (after profile generation step)
        -funroll-loops         # Unroll small loops; often wins in tight market-data parsing
        -fomit-frame-pointer   # Frees one register; minor but measurable in tight loops
    )
endif()
```

Profile-Guided Optimization (PGO) deserves its own explanation because most developers skip it and leave significant performance on the table. PGO is a two-phase compilation process. In the first phase, you compile with `-fprofile-generate`, then run the engine in shadow trading mode for several hours — this instruments the binary and records which code paths are hot, which branches are taken, which functions are called most. In the second phase, you recompile with `-fprofile-use`, and the compiler uses this data to make much better inlining, branch layout, and loop unrolling decisions than it could from static analysis alone. In tight market-data-processing loops, PGO commonly yields 10–20% throughput improvements with zero code changes.

### 1.3 Separate Compilation Targets

You want three distinct executables built from the same source tree. The `engine_backtest` binary links against the data loader and fake order manager, never against network code or live credentials. The `engine_shadow` binary connects to real WebSocket feeds but routes all orders through a simulated fill engine. The `engine_live` binary is the only one that ever touches real API keys, and its build should be the only one that links against the production credential store.

This separation matters not just for safety but for correctness: your backtest harness should use deterministic tick-by-tick replay without any threading, while your live engine uses a fully asynchronous multi-threaded architecture. Trying to share a single binary for both creates a design that serves neither well.

---

## Part 2 — Memory Architecture

### 2.1 The Core Problem with the Default Heap

The default heap allocator (`malloc`/`new`) is a general-purpose tool designed to work adequately across an enormous range of allocation patterns. That generality comes with costs that are unacceptable in a hot trading path: it acquires a mutex on every allocation (in a multithreaded program), it may call into the kernel to request more memory via `sbrk` or `mmap`, it produces fragmentation over time that causes cache thrashing, and its allocation latency is non-deterministic — sometimes 50ns, sometimes 5µs, depending on the state of the free list.

The solution is to *never allocate in the hot path at all*. Every object you will ever need during a trading session should be allocated at startup, placed into a pool, and reused. The hot path only takes from and returns to the pool, which is a handful of pointer operations.

### 2.2 Pool Allocator for Order Objects

An `Order` object has a fixed, known size. It's created when a strategy signals intent, and destroyed when the order reaches a terminal state. This lifecycle maps perfectly onto a pool allocator:

```cpp
// core/pool_allocator.hpp
// A fixed-size object pool. Thread-unsafe by design — intended for use
// on a single thread (the execution thread). If cross-thread allocation
// is needed, use a lock-free freelist instead.
template<typename T, std::size_t Capacity>
class PoolAllocator {
public:
    PoolAllocator() {
        // Pre-link all slots into the freelist at construction time.
        // After this, every alloc/free is O(1) with no syscalls.
        for (std::size_t i = 0; i < Capacity - 1; ++i) {
            // We store the "next free" pointer inside the unused slot memory.
            // This is safe because the slot isn't live yet.
            reinterpret_cast<Slot*>(&storage_[i])->next = &storage_[i + 1];
        }
        reinterpret_cast<Slot*>(&storage_[Capacity - 1])->next = nullptr;
        freelist_head_ = &storage_[0];
    }

    // Allocate one T-sized slot. Returns nullptr if pool is exhausted.
    // Call site should treat nullptr as a FATAL error — it means your
    // capacity estimate was wrong and needs to be increased.
    T* allocate() {
        if (__builtin_expect(freelist_head_ == nullptr, 0)) {
            return nullptr; // pool exhausted — this should never happen in production
        }
        Slot* slot = freelist_head_;
        freelist_head_ = slot->next;
        return reinterpret_cast<T*>(slot);
    }

    void deallocate(T* ptr) {
        ptr->~T(); // call destructor explicitly
        Slot* slot = reinterpret_cast<Slot*>(ptr);
        slot->next = freelist_head_;
        freelist_head_ = slot;
    }

private:
    // A Slot is the same size as T but can also hold a freelist pointer.
    // We use a union to avoid aliasing violations.
    union Slot {
        alignas(T) std::byte data[sizeof(T)];
        Slot* next;
    };

    // The entire pool lives in this array. It's allocated once at program
    // startup (or as a class member) and never moves.
    std::array<Slot, Capacity> storage_;
    Slot* freelist_head_;
};

// Concrete instantiation for orders — 1024 simultaneous live orders
// is far more than you'll ever need at this tier, but costs only
// 1024 * sizeof(Order) bytes of RSS, which is negligible.
using OrderPool = PoolAllocator<Order, 1024>;
```

### 2.3 Cache Line Alignment

Modern CPUs read and write memory in 64-byte cache lines. If a data structure straddles two cache lines, accessing it requires two cache-line loads instead of one — a guaranteed performance penalty. Worse, if two threads read and write *different fields* of the same struct, but those fields share a cache line, every write by one thread invalidates the cache line for the other thread. This is called *false sharing*, and it can reduce throughput by 10x in multithreaded code.

The solution is to align hot structures to cache line boundaries and pad them to fill the full line:

```cpp
// core/order.hpp
// alignas(64) guarantees the struct starts at a 64-byte boundary.
// The padding at the end guarantees no other data shares our cache line.
struct alignas(64) Order {
    // Hot fields: accessed on every tick by the execution thread
    uint64_t    id;               // 8 bytes
    double      price;            // 8 bytes
    double      quantity;         // 8 bytes
    OrderState  state;            // 4 bytes (enum)
    Side        side;             // 1 byte
    uint8_t     strategy_id;      // 1 byte
    uint8_t     _pad0[2];         // explicit padding to align next field

    // Timestamps: written once, read rarely
    int64_t     created_ns;       // nanoseconds since epoch
    int64_t     sent_ns;
    int64_t     acked_ns;
    int64_t     filled_ns;

    // String fields: accessed only for logging/DB writes, not in hot path
    char        symbol[16];       // fixed-length, no heap allocation
    char        exchange_order_id[32];

    // Pad to exactly 128 bytes (2 cache lines) — larger than 64 bytes
    // because the total fields exceed 64, and we'd rather pad cleanly
    // to 128 than leave half a cache line shared with another object.
    uint8_t _pad1[128 - sizeof(uint64_t)*5 - sizeof(double)*2
                      - sizeof(OrderState) - sizeof(Side)
                      - sizeof(uint8_t)*2 - sizeof(char)*(16+32)];
};

// Compile-time verification that our size is exactly what we expect.
// This will fail to compile if you add fields without updating the padding.
static_assert(sizeof(Order) == 128, "Order size mismatch — update padding");
static_assert(alignof(Order) == 64, "Order alignment mismatch");
```

### 2.4 Huge Pages

The CPU's Translation Lookaside Buffer (TLB) is a small cache that stores virtual-to-physical address mappings. On a system with 4KB pages (the default), even a modest 64MB working set requires 16,384 TLB entries — far more than the TLB can hold (~64–1024 entries on typical hardware). Every TLB miss forces a page table walk, which costs 100–200ns. With 2MB huge pages, that same 64MB working set fits in just 32 TLB entries, effectively eliminating TLB pressure.

To enable huge pages for your market data buffer:

```cpp
#include <sys/mman.h>

// Allocate a 64MB ring buffer backed by 2MB huge pages.
// MAP_HUGETLB requires either transparent huge pages (automatic)
// or explicit reservation via /proc/sys/vm/nr_hugepages.
void* alloc_huge(std::size_t size) {
    void* ptr = mmap(
        nullptr,
        size,
        PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB,
        -1, 0
    );
    if (ptr == MAP_FAILED) {
        // Fall back to regular pages — huge pages may not be available
        ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    }
    return ptr;
}
```

At the OS level, reserve huge pages before any application starts (add to `/etc/sysctl.conf`):

```bash
# Reserve 512 × 2MB = 1GB of huge pages for the trading engine.
# Do this at boot time — allocating huge pages at runtime may fail
# if physical memory has become fragmented.
vm.nr_hugepages = 512
```

### 2.5 Memory-Mapped Files for Backtesting

When replaying historical tick data for backtesting, you have a choice between reading the file into a `std::vector` or memory-mapping it. Memory mapping is almost always better for large datasets: the OS handles paging data in on demand, you avoid a copy from kernel to user space, and repeated backtests over the same data will hit the page cache (effectively RAM) after the first run.

```cpp
#include <sys/mman.h>
#include <fcntl.h>
#include <sys/stat.h>

class MmapTickReader {
public:
    explicit MmapTickReader(const char* path) {
        fd_ = open(path, O_RDONLY);
        struct stat st;
        fstat(fd_, &st);
        size_ = st.st_size;

        // PROT_READ: read-only — the OS can share this mapping
        // between multiple backtest processes running in parallel.
        // MAP_POPULATE: pre-fault pages into memory now, so the
        // first pass over the data has no page faults.
        data_ = static_cast<const char*>(
            mmap(nullptr, size_, PROT_READ,
                 MAP_PRIVATE | MAP_POPULATE, fd_, 0)
        );

        // Hint to the kernel: we'll access this sequentially.
        // This causes the kernel to prefetch ahead aggressively.
        madvise(const_cast<char*>(data_), size_, MADV_SEQUENTIAL);
    }

    ~MmapTickReader() {
        munmap(const_cast<char*>(data_), size_);
        close(fd_);
    }

    const char* data() const { return data_; }
    std::size_t size() const { return size_; }

private:
    int         fd_;
    const char* data_;
    std::size_t size_;
};
```

---

## Part 3 — Networking Layer

### 3.1 The Bitstamp Protocol Reality

Before designing the network layer, it's important to be clear-eyed about what Bitstamp actually offers. Unlike CME or NASDAQ, which offer co-location and FIX/ITCH protocols where single-digit microsecond latency is achievable, Bitstamp is a retail-to-semi-professional exchange that communicates over WebSocket (for market data and order updates) and HTTPS REST (for order management). This means your fundamental latency floor is dictated by TCP round-trip time to their servers, which is why your 1–5ms RTT target is realistic — it's the network RTT, not message processing time. Your job is to make sure your software overhead contributes as close to zero additional latency as possible on top of that floor.

### 3.2 The epoll Event Loop

The right I/O multiplexing primitive on Linux is `epoll`. It scales to thousands of file descriptors with O(1) event delivery, unlike `select` (which is O(n) and limited to 1024 fds) or `poll` (O(n)). Your entire network layer should be driven by a single `epoll` event loop running on its own pinned thread.

```cpp
// network/epoll_reactor.cpp
// This class owns the epoll file descriptor and dispatches events
// to registered handlers. It runs on a single dedicated thread and
// should never block — all I/O is non-blocking.
class EpollReactor {
public:
    EpollReactor() {
        // EPOLL_CLOEXEC: close the fd automatically on exec(),
        // preventing accidental inheritance by child processes.
        epfd_ = epoll_create1(EPOLL_CLOEXEC);
    }

    // Register a file descriptor with a callback for readable events.
    // EPOLLET = edge-triggered: we get ONE notification when data arrives,
    // not a notification per byte. This requires us to read until EAGAIN,
    // but dramatically reduces epoll_wait() calls in high-throughput scenarios.
    void register_fd(int fd, std::function<void()> on_readable) {
        handlers_[fd] = std::move(on_readable);
        epoll_event ev{};
        ev.events   = EPOLLIN | EPOLLET;
        ev.data.fd  = fd;
        epoll_ctl(epfd_, EPOLL_CTL_ADD, fd, &ev);
    }

    // Run the event loop. This blocks indefinitely and should be
    // called from a dedicated thread (not main).
    void run() {
        std::array<epoll_event, 64> events;
        while (running_.load(std::memory_order_relaxed)) {
            // Timeout of 1ms: we wake up periodically even without events
            // to check the shutdown flag, heartbeat timers, etc.
            int n = epoll_wait(epfd_, events.data(), events.size(), 1);
            for (int i = 0; i < n; ++i) {
                int fd = events[i].data.fd;
                if (auto it = handlers_.find(fd); it != handlers_.end()) {
                    it->second(); // dispatch to registered handler
                }
            }
        }
    }

private:
    int  epfd_;
    std::atomic<bool> running_{true};
    std::unordered_map<int, std::function<void()>> handlers_;
};
```

### 3.3 Socket Tuning

Raw socket performance is heavily influenced by kernel parameters. Setting these correctly at socket creation time (not just in sysctl) ensures they apply specifically to your trading sockets:

```cpp
void configure_trading_socket(int fd) {
    int one = 1;
    int buf_size = 4 * 1024 * 1024; // 4MB send/receive buffers

    // TCP_NODELAY: disable Nagle's algorithm.
    // Nagle buffers small writes and coalesces them to reduce packet count.
    // This is great for bulk data transfer but catastrophic for trading —
    // it can add up to 40ms of artificial delay on small REST payloads.
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    // Larger socket buffers allow the kernel to buffer more data
    // in-flight without stalling. Important for high-throughput
    // WebSocket streams during volatile market conditions.
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buf_size, sizeof(buf_size));
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buf_size, sizeof(buf_size));

    // SO_KEEPALIVE + TCP keepalive parameters: detect dead connections
    // without waiting for the full TCP timeout (which can be minutes).
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
    int keepidle  = 5;  // start probing after 5s of silence
    int keepintvl = 2;  // probe every 2s
    int keepcnt   = 3;  // give up after 3 failed probes
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE,  &keepidle,  sizeof(keepidle));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &keepintvl, sizeof(keepintvl));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT,   &keepcnt,   sizeof(keepcnt));

    // Non-blocking I/O: all reads/writes return immediately.
    // EAGAIN means "try again later" — handle this in your read loop.
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}
```

### 3.4 JSON Parsing with simdjson

Every WebSocket message from Bitstamp arrives as JSON. Parsing JSON is surprisingly expensive if done naively — a simple `nlohmann::json::parse()` call on a 500-byte tick message can take 3–10µs. At the throughput required for HFT, this becomes a meaningful fraction of your total processing budget.

simdjson is a SIMD-accelerated JSON parser that uses SSE4.2 and AVX2 instructions to parse multiple characters simultaneously. On typical trading payloads, it's 2–5x faster than nlohmann and 10x faster than RapidJSON in many benchmarks:

```cpp
#include <simdjson.h>

// Declare the parser ONCE at class scope — it pre-allocates an internal
// buffer and reuses it across calls. Creating a new parser per message
// defeats the purpose of the optimization entirely.
class BitstampFeedParser {
public:
    bool parse_trade(const char* json, std::size_t len, Trade& out) {
        // parse_many/ondemand: lazily parses only the fields you access,
        // skipping everything else. Perfect for structured messages where
        // you only need 3 of 15 fields.
        auto doc = parser_.iterate(json, len, len + simdjson::SIMDJSON_PADDING);
        if (doc.error()) return false;

        // Access fields by key — only these are actually parsed.
        out.price    = doc["data"]["price"].get_double().value();
        out.quantity = doc["data"]["amount"].get_double().value();
        out.ts_ms    = doc["data"]["microtimestamp"].get_int64().value();
        return true;
    }

private:
    simdjson::ondemand::parser parser_; // persistent, pre-allocated
};
```

---

## Part 4 — Order Lifecycle and Execution Engine

### 4.1 The Lock-Free SPSC Ring Buffer

The single most important data structure between your strategy thread and your execution thread is the order queue. You need it to be fast (nanosecond enqueue/dequeue), safe (no data races), and non-allocating (the hot path cannot touch the heap). A Single-Producer Single-Consumer (SPSC) ring buffer satisfies all three requirements.

The key insight behind a lock-free SPSC ring buffer is that only *one* thread writes the head pointer and only *one* thread writes the tail pointer. This means you don't need a mutex — you only need `std::atomic` loads and stores with carefully chosen memory orders to prevent CPU/compiler reordering:

```cpp
// core/ring_buffer.hpp
template<typename T, std::size_t Capacity>
class SPSCRingBuffer {
    // Capacity MUST be a power of two. This allows us to use
    // bitmask modulo (& mask_) instead of the modulo operator (%),
    // which is 10–50x faster on most CPUs.
    static_assert((Capacity & (Capacity - 1)) == 0,
                  "Capacity must be a power of two");

public:
    // Called by producer thread (strategy thread).
    // Returns false if the buffer is full — caller should back off.
    bool push(const T& item) {
        const auto head = head_.load(std::memory_order_relaxed);
        const auto next_head = (head + 1) & mask_;

        // Check if full. We load tail with ACQUIRE so we see all
        // writes the consumer has made to the data it consumed.
        if (next_head == tail_.load(std::memory_order_acquire)) {
            return false; // full
        }

        data_[head] = item;

        // RELEASE: ensure the data write above is visible to the
        // consumer BEFORE the head update is visible.
        head_.store(next_head, std::memory_order_release);
        return true;
    }

    // Called by consumer thread (execution thread).
    bool pop(T& item) {
        const auto tail = tail_.load(std::memory_order_relaxed);

        // Check if empty. ACQUIRE so we see the data written by the producer.
        if (tail == head_.load(std::memory_order_acquire)) {
            return false; // empty
        }

        item = data_[tail];

        // RELEASE: signal to the producer that this slot is now free.
        tail_.store((tail + 1) & mask_, std::memory_order_release);
        return true;
    }

private:
    static constexpr std::size_t mask_ = Capacity - 1;

    // Pad head and tail onto separate cache lines to prevent false sharing.
    // Without this, the producer writing head_ and the consumer writing tail_
    // would bounce the same cache line between CPU cores on every operation.
    alignas(64) std::atomic<std::size_t> head_{0};
    alignas(64) std::atomic<std::size_t> tail_{0};

    // The data array itself is sized to Capacity items.
    std::array<T, Capacity> data_;
};

// The actual order queue: 4096 slots, each holding an Order.
// 4096 is generous — at 1ms order round-trips, you'd need to
// generate >4 million orders/second to overflow this.
using OrderQueue = SPSCRingBuffer<Order*, 4096>;
```

### 4.2 Execution Thread and CPU Affinity

The execution thread — the thread that dequeues orders, builds REST requests, and submits them to Bitstamp — should be pinned to a dedicated CPU core with real-time scheduling priority. Pinning prevents the OS scheduler from migrating the thread between cores (which causes cache warm-up delays) and ensures no other process shares the core:

```cpp
#include <pthread.h>
#include <sched.h>

void configure_execution_thread() {
    // Pin this thread to CPU core 3.
    // Cores 0–1 are typically used by the OS and IRQ handlers.
    // Core 2 can be used for the network I/O thread.
    // Core 3 is reserved exclusively for order execution.
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(3, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);

    // SCHED_FIFO: real-time scheduling. This thread will never be
    // preempted by a non-real-time thread, and will run to completion
    // unless it voluntarily yields or blocks on I/O.
    // Priority 80: high enough to preempt normal work, low enough
    // not to starve kernel threads (which run at 99).
    sched_param sp{};
    sp.sched_priority = 80;
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);

    // Lock all current and future pages into RAM.
    // Prevents page faults (which could add hundreds of microseconds)
    // during trading by ensuring nothing is swapped to disk.
    mlockall(MCL_CURRENT | MCL_FUTURE);
}
```

To make `SCHED_FIFO` work without being root, add this to `/etc/security/limits.conf`:

```
trading_user  -  rtprio  99
trading_user  -  memlock  unlimited
```

### 4.3 Timestamps and Clock Selection

Timestamping orders correctly requires understanding the Linux clock hierarchy. `CLOCK_REALTIME` gives you wall-clock time but can jump backwards due to NTP corrections. `CLOCK_MONOTONIC` is always monotonically increasing but is not tied to wall time. For trading, you need both: `CLOCK_REALTIME` for correlating with exchange timestamps, `CLOCK_MONOTONIC` for measuring internal durations.

The critical optimization here is that `clock_gettime()` is implemented as a vDSO (virtual Dynamic Shared Object) call on modern Linux. This means it executes *entirely in user space* without a context switch to the kernel — it takes roughly 20–50ns rather than the 1–2µs a full syscall would cost.

```cpp
// core/clock.hpp
inline int64_t now_ns_realtime() {
    timespec ts;
    // CLOCK_REALTIME_COARSE is even faster (~10ns) but has 1ms resolution —
    // acceptable for logging, not for latency measurement.
    clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1'000'000'000LL + ts.tv_nsec;
}

inline int64_t now_ns_monotonic() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1'000'000'000LL + ts.tv_nsec;
}

// TSC-based timing: the absolute fastest timestamp you can take.
// rdtsc reads the CPU's Time Stamp Counter directly — ~3ns.
// WARNING: on multi-socket systems, TSC may not be synchronized
// across NUMA nodes. On a single-socket server (which yours likely is),
// this is safe with invariant TSC (check: grep "constant_tsc" /proc/cpuinfo).
inline uint64_t rdtsc() {
    uint64_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return (hi << 32) | lo;
}
```

---

## Part 5 — Risk Management Layer

### 5.1 Pre-Trade Risk Checks

Every single order must pass through a risk check before it is ever placed in the send queue. This check must be fast (it runs in the hot path), complete (it catches every dangerous condition), and conservative (when in doubt, reject). The risk manager should be a pure function of the order and the current system state — no I/O, no allocation, no blocking:

```cpp
// risk/risk_manager.hpp
class RiskManager {
public:
    struct Limits {
        double max_order_notional_usd  = 500.0;   // single order
        double max_position_usd        = 5000.0;  // gross exposure
        double max_daily_loss_usd      = -1000.0; // PnL floor
        double max_price_deviation_pct = 2.0;     // vs. last known mid
        int    max_orders_per_second   = 10;      // rate limit
    };

    enum class RejectReason {
        OK,
        NOTIONAL_TOO_LARGE,
        POSITION_LIMIT_EXCEEDED,
        DAILY_LOSS_LIMIT,
        PRICE_SANITY_FAIL,
        RATE_LIMIT,
        KILL_SWITCH_ACTIVE
    };

    // Called in the hot path — must complete in < 1µs.
    // Takes the order by const ref: does not modify anything.
    RejectReason check(const Order& order, const MarketState& market) const {
        if (kill_switch_active_.load(std::memory_order_acquire)) {
            return RejectReason::KILL_SWITCH_ACTIVE;
        }

        double notional = order.price * order.quantity;
        if (__builtin_expect(notional > limits_.max_order_notional_usd, 0)) {
            return RejectReason::NOTIONAL_TOO_LARGE;
        }

        double new_position = current_position_usd_ +
            (order.side == Side::BUY ? notional : -notional);
        if (__builtin_expect(std::abs(new_position) > limits_.max_position_usd, 0)) {
            return RejectReason::POSITION_LIMIT_EXCEEDED;
        }

        if (__builtin_expect(realized_pnl_ < limits_.max_daily_loss_usd, 0)) {
            return RejectReason::DAILY_LOSS_LIMIT;
        }

        // Price sanity: reject orders more than 2% from last known mid.
        // This catches fat-finger errors and stale price references.
        double deviation = std::abs(order.price - market.last_mid) / market.last_mid;
        if (__builtin_expect(deviation > limits_.max_price_deviation_pct / 100.0, 0)) {
            return RejectReason::PRICE_SANITY_FAIL;
        }

        return RejectReason::OK;
    }

    // Update position and PnL — called from execution thread on fill
    void record_fill(double notional, double fee, bool is_buy) {
        if (is_buy) current_position_usd_ += notional;
        else        current_position_usd_ -= notional;
        realized_pnl_ -= fee;
    }

private:
    Limits limits_;
    std::atomic<bool> kill_switch_active_{false};
    double current_position_usd_ = 0.0;
    double realized_pnl_         = 0.0;
};
```

Notice the `__builtin_expect(condition, 0)` annotations on all the rejection checks. This tells the compiler that rejection is the *unlikely* branch, so it should lay out the code to keep the happy path (the `OK` return) in the CPU's instruction cache. Since the vast majority of orders should pass risk checks, this is the correct hint — it keeps the fast path as tight as possible.

### 5.2 Circuit Breakers

Circuit breakers are the risk layer's "automatic fuse." They monitor aggregate behavior over time and trip the kill switch when patterns emerge that suggest something is systematically wrong — not just a single bad order, but a bad situation:

```cpp
class CircuitBreaker {
public:
    // Called on every market data update — must be extremely fast.
    void on_tick(const Tick& tick, const PnLSnapshot& pnl) {
        // Check 1: Absolute PnL drawdown from session high
        session_high_pnl_ = std::max(session_high_pnl_, pnl.net_pnl);
        double drawdown = pnl.net_pnl - session_high_pnl_;
        if (drawdown < -max_drawdown_usd_) {
            trip("Drawdown limit exceeded: " + std::to_string(drawdown));
            return;
        }

        // Check 2: Order error rate (rejects + exchange errors per second)
        auto now = now_ns_monotonic();
        if (now - error_window_start_ns_ > 1'000'000'000LL) {
            error_count_this_second_ = 0;
            error_window_start_ns_   = now;
        }
        if (error_count_this_second_ > max_errors_per_second_) {
            trip("Error rate limit exceeded");
            return;
        }

        // Check 3: Exchange RTT spike (possibly indicates connectivity issues)
        if (last_rtt_ms_ > max_rtt_ms_) {
            trip("Exchange RTT spike: " + std::to_string(last_rtt_ms_) + "ms");
            return;
        }
    }

    void record_order_error() {
        error_count_this_second_++;
    }

    void update_rtt(double rtt_ms) {
        last_rtt_ms_ = rtt_ms;
    }

private:
    void trip(const std::string& reason) {
        // seq_cst: all threads must see this update immediately
        g_kill_switch_active.store(true, std::memory_order_seq_cst);
        logger_->log(Logger::Level::AUDIT, "circuit_breaker_tripped",
                     {{"reason", reason}, {"ts_ns", now_ns_realtime()}});
    }

    double   max_drawdown_usd_      = -500.0;
    int      max_errors_per_second_ = 10;
    double   max_rtt_ms_            = 200.0;
    double   session_high_pnl_      = 0.0;
    double   last_rtt_ms_           = 0.0;
    int      error_count_this_second_ = 0;
    int64_t  error_window_start_ns_   = 0;
};
```

---

## Part 6 — Strategy Layer Architecture

### 6.1 The Strategy Interface

Your strategy layer should be defined by a clean abstract interface that the execution engine depends on, rather than a concrete implementation. This lets you swap strategies, run multiple simultaneously, and test them against the same harness without modifying any engine code:

```cpp
// strategy/strategy_base.hpp
class StrategyBase {
public:
    virtual ~StrategyBase() = default;

    // Called on every market data update (tick, order book change, trade).
    // This is the hot path — must complete in microseconds.
    // Returns a vector of order intentions (possibly empty).
    virtual std::vector<OrderIntent> on_tick(const Tick& tick,
                                             const BookSnapshot& book) = 0;

    // Called when one of our orders is filled (partially or fully).
    // Use this to update internal position tracking, PnL attribution,
    // and any state that depends on actual fills rather than intended orders.
    virtual void on_fill(const Fill& fill) = 0;

    // Called when an order is rejected or cancelled.
    // The strategy should decide whether to retry or move on.
    virtual void on_order_update(const OrderUpdate& update) = 0;

    // Called periodically (e.g., every 100ms) for time-based logic.
    // Use for position aging, stale order cleanup, session statistics.
    virtual void on_timer() {}

    // Strategy metadata — used for logging, live gate display, and audit trail.
    virtual std::string name()        const = 0;
    virtual std::string version()     const = 0;
    virtual std::string description() const = 0;
};
```

`OrderIntent` is a lightweight struct expressing what the strategy *wants* to do — it is *not* an `Order` yet. The execution engine decides whether to translate an intent into an actual order submission, applying rate limits, deduplication, and risk checks first. This separation prevents strategy code from ever directly interacting with the exchange:

```cpp
struct OrderIntent {
    std::string symbol;
    Side        side;
    double      price;
    double      quantity;
    OrderType   type;           // LIMIT, MARKET, IOC
    uint64_t    strategy_signal_id; // for traceability
};
```

### 6.2 Configuration Management and Hot-Reload

Strategy parameters — thresholds, lookback windows, position limits — change frequently during development. Recompiling and restarting the engine every time you tweak a parameter is slow and interrupts any running shadow session. A configuration hot-reload system lets you change parameters in a config file and have them applied to a running engine within milliseconds.

The safe way to do this is to make your config a `std::atomic<std::shared_ptr<Config>>`. The config reader thread writes a new `shared_ptr`, and the strategy thread atomically swaps it:

```cpp
// strategy/config_manager.hpp
struct StrategyConfig {
    double  entry_threshold  = 0.0015; // 0.15% price move
    double  exit_threshold   = 0.0008;
    int     lookback_ticks   = 20;
    double  max_position_usd = 2000.0;
    // ... other parameters
};

class ConfigManager {
public:
    // Get current config — called in hot path, must be lock-free.
    std::shared_ptr<const StrategyConfig> get() const {
        return std::atomic_load_explicit(&config_, std::memory_order_acquire);
    }

    // Reload from disk — called from file-watcher thread, NOT hot path.
    void reload(const std::filesystem::path& config_path) {
        auto new_config = parse_toml(config_path);
        std::atomic_store_explicit(&config_,
            std::make_shared<StrategyConfig>(new_config),
            std::memory_order_release);
        logger_->log(Logger::Level::INFO, "config_reloaded",
                     {{"path", config_path.string()}});
    }

private:
    std::shared_ptr<const StrategyConfig> config_{
        std::make_shared<StrategyConfig>()
    };
};
```

To watch for file changes on Linux, use `inotify`:

```cpp
void watch_config_file(const std::string& path, ConfigManager& mgr) {
    int ifd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    inotify_add_watch(ifd, path.c_str(), IN_CLOSE_WRITE | IN_MOVED_TO);
    // Register ifd with the epoll reactor — reload happens asynchronously
    // when the editor saves the file, with zero overhead during normal operation.
    reactor_.register_fd(ifd, [&]() { mgr.reload(path); });
}
```

---

## Part 7 — TimescaleDB Database Layer

### 7.1 Why TimescaleDB

TimescaleDB is PostgreSQL with a time-series extension. It automatically partitions your tables into "chunks" by time interval (e.g., one chunk per day), which means queries over a time range touch only the relevant chunks rather than scanning the entire table. Inserts are fast because they always go to the current chunk (which is hot in the buffer pool), and old data can be compressed or tiered to cheaper storage automatically.

For an HFT engine, TimescaleDB gives you a queryable, relational record of everything that happened — every tick, every order, every fill, every PnL snapshot — that you can analyze with standard SQL and join with your deep learning feature tables.

### 7.2 Schema Design

```sql
-- schema.sql
-- Enable TimescaleDB
CREATE EXTENSION IF NOT EXISTS timescaledb;

-- Raw tick data: every trade that happens on Bitstamp
CREATE TABLE ticks (
    ts          TIMESTAMPTZ NOT NULL,  -- exchange timestamp
    symbol      TEXT        NOT NULL,
    price       DOUBLE PRECISION NOT NULL,
    quantity    DOUBLE PRECISION NOT NULL,
    side        CHAR(1) NOT NULL,      -- 'B' buy, 'S' sell
    exchange_id BIGINT               -- Bitstamp trade ID for deduplication
);

-- Convert to hypertable: chunk by 1 day.
-- Adjust chunk_time_interval based on your ingestion rate.
-- With ~1000 ticks/second, 1 day ≈ 86M rows/day — still manageable.
SELECT create_hypertable('ticks', 'ts', chunk_time_interval => INTERVAL '1 day');

-- Index on symbol for per-symbol queries (e.g., all BTC/USD ticks)
CREATE INDEX ON ticks (symbol, ts DESC);

-- Orders: one row per order, updated in place as state changes
CREATE TABLE orders (
    order_id        TEXT PRIMARY KEY,
    created_at      TIMESTAMPTZ NOT NULL,
    symbol          TEXT NOT NULL,
    side            CHAR(1) NOT NULL,
    price           DOUBLE PRECISION,
    quantity        DOUBLE PRECISION NOT NULL,
    state           TEXT NOT NULL,
    strategy_name   TEXT NOT NULL,
    strategy_signal BIGINT,            -- links to the strategy signal that spawned it
    exchange_id     TEXT,              -- Bitstamp's order ID (set on acknowledgment)
    filled_qty      DOUBLE PRECISION DEFAULT 0,
    avg_fill_price  DOUBLE PRECISION,
    total_fees      DOUBLE PRECISION DEFAULT 0,
    updated_at      TIMESTAMPTZ NOT NULL
);

-- Fills: one row per fill event (an order can generate multiple fills)
CREATE TABLE fills (
    ts              TIMESTAMPTZ NOT NULL,
    fill_id         TEXT NOT NULL,
    order_id        TEXT REFERENCES orders(order_id),
    symbol          TEXT NOT NULL,
    side            CHAR(1) NOT NULL,
    fill_price      DOUBLE PRECISION NOT NULL,
    fill_qty        DOUBLE PRECISION NOT NULL,
    fee             DOUBLE PRECISION NOT NULL,
    fee_currency    TEXT NOT NULL
);
SELECT create_hypertable('fills', 'ts', chunk_time_interval => INTERVAL '7 days');

-- PnL snapshots: written every N seconds for dashboard charting
CREATE TABLE pnl_snapshots (
    ts              TIMESTAMPTZ NOT NULL,
    strategy_name   TEXT NOT NULL,
    realized_pnl    DOUBLE PRECISION NOT NULL,
    unrealized_pnl  DOUBLE PRECISION NOT NULL,
    total_fees      DOUBLE PRECISION NOT NULL,
    net_pnl         DOUBLE PRECISION NOT NULL,
    gross_position  DOUBLE PRECISION NOT NULL
);
SELECT create_hypertable('pnl_snapshots', 'ts',
                         chunk_time_interval => INTERVAL '1 day');

-- System events: kill switch activations, reconnections, mode changes.
-- These are infrequent but critical for post-mortem analysis.
CREATE TABLE system_events (
    ts          TIMESTAMPTZ NOT NULL,
    level       TEXT NOT NULL,   -- INFO, WARN, ERROR, AUDIT
    event_name  TEXT NOT NULL,
    details     JSONB            -- flexible key-value payload
);
SELECT create_hypertable('system_events', 'ts',
                         chunk_time_interval => INTERVAL '30 days');

-- Continuous aggregate: real-time 1-minute OHLCV bars.
-- TimescaleDB materializes this automatically as new ticks arrive.
CREATE MATERIALIZED VIEW ohlcv_1min
WITH (timescaledb.continuous) AS
SELECT
    time_bucket('1 minute', ts) AS bucket,
    symbol,
    FIRST(price, ts)  AS open,
    MAX(price)        AS high,
    MIN(price)        AS low,
    LAST(price, ts)   AS close,
    SUM(quantity)     AS volume
FROM ticks
GROUP BY bucket, symbol;

-- Refresh policy: update the aggregate every 10 seconds.
SELECT add_continuous_aggregate_policy('ohlcv_1min',
    start_offset => INTERVAL '1 hour',
    end_offset   => INTERVAL '10 seconds',
    schedule_interval => INTERVAL '10 seconds');

-- Data retention: compress chunks older than 7 days.
-- After compression, TimescaleDB reports 10–20x space savings on tick data.
SELECT add_compression_policy('ticks', INTERVAL '7 days');

-- Drop raw ticks older than 90 days (keep compressed OHLCV forever).
SELECT add_retention_policy('ticks', INTERVAL '90 days');
```

### 7.3 The Async Write Path

This is the most important architectural decision in the database layer: **the trading hot path must never block on a database write**. Database writes involve network round-trips (even to localhost), buffer flushing, and WAL writes — all of which have unpredictable latency. If your execution thread blocks waiting for a DB write to complete, you've introduced unbounded latency jitter directly into your order submission path.

The solution is a dedicated database writer thread fed by a lock-free queue. The trading thread enqueues a write record in nanoseconds and moves on. The writer thread dequeues records and batches them into `COPY` protocol bulk inserts:

```cpp
// db/db_writer.cpp
// DB write record: a tagged union of everything we might want to persist.
struct WriteRecord {
    enum class Type { TICK, ORDER_UPDATE, FILL, PNL_SNAPSHOT, SYSTEM_EVENT };
    Type type;
    union {
        Tick          tick;
        OrderUpdate   order_update;
        Fill          fill;
        PnLSnapshot   pnl_snapshot;
        SystemEvent   system_event;
    };
};

// Queue between trading threads (producers) and DB writer thread (consumer).
// 65536 slots: at 10k writes/sec, this gives ~6.5 seconds of buffering
// before backpressure. If the DB is consistently this slow, something
// is seriously wrong with your TimescaleDB configuration.
using DBWriteQueue = SPSCRingBuffer<WriteRecord, 65536>;

class AsyncDBWriter {
public:
    void start(const std::string& connstring) {
        conn_ = PQconnectdb(connstring.c_str());
        writer_thread_ = std::thread([this]() { writer_loop(); });
    }

    // Called from trading threads — enqueues in nanoseconds, never blocks.
    void enqueue_tick(const Tick& tick) {
        WriteRecord rec;
        rec.type = WriteRecord::Type::TICK;
        rec.tick = tick;
        queue_.push(rec); // lock-free
    }

private:
    void writer_loop() {
        // Use COPY protocol for bulk inserts: much faster than
        // individual INSERT statements, especially for tick data.
        // Batch up to 1000 records before flushing.
        std::vector<WriteRecord> batch;
        batch.reserve(1000);

        while (running_.load()) {
            WriteRecord rec;
            while (queue_.pop(rec)) {
                batch.push_back(rec);
                if (batch.size() >= 1000) flush_batch(batch);
            }
            if (!batch.empty()) flush_batch(batch);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    void flush_batch(std::vector<WriteRecord>& batch) {
        // Open a COPY stream for ticks
        PQexec(conn_, "COPY ticks (ts, symbol, price, quantity, side) FROM STDIN");

        for (auto& rec : batch) {
            if (rec.type != WriteRecord::Type::TICK) continue;
            // Format: tab-separated, newline terminated
            char line[256];
            snprintf(line, sizeof(line),
                     "%ld\t%s\t%.8f\t%.8f\t%c\n",
                     rec.tick.ts_ns,
                     rec.tick.symbol,
                     rec.tick.price,
                     rec.tick.quantity,
                     rec.tick.side == Side::BUY ? 'B' : 'S');
            PQputCopyData(conn_, line, strlen(line));
        }

        PQputCopyEnd(conn_, nullptr); // finalize COPY
        PQgetResult(conn_);           // consume result to avoid protocol desync
        batch.clear();
    }

    DBWriteQueue  queue_;
    PGconn*       conn_;
    std::thread   writer_thread_;
    std::atomic<bool> running_{true};
};
```

---

## Part 8 — Structured Logging System

### 8.1 The Binary Ring Buffer Logger

The same principle that applies to database writes applies to logging: **the hot path must never block on a log write**. Writing to disk, even to an SSD, takes 50–200µs. Writing to a socket (for centralized logging) can take milliseconds. If your critical execution thread calls `fprintf` or `spdlog::info()` in a tight loop, you are paying that cost on every message.

The solution is a two-tier logging architecture. The hot path writes into a binary ring buffer in RAM — this costs only a handful of memory writes and an atomic counter increment, taking roughly 50–100ns. A separate background thread drains the ring buffer to disk asynchronously, formatting records as NDJSON as it goes:

```cpp
// logging/logger.hpp
struct LogRecord {
    int64_t     ts_ns;          // nanosecond timestamp
    uint8_t     level;          // 0=DEBUG, 1=INFO, 2=WARN, 3=ERROR, 4=AUDIT
    uint8_t     thread_id;      // identifies which thread logged this
    uint16_t    event_len;      // length of event string
    uint32_t    payload_len;    // length of JSON payload
    char        event[64];      // fixed-size event name (no heap)
    char        payload[192];   // fixed-size JSON payload
    // Total: 264 bytes. Fits in 5 cache lines — acceptable for a log record.
};

class RingBufferLogger {
public:
    // HOT PATH: enqueue a log record.
    // Average cost: ~80ns (cache-warm), ~300ns (cache-cold).
    void log(uint8_t level, const char* event,
             const char* payload, uint16_t payload_len) {

        // AUDIT-level records bypass the ring buffer and write synchronously.
        // These are rare (kill switch, live mode activation) so the overhead
        // is acceptable, and the guarantee that they're not lost is essential.
        if (__builtin_expect(level == LEVEL_AUDIT, 0)) {
            write_sync(level, event, payload, payload_len);
            return;
        }

        auto idx = write_head_.fetch_add(1, std::memory_order_relaxed);
        auto& rec = buffer_[idx & mask_];

        rec.ts_ns       = now_ns_realtime();
        rec.level       = level;
        rec.thread_id   = get_thread_id();
        strncpy(rec.event, event, sizeof(rec.event) - 1);
        memcpy(rec.payload, payload, std::min(payload_len,
               (uint16_t)sizeof(rec.payload)));
        rec.payload_len = payload_len;

        // Signal the drain thread that a new record is available.
        // std::memory_order_release: ensure all the field writes above
        // are visible before the drain thread reads them.
        commit_head_.store(idx + 1, std::memory_order_release);
    }

private:
    static constexpr std::size_t BUFFER_SIZE = 65536; // must be power of 2
    static constexpr std::size_t mask_       = BUFFER_SIZE - 1;

    std::array<LogRecord, BUFFER_SIZE> buffer_;
    alignas(64) std::atomic<uint64_t>  write_head_{0};
    alignas(64) std::atomic<uint64_t>  commit_head_{0};
    alignas(64) uint64_t               drain_tail_{0};
};
```

The drain thread formats each binary record into NDJSON and writes it to a rotating log file:

```cpp
void drain_thread_loop(RingBufferLogger& logger, std::FILE* log_file) {
    while (true) {
        uint64_t commit = logger.commit_head_.load(std::memory_order_acquire);

        while (logger.drain_tail_ < commit) {
            const auto& rec = logger.buffer_[logger.drain_tail_ & logger.mask_];

            // Format as NDJSON — one JSON object per line.
            // Each line is independently parseable: `jq .` on the log file works.
            fprintf(log_file,
                "{\"ts_ns\":%ld,\"level\":\"%s\",\"event\":\"%s\","
                "\"data\":%.*s}\n",
                rec.ts_ns,
                level_string(rec.level),
                rec.event,
                rec.payload_len, rec.payload);

            ++logger.drain_tail_;
        }

        fflush(log_file);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}
```

### 8.2 Correlation IDs

A correlation ID is a single unique integer that follows a trading event from the moment the strategy generates a signal all the way through to the final database write. With correlation IDs, you can take any fill and reconstruct *exactly* what the strategy was thinking when it placed that order: what tick triggered it, what the risk check saw, what the execution thread sent, and what latency each step took.

```cpp
// Generate a unique correlation ID for each strategy signal.
// Uses a simple atomic counter — fast and guaranteed unique within a session.
uint64_t next_correlation_id() {
    static std::atomic<uint64_t> counter{0};
    return counter.fetch_add(1, std::memory_order_relaxed);
}

// Every log call from strategy → execution → fill → DB write
// includes the same correlation_id in its payload.
// Querying all log lines with a given correlation_id gives you
// a complete trace of one trade lifecycle.
```

---

## Part 9 — Performance Engineering

### 9.1 CPU Isolation and the isolcpus Boot Parameter

All the `sched_setaffinity` calls in the world won't fully isolate your trading core if the kernel's load balancer keeps migrating other work there. The definitive solution is the `isolcpus` kernel boot parameter, which removes specified CPUs from the kernel scheduler entirely — they can only run a task if explicitly assigned to them:

```bash
# In /etc/default/grub, add to GRUB_CMDLINE_LINUX_DEFAULT:
# isolcpus=2,3 — remove CPUs 2 and 3 from the scheduler
# nohz_full=2,3 — disable timer interrupts on these CPUs (reduces jitter)
# rcu_nocbs=2,3 — offload RCU callbacks (reduces latency spikes)
# irqaffinity=0,1 — route all IRQs to CPUs 0 and 1 only

GRUB_CMDLINE_LINUX_DEFAULT="isolcpus=2,3 nohz_full=2,3 rcu_nocbs=2,3 irqaffinity=0,1"
```

After this, CPUs 2 and 3 are completely quiet until your trading engine assigns threads to them. You can verify this with `cat /proc/interrupts | grep CPU2` — interrupt counts should be nearly zero.

### 9.2 CPU Frequency Scaling

Modern CPUs dynamically adjust their clock frequency based on load (Intel SpeedStep, AMD Cool'n'Quiet). This is great for power consumption but terrible for latency consistency — a CPU that was idling might take 10–50µs to ramp up to full speed when your engine suddenly needs to process a burst of ticks.

```bash
# Set the performance governor for ALL CPUs.
# This keeps them at maximum clock frequency permanently.
# On a dedicated trading server, power consumption is not a concern.
for cpu in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
    echo performance > $cpu
done

# Or more permanently via cpupower:
cpupower frequency-set -g performance
```

### 9.3 Measuring Latency Correctly

Measuring latency in an HFT context requires precision that `gettimeofday()` simply doesn't offer. The correct approach is to use `rdtsc` to bracket the operation being measured, convert the TSC delta to nanoseconds using the CPU's known frequency, and maintain a histogram of observed latencies:

```cpp
class LatencyHistogram {
public:
    // Record a latency observation in nanoseconds.
    void record(uint64_t latency_ns) {
        // Find the appropriate bucket using __builtin_clz (count leading zeros).
        // This gives log2(latency_ns), mapping to a power-of-2 bucket.
        // Buckets: [0-1ns], [1-2ns], [2-4ns], ... [1ms+]
        int bucket = (latency_ns == 0) ? 0 :
                     63 - __builtin_clzll(latency_ns);
        bucket = std::min(bucket, 63);
        buckets_[bucket].fetch_add(1, std::memory_order_relaxed);
        count_.fetch_add(1, std::memory_order_relaxed);
    }

    // Print percentile statistics.
    void report() const {
        uint64_t total = count_.load();
        uint64_t p50_target = total * 50 / 100;
        uint64_t p99_target = total * 99 / 100;
        uint64_t p999_target = total * 999 / 1000;

        uint64_t running = 0;
        for (int i = 0; i < 64; ++i) {
            running += buckets_[i].load();
            if (running >= p50_target)
                printf("p50: ~%lu ns\n", 1ULL << i);
            // ... etc
        }
    }

private:
    std::array<std::atomic<uint64_t>, 64> buckets_{};
    std::atomic<uint64_t> count_{0};
};

// Usage: measure the cost of the risk check
auto t0 = rdtsc();
auto result = risk_manager_.check(order, market_state);
auto t1 = rdtsc();
latency_histogram_.record((t1 - t0) * ns_per_tick_); // ns_per_tick_ = 1e9 / cpu_freq_hz
```

### 9.4 Flamegraph Profiling

When you need to find where CPU time is being spent, `perf` is the right tool. It samples the call stack at regular intervals without requiring instrumentation:

```bash
# Record CPU samples at 10,000 Hz for 30 seconds of shadow trading
perf record -F 10000 -p $(pgrep engine_shadow) -g -- sleep 30

# Convert to flamegraph format
perf script | ./stackcollapse-perf.pl > out.folded
./flamegraph.pl out.folded > flamegraph.svg
```

The resulting SVG is an interactive flame graph where the width of each frame represents its percentage of CPU time. Functions that appear unexpectedly wide (e.g., `memcpy`, `malloc`, `pthread_mutex_lock`) in your hot path are immediate optimization targets.

---

## Part 10 — Post-Trade Deep Learning Pipeline (C/C++ Only)

### 10.1 Why Post-Trade, Not Intra-Trade

At the 1–5ms RTT tier, your latency floor is dominated by network round-trip time to Bitstamp. Running a neural network inference in that critical path would add 10–50µs of additional latency — potentially doubling your total execution time. The right architecture is therefore *post-trade*: the model trains on historical fills and generates updated strategy parameters periodically (e.g., every hour or at session end), which are then atomically fed back into the live strategy. The model never runs in the hot path.

This is also intellectually honest: at this tier, your alpha comes from strategy logic, execution quality, and risk management — not from microsecond model predictions. The DL pipeline is most valuable for *learning from your own behavior* — identifying which market conditions your strategy performs well in, when it loses, and how to tune parameters accordingly.

### 10.2 LibTorch: PyTorch's C++ API

LibTorch is the official C++ frontend for PyTorch. It provides the full training and inference API — tensors, autograd, optimizers, model serialization — without requiring Python. You can define models in C++, train them in C++, save them as TorchScript, and load them back in C++ for inference.

```bash
# Download LibTorch (CPU version — GPU is optional but useful for training)
wget https://download.pytorch.org/libtorch/cpu/libtorch-cxx11-abi-shared-with-deps-2.1.0%2Bcpu.zip
```

```cmake
# CMakeLists.txt
find_package(Torch REQUIRED)
target_link_libraries(engine_ml ${TORCH_LIBRARIES})
target_include_directories(engine_ml PRIVATE ${TORCH_INCLUDE_DIRS})
```

### 10.3 Feature Engineering in C++

The first step in the pipeline is extracting features from your fills and tick data. Features should capture market microstructure — the patterns that precede profitable and unprofitable trades:

```cpp
// ml/feature_extractor.cpp
struct TradeFeatureVector {
    // Price action features
    float returns_1m;        // 1-minute price return before the trade
    float returns_5m;        // 5-minute price return
    float returns_15m;       // 15-minute price return
    float volatility_1m;     // realized volatility (std dev of 1m returns)
    float volatility_5m;

    // Microstructure features
    float bid_ask_spread;    // spread at time of order placement
    float order_book_imbalance; // (bid_qty - ask_qty) / (bid_qty + ask_qty)
    float trade_imbalance_1m;   // buy_volume / total_volume over 1 min
    float vwap_deviation;       // price vs. 5-min VWAP

    // Our own execution features
    float fill_latency_ms;      // time from signal to fill
    float slippage_bps;         // fill price vs. signal price, in basis points
    float queue_position;       // estimated position in order book queue

    // Time features
    float hour_of_day;          // sin-encoded to preserve cyclical nature
    float day_of_week;          // sin-encoded

    // Target variable (for training)
    float pnl_next_5m;          // PnL over the 5 minutes after this trade
};

class FeatureExtractor {
public:
    // Extract features for a given fill by querying TimescaleDB.
    // This runs in the post-trade pipeline — not the hot path.
    TradeFeatureVector extract(const Fill& fill, PGconn* db) {
        TradeFeatureVector fv{};

        // Query rolling statistics from TimescaleDB using the
        // continuous aggregates and window functions.
        const char* query = R"(
            SELECT
                -- 1-minute return
                (LAST(price, ts) - FIRST(price, ts)) / FIRST(price, ts)
                    AS return_1m,
                -- 1-minute realized volatility
                STDDEV(price) AS volatility_1m,
                -- 5-minute VWAP deviation
                ($1::float - SUM(price * quantity) / SUM(quantity))
                    / (SUM(price * quantity) / SUM(quantity)) AS vwap_dev_5m
            FROM ticks
            WHERE symbol = $2
              AND ts BETWEEN $3::timestamptz - INTERVAL '5 minutes'
                         AND $3::timestamptz
        )";

        // Use parameterized queries — NEVER string-interpolate into SQL.
        const char* params[3] = {
            std::to_string(fill.fill_price).c_str(),
            fill.symbol,
            format_timestamptz(fill.ts_ns).c_str()
        };

        PGresult* res = PQexecParams(db, query, 3, nullptr,
                                     params, nullptr, nullptr, 0);

        if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0) {
            fv.returns_1m    = atof(PQgetvalue(res, 0, 0));
            fv.volatility_1m = atof(PQgetvalue(res, 0, 1));
            fv.vwap_deviation = atof(PQgetvalue(res, 0, 2));
        }
        PQclear(res);

        // Time encoding: use sin/cos to make hour-of-day cyclical.
        // Without this, hour 23 and hour 0 would appear very different
        // to the model even though they're adjacent in time.
        auto t  = fill.ts_ns / 1'000'000'000LL;
        struct tm* tm_info = gmtime(&t);
        float hour = tm_info->tm_hour + tm_info->tm_min / 60.0f;
        fv.hour_of_day = sinf(2.0f * M_PI * hour / 24.0f);

        return fv;
    }
};
```

### 10.4 Model Architecture: Temporal Convolutional Network

For time-series prediction on trading data, a Temporal Convolutional Network (TCN) outperforms LSTMs in most benchmarks: it's faster to train (fully parallelizable, no sequential dependency), easier to regularize, and provides better gradient flow. A TCN applies dilated causal convolutions — each layer "looks back" further in time without increasing parameters:

```cpp
// ml/model.cpp
#include <torch/torch.h>

// A single residual block in the TCN.
// dilation controls how far back this layer looks:
// dilation=1 → adjacent timesteps
// dilation=2 → every other timestep (2x range)
// dilation=4 → every 4th timestep (4x range)
struct TCNBlockImpl : torch::nn::Module {
    TCNBlockImpl(int in_channels, int out_channels, int kernel_size,
                 int dilation, float dropout_rate) {
        // Dilated causal convolution: padding = (kernel_size-1) * dilation
        // ensures the output length equals the input length (causal: no future leakage)
        int padding = (kernel_size - 1) * dilation;
        conv1 = register_module("conv1",
            torch::nn::Conv1d(torch::nn::Conv1dOptions(in_channels, out_channels, kernel_size)
                .dilation(dilation).padding(padding)));
        conv2 = register_module("conv2",
            torch::nn::Conv1d(torch::nn::Conv1dOptions(out_channels, out_channels, kernel_size)
                .dilation(dilation).padding(padding)));
        dropout = register_module("dropout",
            torch::nn::Dropout(dropout_rate));
        // Residual connection: 1x1 conv if channel sizes differ
        if (in_channels != out_channels) {
            residual_proj = register_module("residual_proj",
                torch::nn::Conv1d(torch::nn::Conv1dOptions(in_channels, out_channels, 1)));
        }
    }

    torch::Tensor forward(torch::Tensor x) {
        auto residual = x;
        // Apply conv → ReLU → dropout, twice
        x = torch::relu(conv1->forward(x));
        x = dropout->forward(x);
        x = torch::relu(conv2->forward(x));
        x = dropout->forward(x);

        // Trim the non-causal padding from the right
        // (causal padding adds to the left, but Conv1d may also pad right)
        x = x.slice(2, 0, residual.size(2));

        // Add residual connection
        if (residual_proj) residual = residual_proj->forward(residual);
        return torch::relu(x + residual);
    }

    torch::nn::Conv1d conv1{nullptr}, conv2{nullptr};
    torch::nn::Conv1d residual_proj{nullptr};
    torch::nn::Dropout dropout{nullptr};
};
TORCH_MODULE(TCNBlock);

// Full TCN: stacked blocks with exponentially increasing dilation.
// With kernel_size=3 and 6 blocks, the receptive field is:
// sum(2 * (kernel_size-1) * 2^i for i in 0..5) = 2*(3-1)*(1+2+4+8+16+32) = 252 timesteps
struct TradePredictorImpl : torch::nn::Module {
    TradePredictorImpl(int num_features, int num_channels, int num_blocks) {
        // Input projection: map feature vector to channel space
        input_proj = register_module("input_proj",
            torch::nn::Linear(num_features, num_channels));

        // Stack TCN blocks with doubling dilation
        for (int i = 0; i < num_blocks; ++i) {
            blocks->push_back(TCNBlock(num_channels, num_channels, 3, 1 << i, 0.2));
        }
        register_module("blocks", blocks);

        // Output head: predict 5-minute PnL
        output_head = register_module("output_head",
            torch::nn::Linear(num_channels, 1));
    }

    torch::Tensor forward(torch::Tensor x) {
        // x shape: [batch, seq_len, num_features]
        x = torch::relu(input_proj->forward(x));
        // Transpose to [batch, channels, seq_len] for Conv1d
        x = x.transpose(1, 2);
        for (auto& block : *blocks) {
            x = block->forward(x);
        }
        // Take the last timestep's representation
        x = x.select(2, -1);
        return output_head->forward(x);
    }

    torch::nn::Linear input_proj{nullptr};
    torch::nn::ModuleList blocks;
    torch::nn::Linear output_head{nullptr};
};
TORCH_MODULE(TradePredictor);
```

### 10.5 Walk-Forward Validation

The most important thing to understand about backtesting and model validation in trading is that standard cross-validation is *wrong*. If you randomly split your data into train and test sets, you will include future information in your training data (data leakage), producing optimistic results that don't hold in live trading.

Walk-forward validation is the correct approach: train on data from period T0 to T1, validate on T1 to T2 only, then advance the window. Repeat. This mimics exactly what will happen in production:

```cpp
// ml/walk_forward_validator.cpp
struct WalkForwardResult {
    double sharpe_ratio;
    double max_drawdown;
    double total_return;
    int    num_windows;
};

WalkForwardResult walk_forward_validate(
    TradePredictor& model,
    const std::vector<TradeFeatureVector>& all_data,
    int train_size,
    int test_size)
{
    std::vector<double> window_returns;
    int start = 0;

    while (start + train_size + test_size <= (int)all_data.size()) {
        // Training window: [start, start + train_size)
        auto train_begin = all_data.begin() + start;
        auto train_end   = train_begin + train_size;

        // Validation window: [start + train_size, start + train_size + test_size)
        auto test_begin  = train_end;
        auto test_end    = test_begin + test_size;

        // Train model on training window
        train_model(model, {train_begin, train_end});

        // Evaluate on test window — NO TRAINING DATA LEAKS IN
        double window_return = evaluate_model(model, {test_begin, test_end});
        window_returns.push_back(window_return);

        // Advance window by test_size (non-overlapping test windows)
        start += test_size;
    }

    return compute_statistics(window_returns);
}
```

### 10.6 Feeding Model Signals Back Without Restart

Once the model generates updated parameters (e.g., "use a tighter entry threshold in current market conditions"), they need to reach the live strategy atomically — without restarting the engine, without a race condition, and without any possibility of the strategy seeing a half-updated parameter set.

The `atomic<shared_ptr>` pattern introduced in the ConfigManager section handles this perfectly:

```cpp
// ml/parameter_bridge.hpp
// The model writes updated parameters here.
// The strategy reads from here on every tick.
struct ModelDerivedParams {
    float entry_threshold_multiplier; // scale the base threshold
    float position_size_multiplier;   // scale the default position size
    float confidence_score;           // model's confidence in current regime
    int64_t generated_at_ns;          // timestamp — strategy can reject stale params
};

class ModelParameterBridge {
public:
    // Called by the DL pipeline thread (post-trade, not hot path)
    void update(const ModelDerivedParams& new_params) {
        auto ptr = std::make_shared<ModelDerivedParams>(new_params);
        std::atomic_store_explicit(&params_, ptr, std::memory_order_release);
    }

    // Called by strategy thread on every tick — lock-free, ~10ns
    std::shared_ptr<const ModelDerivedParams> get() const {
        return std::atomic_load_explicit(&params_, std::memory_order_acquire);
    }

private:
    std::shared_ptr<const ModelDerivedParams> params_{
        std::make_shared<ModelDerivedParams>()
    };
};
```

---

## Part 11 — Monitoring and Observability

### 11.1 Prometheus Metrics in C++

Prometheus is the standard for metrics collection in production systems. Your engine should expose a `/metrics` HTTP endpoint that Prometheus scrapes every 15 seconds. This requires embedding a minimal HTTP server — using `libmicrohttpd` or a hand-rolled one — that serves the current metric values in Prometheus text format:

```cpp
// monitoring/metrics.cpp
// Key metrics to track — each a different instrument type.

// Histograms: distributions of observed values (latencies, sizes)
prometheus::Histogram order_latency_ms{
    "order_latency_ms",
    "Time from strategy signal to exchange acknowledgment",
    {0.1, 0.5, 1.0, 2.0, 5.0, 10.0, 50.0, 100.0} // bucket boundaries in ms
};

// Gauges: current values that go up and down
prometheus::Gauge pnl_usd{
    "pnl_usd",
    "Current net PnL in USD"
};

prometheus::Gauge open_orders_count{
    "open_orders_count",
    "Number of orders currently live at the exchange"
};

prometheus::Gauge ws_reconnect_count{
    "ws_reconnect_count",
    "Number of WebSocket reconnections since start"
};

// Counters: monotonically increasing (total fills, total orders, etc.)
prometheus::Counter fills_total{
    "fills_total",
    "Total number of order fills since engine start"
};

prometheus::Counter rejected_orders_total{
    "rejected_orders_total",
    "Total orders rejected by risk manager"
};
```

### 11.2 Grafana Dashboard Layout

A useful Grafana dashboard for this engine has three rows. The top row shows the health indicators: net PnL over the session (line chart), open order count (gauge), and WebSocket connection status (state timeline). The middle row shows execution quality: order latency histogram (heatmap), fill rate per minute (bar chart), and slippage in basis points (scatter plot). The bottom row shows system health: DB write queue depth, log buffer utilization, and circuit breaker state.

The most important alerting rules are a PnL drawdown alert (notify when drawdown exceeds 50% of the session maximum), a latency spike alert (notify when p99 order latency exceeds 10ms), and a connectivity alert (notify when no WebSocket messages have been received for 10 seconds).

---

## Part 12 — Operational Concerns

### 12.1 systemd Service Unit

Running the engine under systemd gives you automatic restart on crash, resource limits, and proper log capture:

```ini
# /etc/systemd/system/hft-engine.service
[Unit]
Description=HFT Trading Engine
After=network-online.target postgresql.service
Wants=network-online.target

[Service]
Type=simple
User=trading
Group=trading
ExecStart=/opt/hft_engine/bin/engine_live --config /opt/hft_engine/config/live.toml
ExecStop=/bin/kill -SIGTERM $MAINPID

# Restart automatically on crash, but not if we exit cleanly (shutdown)
Restart=on-failure
RestartSec=5s

# Resource limits: give the engine exclusive access to its memory
LimitMEMLOCK=infinity
LimitRTPRIO=99
LimitNICE=-20

# Kill signal: SIGTERM first, giving us time to cancel orders and flush logs.
# After 30 seconds, escalate to SIGKILL.
KillSignal=SIGTERM
TimeoutStopSec=30

# Log to journal with engine identifier
StandardOutput=journal
StandardError=journal
SyslogIdentifier=hft-engine

[Install]
WantedBy=multi-user.target
```

### 12.2 Graceful Shutdown Sequence

The ordered shutdown sequence — described earlier but worth codifying as a concrete function — is the last line of defense against leaving live orders at the exchange:

```cpp
void graceful_shutdown(BitstampClient& client,
                       OrderManager& order_mgr,
                       AsyncDBWriter& db_writer,
                       RingBufferLogger& logger) {

    logger.log(AUDIT, "shutdown_initiated", "{}");

    // Step 1: Stop strategy from generating new orders
    g_shutdown_requested.store(true, std::memory_order_seq_cst);

    // Step 2: Wait for in-flight order send to complete (max 500ms)
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (order_mgr.has_pending_sends() &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // Step 3: Cancel all live orders at the exchange
    bool cancelled = client.cancel_all_orders();
    logger.log(AUDIT, "cancel_all_orders",
               cancelled ? "{\"result\":\"ok\"}" : "{\"result\":\"FAILED\"}");

    // Step 4: Flush the DB write queue (all fills must be persisted)
    db_writer.flush_and_stop();

    // Step 5: Drain and flush the log ring buffer
    logger.flush_sync();

    // Step 6: Exit
    logger.log(AUDIT, "shutdown_complete", "{}");
}
```

### 12.3 Development Tooling

During development, always build with AddressSanitizer and UndefinedBehaviorSanitizer enabled. These add roughly 2x runtime overhead but catch memory bugs (use-after-free, buffer overflows, stack smashing) and undefined behavior instantly rather than letting them manifest as mysterious crashes in production:

```bash
# Development build with sanitizers
cmake -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
      -B build_debug

# Valgrind for more thorough memory checking during backtest runs
valgrind --tool=memcheck --leak-check=full --show-leak-kinds=all \
    ./engine_backtest --config config/backtest.toml
```

Never profile with sanitizers enabled — they change the memory layout and timing characteristics significantly. Profile with a release build, debug with sanitizers.

---

## Conclusion: The Philosophy Behind the Architecture

Every decision in this document flows from two principles. The first is *never let the hot path touch anything that can block* — no heap, no disk, no locks, no syscalls that aren't vDSO-optimized. The second is *trust, but verify* — every component that can fail (exchange connections, database writes, model parameters, strategy configurations) should fail loudly and immediately, not silently corrupt state that only manifests hours later.

The performance target of 1–5ms RTT is entirely achievable with this architecture. The network round-trip to Bitstamp will dominate that budget, and your software overhead — with pinned threads, pre-allocated memory, lock-free queues, and vDSO clock calls — should contribute well under 100µs. The remaining work is operational: profiling with real market data, tuning TimescaleDB chunk sizes to your actual ingestion rate, calibrating circuit breaker thresholds to your strategy's normal behavior, and iterating on the DL pipeline as your fill history grows large enough to train meaningfully.

The engine described here is not theoretical — every component has a clear implementation path in standard C++20 with well-maintained open-source dependencies. Build it layer by layer, test each layer in isolation, and treat the integration as the final exam rather than the first step.
