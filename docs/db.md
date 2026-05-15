# QuestDB Integration — Implementation Plan

> **Audience:** Claude Code executing this plan step-by-step.
> **Status:** Authoritative spec. Execute steps in order. Do not skip the
> verification gate at the end of each step.
> **Scope:** Land QuestDB alongside the existing SQLite persistence. A later
> follow-up PR (out of scope here) removes SQLite + PostgreSQL. This PR must
> leave SQLite working.

---

## 0. Summary

Add QuestDB as a new opt-in persistence backend that captures **every order
lifecycle event** (submissions, status transitions, fills, rejections,
cancellations, amendments) for replay, analysis, and cross-run comparison.

**USDT-M futures note**: The same tables and schema are used for both spot
and futures runs. Futures activity appears with signed quantities, futures-
specific order flags (`reduce_only`, `close_position`), and mark-price
context in the raw event stream. All example queries in this document work
for futures runs.

**Key decisions (locked in):**

- **Compile-time gate:** `ENABLE_QUESTDB` CMake option, default **OFF**,
  defines `HAS_QUESTDB`. When off, no QuestDB code compiles and no runtime
  flag exists. When on, the flag is accepted but persistence still requires
  an explicit runtime opt-in.
- **Runtime gate:** `--persist` CLI flag, default **off**. Must be passed
  explicitly every session the user wants to write to the DB.
- **Transport:** hand-rolled raw sockets (POSIX) — zero external deps.
  ILP on TCP/9009 for ingest; HTTP/1.1 on TCP/9000 for DDL + health check.
  No Boost, no libpq, no libpqxx.
- **Per-run table prefixes.** Each session creates 6 tables named
  `{run_tag}_orders`, `{run_tag}_order_status`, `{run_tag}_fills`,
  `{run_tag}_rejections`, `{run_tag}_cancellations`,
  `{run_tag}_amendments`. One permanent table `runs_meta` indexes all runs.
- **Soft warning** when QuestDB daemon is unreachable. Engine continues
  with persistence disabled for the session. (Hard-fail is a future TODO.)
- **Worker thread model.** A dedicated `QuestDbWorker` drains a new event
  ring, identical in shape to the existing `LoggingWorker`.
- **Works in all three binaries:** `engine_backtest`, `engine_shadow`,
  `engine_live`. No mode-specific behaviour.

**Reminder:** we just removed the Web UI. The new `questdb_ring_` /
`questdb_worker_` members in the engine should mirror the structural
pattern that `ws_ring_` / `ws_worker_` followed — but now lives behind
`#ifdef HAS_QUESTDB`.

---

## 1. Architecture overview

```
 engine hot path
     │
     │ publish_event(ev)
     ▼
 ┌─────────────┐   try_push                ┌──────────────────┐
 │ event_ring  │◄──────────────────────────│ publish_event()  │
 │ (per worker)│                           └──────────────────┘
 └─────────────┘
     │ (existing: logging_ring, risk_ring, stats_ring, …)
     │
     │ NEW:
     ▼
 ┌──────────────┐   try_push              event producers on the
 │ questdb_ring │◄───────────────────────── event-loop thread
 └──────────────┘   (only if config_.persist_enabled)
     │
     │ consumed by QuestDbWorker on its own core
     ▼
 ┌──────────────────────┐
 │ QuestDbWorker        │   drains ring, forwards each event to the store
 │   holds store_       │
 └──────────────────────┘
     │
     ▼
 ┌──────────────────────┐    ┌───────────────────────┐
 │ QuestdbStore         │    │ HTTP client (DDL)     │───► :9000 /exec
 │   classifies events  │    └───────────────────────┘
 │   calls ILP writer   │    ┌───────────────────────┐
 └──────────────────────┘    │ ILP writer (batched)  │───► :9009 (ILP)
                             └───────────────────────┘
```

---

## 2. Directory layout (to be created)

```
src/
├── data/
│   └── questdb/                         # NEW — all QuestDB code lives here
│       ├── tcp_client.h/.cpp            # raw POSIX socket connect/read/write
│       ├── http_client.h/.cpp           # HTTP/1.1 POST /exec, URL encoding
│       ├── ilp_writer.h/.cpp            # line protocol + batching + reconnect
│       ├── schema.h/.cpp                # CREATE TABLE IF NOT EXISTS DDL
│       ├── run_tag.h/.cpp               # unique run_tag generator
│       └── store.h/.cpp                 # QuestdbStore facade
└── engine/
    └── questdb_worker.h                 # NEW — drains questdb_ring

tests/
├── test_questdb_http_client.cpp         # URL encoding, response parsing
├── test_questdb_ilp_writer.cpp          # line format, batch behaviour
├── test_questdb_schema.cpp              # DDL string generation
├── test_questdb_run_tag.cpp             # uniqueness, format
├── test_questdb_store.cpp               # store dispatches to mock transports
├── test_questdb_worker.cpp              # worker drain semantics
└── test_questdb_integration.cpp         # gated on QUESTDB_TEST_HOST env var
```

---

## Step 1 — CMake flag + header scaffold

**Goal:** Add `ENABLE_QUESTDB` build option with no source files yet. Verify
both `OFF` and `ON` builds succeed. This step touches no runtime code.

### Files

- `CMakeLists.txt` — add the option
- `cmake/Dependencies.cmake` — add the `ENABLE_QUESTDB` branch in
  `tt_wire_optional_backends`
- `docs/licenses.md` — note that `ENABLE_QUESTDB` introduces no new
  third-party deps (hand-rolled, so the table does not change)

### Work

1. In `CMakeLists.txt`, after the existing `option(ENABLE_BINANCE …)` line,
   add:
   ```cmake
   option(ENABLE_QUESTDB    "Build with QuestDB persistence backend"              OFF)
   ```

2. In `cmake/Dependencies.cmake`, inside `tt_wire_optional_backends()`,
   add a new branch after the SQLite branch:
   ```cmake
   # QuestDB persistence (raw sockets, zero external deps)
   if(ENABLE_QUESTDB)
       # No find_package / FetchContent — we use POSIX sockets directly.
       # Source files get added in Step 2 onward. For now just define the macro.
       target_compile_definitions(${target} PUBLIC HAS_QUESTDB)
   endif()
   ```

### Tests / Verification

```bash
# Default OFF: must still build and pass tests
rm -rf build
cmake -B build -DBUILD_TESTS=ON
cmake --build build -j$(nproc)
ctest --test-dir build -j$(nproc)

# With flag ON: must still configure and build (no new sources yet, so
# nothing actually compiles into engine_core, just the #define is set)
rm -rf build
cmake -B build -DBUILD_TESTS=ON -DENABLE_QUESTDB=ON
cmake --build build -j$(nproc)
ctest --test-dir build -j$(nproc)
```

### Done when

- Both builds configure without warnings.
- Both builds compile all targets.
- Test pass count is unchanged from before this step (same N-of-M).

---

## Step 2 — Raw TCP + HTTP client

**Goal:** Implement a minimal POSIX-socket TCP client and a thin HTTP/1.1
POST wrapper. These are the only I/O primitives the rest of the
implementation builds on. Unit-testable via an injected mock.

### Files

- `src/data/questdb/tcp_client.h/.cpp`
- `src/data/questdb/http_client.h/.cpp`
- `tests/test_questdb_http_client.cpp`

### Work

**`tcp_client.h` — concrete class:**

```cpp
#pragma once
#ifdef HAS_QUESTDB

#include <cstddef>
#include <string>
#include <string_view>

namespace truetest::questdb {

// Non-blocking-friendly POSIX TCP client. Thin wrapper around socket(2) +
// connect(2) + send(2) + recv(2). Reused by both the HTTP client (DDL,
// short one-shot requests) and the ILP writer (persistent connection,
// batched line writes). Blocking I/O is fine — both use cases run off the
// hot path on the worker thread.
class TcpClient
{
public:
    TcpClient() = default;
    ~TcpClient();
    TcpClient(const TcpClient&) = delete;
    TcpClient& operator=(const TcpClient&) = delete;

    // Returns false on failure. `connect_timeout_ms` applies via
    // non-blocking connect + select/poll.
    bool connect(const std::string& host, std::uint16_t port,
                 int connect_timeout_ms = 2000);

    bool write_all(std::string_view data);

    // Reads until `\r\n\r\n` (header terminator) or up to `max_bytes`.
    // Returns the accumulated bytes (may include body after header).
    std::string read_until_header_end(std::size_t max_bytes = 65536,
                                      int read_timeout_ms = 5000);

    // Read exactly `n` additional bytes. Used after header parsing to
    // consume Content-Length body.
    std::string read_n(std::size_t n, int read_timeout_ms = 5000);

    bool is_connected() const { return fd_ >= 0; }
    void close();

private:
    int fd_ = -1;
};

} // namespace truetest::questdb

#endif // HAS_QUESTDB
```

`.cpp` uses `<sys/socket.h>`, `<netdb.h>`, `<unistd.h>`, `<fcntl.h>`,
`<sys/select.h>`. `getaddrinfo` for hostname resolution. Set
`TCP_NODELAY` after connect (ILP throughput).

**`http_client.h` — thin wrapper:**

```cpp
#pragma once
#ifdef HAS_QUESTDB

#include <optional>
#include <string>
#include <string_view>

namespace truetest::questdb {

struct HttpResponse
{
    int status = 0;          // 200, 400, 503, …
    std::string body;        // JSON response from QuestDB
    std::string raw_headers; // for debugging
};

// Percent-encode a SQL string for inclusion in a query string.
// Only the characters unsafe for URL query values are escaped.
std::string url_encode(std::string_view s);

// Issue GET /exec?query=<url_encoded(sql)> against the QuestDB HTTP
// endpoint. Blocking. Returns std::nullopt if the TCP connect or any I/O
// fails. Status 200 with non-empty body on success.
std::optional<HttpResponse> query_exec(const std::string& host,
                                       std::uint16_t port,
                                       const std::string& sql,
                                       int timeout_ms = 5000);

// Cheap health probe. Issues `GET /exec?query=SELECT%201` and returns
// true iff the response status is 200.
bool ping(const std::string& host, std::uint16_t port,
          int timeout_ms = 2000);

} // namespace truetest::questdb

#endif // HAS_QUESTDB
```

**Response parsing rules (`.cpp`):**

- Status line: split on first space → "HTTP/1.1 200 OK" → parse 200 as int.
- Find `\r\nContent-Length: N\r\n` header (case-insensitive).
- After `\r\n\r\n`, read `N` more bytes.
- If `Content-Length` is missing, fall back to reading until connection
  close. QuestDB's `/exec` always sets it; log a warning if absent.

**`test_questdb_http_client.cpp`:**

Tests that need no network:

1. `url_encode_basic` — space → `%20`, `=` → `%3D`, `&` → `%26`, plus
   unreserved chars pass through unchanged.
2. `url_encode_sql_create_table` — encode a real `CREATE TABLE` string,
   assert a golden output.
3. `url_encode_empty` — empty input returns empty.
4. `url_encode_non_ascii` — `ü` → `%C3%BC` (UTF-8 per byte).

(No parsing tests yet — the parser is tested live in Step 4.)

Add the test file to `CMakeLists.txt`:
```cmake
tests/test_questdb_http_client.cpp
```

Add the sources to `tt_wire_optional_backends`:
```cmake
if(ENABLE_QUESTDB)
    target_sources(${target} PRIVATE
        ${CMAKE_SOURCE_DIR}/src/data/questdb/tcp_client.cpp
        ${CMAKE_SOURCE_DIR}/src/data/questdb/http_client.cpp)
    target_compile_definitions(${target} PUBLIC HAS_QUESTDB)
endif()
```

### Tests / Verification

```bash
rm -rf build
cmake -B build -DBUILD_TESTS=ON -DENABLE_QUESTDB=ON
cmake --build build -j$(nproc) 2>&1 | tail -20
ctest --test-dir build -R "questdb_http_client" --output-on-failure

# Also verify OFF build still clean
rm -rf build
cmake -B build -DBUILD_TESTS=ON
cmake --build build -j$(nproc)
ctest --test-dir build -j$(nproc)
```

### Done when

- `test_questdb_http_client` compiles and all assertions pass.
- `ENABLE_QUESTDB=ON` build has no warnings in the new files.
- `ENABLE_QUESTDB=OFF` build unchanged.

---

## Step 3 — ILP writer + batching + reconnect

**Goal:** Implement the InfluxDB Line Protocol writer. Holds a persistent
TCP socket, batches lines, flushes on threshold or explicit call, reconnects
on drop.

### Files

- `src/data/questdb/ilp_writer.h/.cpp`
- `tests/test_questdb_ilp_writer.cpp`

### Work

**`ilp_writer.h`:**

```cpp
#pragma once
#ifdef HAS_QUESTDB

#include "tcp_client.h"
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace truetest::questdb {

// ILP line builder. One instance per logical row. Use the `add_tag` /
// `add_field` calls in order, then `finish(timestamp_ns)` to produce the
// final string ending in '\n'.
//
// Line protocol reference:
//   table_name,tag1=v1,tag2=v2 field1=x,field2="y" 1234567890000000000\n
// Rules:
//   - tags are dictionary-encoded (SYMBOL in QuestDB)
//   - field string values must be quoted
//   - field numeric values bare
//   - commas and spaces in tags/fields MUST be escaped with '\'
//   - double-quotes in string values escaped with '\'
class LineBuilder
{
public:
    explicit LineBuilder(std::string_view table);
    LineBuilder& add_tag(std::string_view key, std::string_view value);
    LineBuilder& add_field_str(std::string_view key, std::string_view value);
    LineBuilder& add_field_double(std::string_view key, double value);
    LineBuilder& add_field_long(std::string_view key, std::int64_t value);
    LineBuilder& add_field_bool(std::string_view key, bool value);
    // Emits "... <ns>\n". timestamp_ns is wall-clock nanoseconds.
    std::string finish(std::int64_t timestamp_ns);

private:
    std::string line_;
    bool in_fields_ = false;
    void escape_into(std::string_view s, std::string& out, bool is_tag);
};

// Persistent ILP connection with write-buffering.
class IlpWriter
{
public:
    IlpWriter(std::string host, std::uint16_t port,
              std::size_t flush_every_n_lines = 1000,
              std::chrono::milliseconds flush_every = std::chrono::milliseconds(50));
    ~IlpWriter();

    // Attempt initial connect. Returns false on failure; caller decides
    // whether to retry, warn, or give up.
    bool connect();

    // Append a pre-built line (must end with '\n'). Flushes the buffer if
    // the threshold is reached. Safe to call after a connection drop —
    // the buffer accumulates and a reconnect is attempted on the next
    // flush.
    void enqueue(std::string line);

    // Force-flush. Returns false if the socket write failed; the buffer
    // is retained for the next attempt.
    bool flush();

    std::size_t pending_lines() const { return buffer_count_; }
    std::size_t dropped_lines() const { return dropped_; }

private:
    std::string host_;
    std::uint16_t port_;
    std::size_t flush_every_n_lines_;
    std::chrono::milliseconds flush_every_;
    TcpClient tcp_;
    std::string buffer_;
    std::size_t buffer_count_ = 0;
    std::size_t dropped_ = 0;
    std::chrono::steady_clock::time_point last_flush_;
    int consecutive_failures_ = 0;

    bool reconnect_with_backoff();
};

} // namespace truetest::questdb

#endif // HAS_QUESTDB
```

**Escape rules (`LineBuilder::escape_into`):**

| Context     | Escape these chars | Prepend with |
|-------------|---------------------|--------------|
| table name  | space, comma        | `\`          |
| tag key/val | space, comma, `=`   | `\`          |
| field key   | space, comma, `=`   | `\`          |
| field value (string) | `"`, `\`   | `\`          |

**Reconnect behaviour:** On flush failure, close the socket, increment
`consecutive_failures_`, and try to reconnect with exponential backoff
capped at 30s (`min(1000ms * 2^n, 30000ms)`). **Do not** sleep inside
`flush()` — the worker thread's loop handles the cadence. `flush()` just
returns false, and the caller decides whether to retry now or later.

**`test_questdb_ilp_writer.cpp`:**

1. `LineBuilder_plain` — build `t,a=1 b=2 1000` and assert exact output.
2. `LineBuilder_escape_tag_comma` — tag value `"a,b"` → `a\,b`.
3. `LineBuilder_escape_tag_space` — tag value `"a b"` → `a\ b`.
4. `LineBuilder_escape_string_quote` — string `"a\"b"` → `"a\\\"b"`.
5. `LineBuilder_mixed_field_types` — long, double, string, bool.
6. `LineBuilder_double_precision` — `1.234567890123` round-trips to at
   least 12 significant digits (use `%.17g`).
7. `LineBuilder_timestamp_ns` — trailing `\n` and correct ns value.

Writer tests use a **FakeTransport** injection — add a virtual-function
seam so the writer accepts a mock TCP sink in tests. Keep the production
path using `TcpClient` directly for zero overhead.

Suggested seam: extract a `class IIlpTransport` interface with
`connect()` / `write_all()` / `close()`, make `TcpClient` implement it,
and let `IlpWriter` hold `std::unique_ptr<IIlpTransport>`. The real
constructor news up a `TcpClient`; tests construct with a mock.

8. `IlpWriter_buffers_until_threshold` — enqueue < threshold, assert
   mock received 0 writes. Enqueue exactly threshold, assert 1 write
   with the concatenated payload.
9. `IlpWriter_explicit_flush` — enqueue 3, call `flush()`, assert write.
10. `IlpWriter_flush_failure_retains_buffer` — mock's `write_all` returns
    false, assert `pending_lines()` still reflects buffered lines.
11. `IlpWriter_reconnect_after_failure` — mock fails once, succeeds next;
    next `flush()` succeeds and drains.

### Tests / Verification

```bash
rm -rf build
cmake -B build -DBUILD_TESTS=ON -DENABLE_QUESTDB=ON
cmake --build build -j$(nproc)
ctest --test-dir build -R "questdb_ilp_writer" --output-on-failure
```

### Done when

- All 11 tests pass.
- `IlpWriter` compiles clean with `-Wall -Wextra`.

---

## Step 4 — Schema + run_tag

**Goal:** Hard-coded DDL strings for the 7 tables, and a run_tag generator.
No I/O in this step — just string production + a unit test.

### Files

- `src/data/questdb/schema.h/.cpp`
- `src/data/questdb/run_tag.h/.cpp`
- `tests/test_questdb_schema.cpp`
- `tests/test_questdb_run_tag.cpp`

### Work

**`schema.h`:**

```cpp
#pragma once
#ifdef HAS_QUESTDB

#include <string>
#include <vector>

namespace truetest::questdb::schema {

// DDL for the shared runs_meta table. Idempotent (IF NOT EXISTS).
std::string runs_meta_ddl();

// Six per-run DDL statements, prefixed with `run_tag`.
std::vector<std::string> per_run_ddls(const std::string& run_tag);

// Convenience — all 7 statements in the order they should be issued.
std::vector<std::string> all_ddls(const std::string& run_tag);

// Pretty names of the 6 per-run tables (without prefix), useful for
// programmatic access.
inline constexpr const char* kTableOrders        = "orders";
inline constexpr const char* kTableOrderStatus   = "order_status";
inline constexpr const char* kTableFills         = "fills";
inline constexpr const char* kTableRejections    = "rejections";
inline constexpr const char* kTableCancellations = "cancellations";
inline constexpr const char* kTableAmendments    = "amendments";

} // namespace truetest::questdb::schema

#endif // HAS_QUESTDB
```

**`schema.cpp`** — the DDL strings are verbatim from Appendix A below.
Use raw string literals. Each DDL is a single `CREATE TABLE IF NOT EXISTS
...` statement (no trailing semicolon — QuestDB HTTP does not want it).

**`run_tag.h`:**

```cpp
#pragma once
#ifdef HAS_QUESTDB

#include <string>

namespace truetest::questdb {

// Generates a fresh run_tag if `user_override` is empty. Format:
//   run_<YYYYMMDD>_<HHMMSS>_<6_char_hex>
// The hex suffix is random (or derived from an injected seed in tests).
// If `user_override` is non-empty, returns it unchanged after
// validation (only [A-Za-z0-9_] permitted, max 64 chars). Throws
// std::invalid_argument on bad override.
std::string make_run_tag(const std::string& user_override,
                         std::uint64_t test_seed = 0);

// True iff `tag` is a valid QuestDB table-name prefix.
bool is_valid_run_tag(const std::string& tag);

} // namespace truetest::questdb

#endif // HAS_QUESTDB
```

**`test_questdb_schema.cpp`:**

1. `runs_meta_has_started_at_as_designated_timestamp` — string contains
   `TIMESTAMP(started_at) PARTITION BY MONTH`.
2. `per_run_ddls_count` — returns exactly 6.
3. `per_run_ddls_prefix` — every string starts with
   `CREATE TABLE IF NOT EXISTS myrun_` when called with `"myrun"`.
4. `per_run_ddls_tables` — the 6 suffixes match the 6 `kTable*` constants.
5. `orders_ddl_columns_match_appendix_a` — contains `order_id LONG`,
   `opener_order_id LONG`, `symbol SYMBOL`, `side SYMBOL`,
   `strategy_name SYMBOL`, `initial_status SYMBOL`.
6. `fills_ddl_has_source_column` — contains `source SYMBOL CAPACITY 8`.
7. `all_tables_partition_by_day` — every per-run DDL contains
   `PARTITION BY DAY`.
8. `all_ddls_starts_with_runs_meta` — first element is `runs_meta_ddl()`.

**`test_questdb_run_tag.cpp`:**

1. `generated_tag_matches_expected_format` — regex
   `^run_\d{8}_\d{6}_[0-9a-f]{6}$`.
2. `override_passes_through` — `"experiment_v3"` → `"experiment_v3"`.
3. `invalid_override_throws` — `"bad tag!"` throws.
4. `empty_override_with_same_seed_is_deterministic` — two calls with
   `test_seed=42` and same wall-clock → same result.
5. `two_unseeded_calls_differ` — calls 1 ms apart produce different tags.

Add both test files and the two `.cpp` source files to CMake.

### Tests / Verification

```bash
cmake --build build -j$(nproc)
ctest --test-dir build -R "questdb_schema|questdb_run_tag" --output-on-failure
```

### Done when

- All schema + run_tag tests pass.
- No warnings.

---

## Step 5 — QuestdbStore facade

**Goal:** Single entry point for engine callers. Takes `order_event`,
`fill_event`, and friends, classifies them, and emits ILP lines through
the writer. Owns the HTTP client for DDL bootstrapping.

### Files

- `src/data/questdb/store.h/.cpp`
- `tests/test_questdb_store.cpp`

### Work

**`store.h`:**

```cpp
#pragma once
#ifdef HAS_QUESTDB

#include "core/event.h"
#include "ilp_writer.h"
#include <chrono>
#include <memory>
#include <string>

namespace truetest::questdb {

struct StoreConfig
{
    std::string host = "127.0.0.1";
    std::uint16_t ilp_port = 9009;
    std::uint16_t http_port = 9000;
    std::string run_tag;
    // Metadata written to runs_meta on begin/end.
    std::string mode;       // backtest | shadow | live
    std::string binary;     // engine_backtest | engine_shadow | engine_live
    std::string strategy;
    std::string symbol;
    double initial_equity = 0.0;
    std::string params_json;
};

class QuestdbStore
{
public:
    explicit QuestdbStore(StoreConfig cfg);
    ~QuestdbStore();

    // Bootstrap: issues the 7 CREATE TABLE IF NOT EXISTS DDLs via HTTP,
    // opens the persistent ILP socket, inserts the initial runs_meta row.
    // Returns false on any step failure; caller should warn + disable
    // persistence for the session.
    bool begin();

    // Finalise: flush pending ILP lines, UPDATE runs_meta.ended_at +
    // final equity + counters. Idempotent.
    void end(double final_equity,
             std::size_t total_orders,
             std::size_t total_fills,
             std::size_t total_rejections);

    // One function per capture point. Each takes the event + the
    // opener_order_id / strategy_name as determined by the engine (using
    // the same order_meta_ lookup the lot table uses).
    void record_order_submitted(const order_event& o,
                                const std::string& initial_status);
    void record_status_transition(std::uint64_t order_id,
                                  order_status old_s,
                                  order_status new_s,
                                  const std::string& reason = {});
    void record_fill(const fill_event& f,
                     std::uint64_t opener_order_id,
                     const std::string& strategy_name,
                     const std::string& source);
    void record_rejection(const order_event& o,
                          const std::string& reason_category,
                          const std::string& reason_detail);
    void record_cancellation(std::uint64_t order_id,
                             const std::string& symbol,
                             const std::string& strategy_name,
                             const std::string& reason);
    void record_amendment(std::uint64_t order_id,
                          const std::string& symbol,
                          double old_price, double new_price,
                          double old_qty, double new_qty,
                          std::chrono::system_clock::time_point ts);

    // For the worker's periodic tick — does not force-flush if below
    // threshold but respects the time-based flush interval.
    void tick();

    // Force-flush pending ILP lines.
    void flush();

private:
    StoreConfig cfg_;
    std::unique_ptr<IlpWriter> ilp_;
    std::chrono::system_clock::time_point started_at_;

    std::string table_name(const char* suffix) const;
    std::int64_t ns_from(std::chrono::system_clock::time_point ts) const;
};

} // namespace truetest::questdb

#endif // HAS_QUESTDB
```

**Implementation notes:**

- `begin()`:
  1. For each DDL in `schema::all_ddls(cfg_.run_tag)`, call
     `http_client::query_exec(cfg_.host, cfg_.http_port, ddl)`. If any
     returns nullopt or non-200 status, log and return `false`.
  2. Construct `IlpWriter(cfg_.host, cfg_.ilp_port)` and call `connect()`.
     If it fails, return `false`.
  3. Issue one ILP line to `runs_meta` with `started_at=now`, the static
     fields (mode, binary, strategy, symbol, initial_equity, params).
  4. Store `started_at_` for later.

- `end()`: flush ILP, then issue an HTTP `UPDATE runs_meta SET ...
  WHERE run_tag = '<tag>' AND started_at = <started_at_ns>`. (QuestDB
  supports UPDATE on non-designated-timestamp tables, but `runs_meta` has
  `started_at` as designated — so we instead INSERT a second row with the
  same `run_tag` and `ended_at` populated. Call this an "update row" and
  let consumers aggregate via `GROUP BY run_tag` + `LAST(ended_at)`.)

  **Why two rows, not UPDATE:** QuestDB's UPDATE support on partitioned
  tables is limited. Inserting a second row is simpler and matches the
  append-only ingest model.

- All `record_*` calls: build a `LineBuilder` for the appropriate table,
  stamp `run_tag` as a SYMBOL column on every row, add fields, call
  `ilp_->enqueue(line.finish(ns_now))`.

- `ns_from()` handles the `system_clock::time_point → int64 ns`
  conversion. Use `duration_cast<nanoseconds>(ts.time_since_epoch()).count()`.

**`test_questdb_store.cpp`:**

Inject a mock `IIlpTransport` through the writer (same seam as Step 3)
and a mock HTTP function (expose an overridable `std::function<bool(sql)>`
in the store for DDL issuance in tests — or expose a protected virtual
`http_exec()` method and subclass it in tests).

1. `begin_issues_7_ddls_in_order` — mock records every SQL string; assert
   first is `runs_meta` DDL, then `{prefix}_orders`, etc.
2. `begin_aborts_if_any_ddl_fails` — 4th DDL returns false; `begin()`
   returns false; no ILP line enqueued.
3. `begin_writes_runs_meta_row` — after successful DDL, the ILP mock
   received exactly one line starting with `runs_meta,`.
4. `record_order_submitted_produces_orders_row` — line starts with
   `{run_tag}_orders,` and contains expected tags/fields.
5. `record_status_transition_produces_order_status_row`
6. `record_fill_produces_fills_row_with_opener`
7. `record_rejection_produces_rejections_row_with_reason`
8. `record_cancellation_produces_cancellations_row`
9. `record_amendment_produces_amendments_row`
10. `end_writes_second_runs_meta_row` — after `end()`, another `runs_meta,`
    line appears carrying `ended_at`, `final_equity`, counters.
11. `all_rows_tagged_with_run_tag` — every ILP line has
    `run_tag=<cfg.run_tag>` exactly once.
12. `flush_calls_writer_flush` — store's `flush()` calls `ilp_->flush()`.

### Tests / Verification

```bash
cmake --build build -j$(nproc)
ctest --test-dir build -R "questdb_store" --output-on-failure
```

### Done when

- All 12 store tests pass.
- `test_questdb_ilp_writer` and earlier tests still pass.

---

## Step 6 — QuestDbWorker + ring integration

**Goal:** A dedicated worker thread that drains a new event ring and forwards
each event to the store. Mirrors the existing `LoggingWorker` pattern.

### Files

- `src/engine/questdb_worker.h`

### Work

**`questdb_worker.h`:**

```cpp
#pragma once
#ifdef HAS_QUESTDB

#include "../data/questdb/store.h"
#include "../threading/ring_buffer.h"
#include "../threading/worker.h"
#include "../core/event.h"
#include <memory>

using event_pointer = std::shared_ptr<event>;
using EventRing = RingBuffer<event_pointer, 65536>;

class QuestDbWorker : public Worker
{
public:
    explicit QuestDbWorker(std::shared_ptr<truetest::questdb::QuestdbStore> store);

    void run(EventRing& ring);
    void stop();

private:
    std::shared_ptr<truetest::questdb::QuestdbStore> store_;
    std::atomic<bool> stop_flag_{false};
};

#endif // HAS_QUESTDB
```

Inline the implementation in the header (follows existing `logging_worker.h`
pattern). `run()`:

1. Loop while `!stop_flag_`.
2. `try_pop` from ring; if empty, honour spin policy (use the same
   `apply_spin_policy()` helper other workers use).
3. For each event: pattern-match on event type (order / fill / rejection
   / cancel / amend / status_change) and call the corresponding
   `store_->record_*()`. Skip event types the store doesn't care about
   (market/tick/L2).
4. After every batch of drained events, call `store_->tick()` to honour
   the time-based flush cadence.
5. On `stop()`: set flag, exit loop, `store_->flush()`.

**Event type dispatch:** events in the engine are `std::shared_ptr<event>`
with a polymorphic `get_type()`. For each captured type you'll need a
`dynamic_pointer_cast` — follow the pattern in `logging_worker.h`.

**Note about status transitions:** The engine emits order_event,
fill_event, rejection_event, cancel_event, amend_event. Status
transitions are tracked by `OrderTracker` but aren't separate events on
the ring today. For this step, **skip the order_status table in the worker**
— the store's `record_status_transition()` will be called directly from
the engine's hot path (Step 7) at the spots where `OrderTracker::set_status`
fires, not through the ring. Document this limitation: QuestDB
`order_status` writes are synchronous. Volume is low (≤ orders × few
transitions) so this is fine.

### Tests

Testing a thread is awkward. Instead:

- Construct a `MockStore` (subclass of `QuestdbStore` with virtual
  `record_*` methods overridden). Or make the worker hold a
  `std::shared_ptr<IStore>` interface and test at that level.
- Run the worker briefly on a thread with a ring pre-populated.
- Assert the mock received the expected events in the expected order.

File: `tests/test_questdb_worker.cpp`

1. `worker_drains_ring_until_stopped` — push 100 events, run worker,
    stop after 200ms, assert mock recorded 100.
2. `worker_flushes_on_stop` — mock counts flush() calls; assert exactly 1
    after `stop()`.
3. `worker_dispatches_by_event_type` — push 1 order, 1 fill, 1 rejection,
   1 cancel, 1 amend; assert each mock method called once.
4. `worker_ignores_uncaptured_event_types` — push market_event and
   tick_event; assert mock called zero times.

### Tests / Verification

```bash
cmake --build build -j$(nproc)
ctest --test-dir build -R "questdb_worker" --output-on-failure
```

### Done when

- All 4 worker tests pass.
- Existing `test_threading_correctness` still passes.

---

## Step 7 — Engine wiring (config + ring + capture points)

**Goal:** Plumb everything into the engine. Introduce `questdb_ring_`,
`questdb_worker_`, `questdb_store_` members. Hook `record_*` calls at every
lifecycle point. Nothing is activated yet without the `--persist` runtime
flag (Step 8).

### Files

- `src/engine/engine_config.h` — new fields
- `src/engine/engine.h` — new members + methods
- `src/engine/engine.cpp` — wire workers + capture points

### Work

**`engine_config.h`** — add after the existing `db_path` field, guarded:

```cpp
#ifdef HAS_QUESTDB
    bool persist_enabled = false;
    std::string questdb_host = "127.0.0.1";
    std::uint16_t questdb_ilp_port = 9009;
    std::uint16_t questdb_http_port = 9000;
    std::string run_tag;         // empty → auto-generate
    std::string run_notes;       // optional free-form, goes to runs_meta
#endif
```

**`engine.h`:**

```cpp
#ifdef HAS_QUESTDB
#include "../data/questdb/store.h"
#include "questdb_worker.h"
#endif
```

And inside the private section:

```cpp
#ifdef HAS_QUESTDB
    std::shared_ptr<truetest::questdb::QuestdbStore> questdb_store_;
    std::shared_ptr<EventRing> questdb_ring_;
    std::unique_ptr<QuestDbWorker> questdb_worker_;
    std::size_t questdb_drops_ = 0;
    bool questdb_active_ = false;  // true only after successful begin()

    void questdb_begin();
    void questdb_end();
#endif
```

**`engine.cpp`** — edits in many spots:

1. **`publish_event`** (around the top) — after the existing ring push
   block:
   ```cpp
   #ifdef HAS_QUESTDB
       if (questdb_ring_ && !questdb_ring_->try_push(ev))
       {
           questdb_drops_++;
           if (questdb_drops_ == 1 || questdb_drops_ % 1000 == 0)
               std::cerr << "  QuestDB ring: " << questdb_drops_
                         << " events dropped\n";
       }
   #endif
   ```

2. **`start_workers`** — after other workers are created, add the
   QuestDB worker block:
   ```cpp
   #ifdef HAS_QUESTDB
       if (config_.persist_enabled && questdb_active_)
       {
           questdb_ring_ = std::make_shared<EventRing>();
           questdb_worker_ = std::make_unique<QuestDbWorker>(questdb_store_);
           wire_failure(*questdb_worker_);
           worker_threads_.emplace_back([this]() {
               questdb_worker_->run(*questdb_ring_);
           });
       }
   #endif
   ```

3. **`stop_workers`** — add symmetrically:
   ```cpp
   #ifdef HAS_QUESTDB
       if (questdb_worker_) questdb_worker_->stop();
   #endif
   ```
   and include `questdb_worker_.get()` in the `all_workers[]` array.
   Include `questdb_drops_` in the `total_drops` tally. Add a
   `report_hwm("questdb", questdb_ring_)` line.

4. **`questdb_begin()` / `questdb_end()`** — new helper methods:
   ```cpp
   #ifdef HAS_QUESTDB
   void engine::questdb_begin()
   {
       if (!config_.persist_enabled) return;

       truetest::questdb::StoreConfig scfg;
       scfg.host = config_.questdb_host;
       scfg.ilp_port = config_.questdb_ilp_port;
       scfg.http_port = config_.questdb_http_port;
       scfg.run_tag = truetest::questdb::make_run_tag(config_.run_tag);
       scfg.mode = (config_.mode == engine_mode::backtest ? "backtest"
                  : config_.mode == engine_mode::shadow   ? "shadow"
                                                          : "live");
       scfg.binary = /* infer from TT_TARGET */;
       scfg.strategy = primary_strategy_name_;
       scfg.symbol = /* from data_handler */;
       scfg.initial_equity = config_.initial_balance;
       scfg.params_json = /* optional: serialize strategy params */;

       questdb_store_ = std::make_shared<truetest::questdb::QuestdbStore>(
           std::move(scfg));

       // Soft-fail per spec: if begin() fails, warn and continue without
       // persistence. Hard-fail is a future TODO.
       if (questdb_store_->begin())
       {
           questdb_active_ = true;
       }
       else
       {
           std::cerr << "  WARNING: QuestDB unreachable at "
                     << config_.questdb_host << ":" << config_.questdb_http_port
                     << " — continuing with persistence DISABLED for this session.\n"
                     << "  Start the daemon with: questdb start\n"
                     << "  Or re-run without --persist to suppress this warning.\n";
           questdb_store_.reset();
       }
   }

   void engine::questdb_end()
   {
       if (!questdb_active_ || !questdb_store_) return;
       const auto report = analytics_.snapshot();
       questdb_store_->end(portfolio_.get_equity(last_mid_price_),
                           report.total_orders, report.total_fills,
                           /* total_rejections */ 0 /* TODO plumb */);
       questdb_active_ = false;
   }
   #endif
   ```

5. **Capture points** — the engine has the following hot-path sites
   today (after the WebUI removal). Each needs a new synchronous call
   into `questdb_store_` when `questdb_active_` is true:

   | Site | Action |
   |---|---|
   | `process_order` — risk acceptance | `record_order_submitted(*o, "pending")`; `record_status_transition(id, pending, open)` |
   | `process_order` — risk rejection (top & inside execution loop) | `record_rejection(*o, category, detail)` + `record_order_submitted(*o, "rejected")` |
   | `route_order` — instrument-filter rejection | same as above |
   | Fill callback path | `record_fill(f, lookup_opener(f.order_id), lookup_strategy_name(f.order_id), source)` + `record_status_transition(id, open, filled-or-partial)` |
   | `cancel_order` | `record_cancellation(id, symbol, strategy, reason)` + `record_status_transition(id, open, cancelled)` |
   | `modify_order` success | `record_amendment(id, sym, old_px, new_px, old_qty, new_qty, now)` |
   | `unwind_positions` | each synthetic order → `record_cancellation` with reason `"risk_unwind"` |

   **Wrap every call** in:
   ```cpp
   #ifdef HAS_QUESTDB
       if (questdb_active_ && questdb_store_)
           questdb_store_->record_*(...);
   #endif
   ```

   Use `lookup_opener()` and `lookup_strategy_name()` which already exist
   from the per-lot refactor.

6. **Call `questdb_begin()` and `questdb_end()`** from each of the three
   `run_*` methods (`run`, `run_tick_data`, `run_streaming` × 3). `begin`
   before `start_workers`; `end` after `stop_workers`, before the
   final return.

### Tests

No new test file needed for this step — the store + worker are already
covered. For the hook points, add **one new integration test** that
asserts the engine's orchestration calls `questdb_store_->begin()` /
`end()` when `persist_enabled=true`, using a mock store injected via a
protected setter.

`tests/test_engine_questdb_wiring.cpp`:

1. `engine_does_not_touch_store_when_persist_disabled`
2. `engine_calls_begin_and_end_when_persist_enabled`
3. `engine_records_order_submission`
4. `engine_records_rejection_on_risk_halt`
5. `engine_records_fill_with_opener_and_strategy`

These tests run the engine in inline mode with CSV data and assert mock
store received expected calls.

### Tests / Verification

```bash
cmake --build build -j$(nproc)
ctest --test-dir build -j$(nproc) 2>&1 | tail -15
```

### Done when

- All new engine-wiring tests pass.
- Full suite still passes (minus the known pre-existing
  `DataHandler.HasBarData` failure).
- Build clean on both `ENABLE_QUESTDB=ON` and `OFF`.

---

## Step 8 — CLI flags + config file

**Goal:** Expose `--persist`, `--run-tag`, `--questdb-host`,
`--questdb-ilp-port`, `--questdb-http-port`, `--run-notes` on the CLI. Wire
them through JSON config file, CLI overrides, and dump-config.

### Files

- `src/bin/main.inc`

### Work

Follow the exact pattern the removed `--web-ui` flags used (this is why we
removed them cleanly in the previous pass — the surface to add to is
identical).

1. **`cli_options` struct** — add (guard with `#ifdef HAS_QUESTDB` if you
   want — or leave unguarded and just ignore the fields in non-QuestDB
   builds. Prefer **guarded** so the struct size only grows when needed):
   ```cpp
   #ifdef HAS_QUESTDB
       bool persist = false;
       std::string run_tag;
       std::string run_notes;
       std::string questdb_host = "127.0.0.1";
       std::uint16_t questdb_ilp_port = 9009;
       std::uint16_t questdb_http_port = 9000;
   #endif
   ```

2. **`load_config_file`** (the `get_*` block) — restore the `get_u16`
   lambda removed in the WebUI cleanup, then:
   ```cpp
   #ifdef HAS_QUESTDB
       get_bool("persist", o.persist);
       get_str("run_tag", o.run_tag);
       get_str("run_notes", o.run_notes);
       get_str("questdb_host", o.questdb_host);
       get_u16("questdb_ilp_port", o.questdb_ilp_port);
       get_u16("questdb_http_port", o.questdb_http_port);
   #endif
   ```

3. **JSON dump-config** — same symmetric additions to the `j[...] = ...`
   block.

4. **CLI registration** — inside the flags registration block, guarded:
   ```cpp
   #ifdef HAS_QUESTDB
       app.add_flag("--persist", o.persist,
           "Write every order lifecycle event to QuestDB for this session");
       app.add_option("--run-tag", o.run_tag,
           "Table prefix for this run (auto-generated if omitted)");
       app.add_option("--run-notes", o.run_notes,
           "Free-form note stored with the run metadata");
       app.add_option("--questdb-host", o.questdb_host,
           "QuestDB host (default 127.0.0.1)");
       app.add_option("--questdb-ilp-port", o.questdb_ilp_port,
           "QuestDB ILP ingest port (default 9009)");
       app.add_option("--questdb-http-port", o.questdb_http_port,
           "QuestDB HTTP/DDL port (default 9000)");
   #endif
   ```

5. **CLI → config-file override merge** — symmetric:
   ```cpp
   #ifdef HAS_QUESTDB
       if (!was_set("--persist")) opts.persist = file_opts.persist;
       if (!was_set("--run-tag")) opts.run_tag = file_opts.run_tag;
       if (!was_set("--run-notes")) opts.run_notes = file_opts.run_notes;
       if (!was_set("--questdb-host")) opts.questdb_host = file_opts.questdb_host;
       if (!was_set("--questdb-ilp-port")) opts.questdb_ilp_port = file_opts.questdb_ilp_port;
       if (!was_set("--questdb-http-port")) opts.questdb_http_port = file_opts.questdb_http_port;
   #endif
   ```

6. **Human-readable dump** (`print_config`) — in the summary block:
   ```cpp
   #ifdef HAS_QUESTDB
       std::cout << "    Persistence:   " << (o.persist ? "QuestDB" : "no") << "\n";
       if (o.persist)
       {
           std::cout << "    QuestDB host:  " << o.questdb_host << "\n";
           std::cout << "    ILP port:      " << o.questdb_ilp_port << "\n";
           std::cout << "    HTTP port:     " << o.questdb_http_port << "\n";
           std::cout << "    Run tag:       "
                     << (o.run_tag.empty() ? "(auto)" : o.run_tag) << "\n";
       }
   #endif
   ```

7. **Copy to `engine_config`** — at the spot where `prov_cfg.initial_balance`
   etc. are assigned:
   ```cpp
   #ifdef HAS_QUESTDB
       prov_cfg.persist_enabled = o.persist;
       prov_cfg.questdb_host = o.questdb_host;
       prov_cfg.questdb_ilp_port = o.questdb_ilp_port;
       prov_cfg.questdb_http_port = o.questdb_http_port;
       prov_cfg.run_tag = o.run_tag;
       prov_cfg.run_notes = o.run_notes;
   #endif
   ```

### Tests

Extend `tests/test_cli.cpp` with:

1. `CLI.PersistFlagAccepted` — `--persist` succeeds on `ENABLE_QUESTDB=ON`
   build. Verify via `--dump-config` that `"persist": true` appears.
2. `CLI.PersistRejectedWhenQuestDbDisabled` — on `ENABLE_QUESTDB=OFF`
   build, `--persist` produces "unknown flag" error.
3. `CLI.RunTagFlagAccepted`
4. `CLI.QuestdbPortsAccepted`

The last three need a gate so they only run on `ENABLE_QUESTDB=ON` builds.
Use `#ifdef HAS_QUESTDB` around the test bodies, or a `GTEST_SKIP()`.

### Tests / Verification

```bash
# ENABLE_QUESTDB=ON build
cmake --build build -j$(nproc)
ctest --test-dir build -R "CLI" --output-on-failure

# Manual smoke
./build/engine_backtest \
    --provider local --path market_data.csv \
    --strategy sma \
    --persist --run-tag smoke_test \
    --dry-run --dump-config
```

### Done when

- New CLI tests pass.
- Dry-run output reflects the `--persist` + `--run-tag` values.
- `ENABLE_QUESTDB=OFF` build rejects `--persist` with a helpful error.

---

## Step 9 — Startup health check + soft warning polish

**Goal:** Make the soft-warning experience production-worthy. Confirm the
engine starts cleanly against a live QuestDB, and continues cleanly
when the daemon is down.

### Files

- `src/engine/engine.cpp` — tighten `questdb_begin()` error messaging
- `src/data/questdb/store.cpp` — ensure `begin()` reports **which** step
  failed (DDL vs ILP connect vs initial row) in a log line

### Work

Inside `QuestdbStore::begin()`, when a step fails, print:
```
[questdb] DDL for runs_meta failed (HTTP 503)
[questdb] ILP connect to 127.0.0.1:9009 failed (Connection refused)
```

`engine::questdb_begin()` already prints the high-level warning; the
store provides the low-level breadcrumb.

### Manual verification

**Test 1 — daemon not running:**

```bash
# Make sure no QuestDB is running on 9000/9009
pgrep -f questdb && echo "WARNING: QuestDB already running, stop it first"

./build/engine_backtest \
    --provider local --path market_data.csv \
    --strategy sma \
    --persist
```

Expected output includes:

- A warning line from the store: `[questdb] ... failed (Connection refused)`
- The engine warning: `WARNING: QuestDB unreachable at 127.0.0.1:9000 — …`
- Backtest proceeds and completes normally.
- Exit code 0.

**Test 2 — daemon running (requires local QuestDB; see Appendix C):**

```bash
# Start QuestDB if you have it installed, e.g. via Docker:
docker run --rm -d --name truetest-questdb \
    -p 9000:9000 -p 9009:9009 -p 8812:8812 \
    questdb/questdb:latest

sleep 5  # wait for daemon

./build/engine_backtest \
    --provider local --path market_data.csv \
    --strategy mean-reversion \
    --persist --run-tag manual_smoke

# Expected: no warnings, backtest completes normally.

# Verify tables exist:
curl -s 'http://127.0.0.1:9000/exec?query=SELECT+table_name+FROM+tables()+ORDER+BY+table_name' | jq '.dataset[][0]'
# Should list: manual_smoke_amendments, manual_smoke_cancellations,
#              manual_smoke_fills, manual_smoke_order_status,
#              manual_smoke_orders, manual_smoke_rejections, runs_meta

# Verify runs_meta has two rows (start + end):
curl -s "http://127.0.0.1:9000/exec?query=SELECT+COUNT(*)+FROM+runs_meta+WHERE+run_tag='manual_smoke'" | jq

# Verify orders were captured:
curl -s "http://127.0.0.1:9000/exec?query=SELECT+COUNT(*)+FROM+manual_smoke_orders" | jq

# Tear down
docker stop truetest-questdb
```

### Done when

- Test 1 produces the expected warning and the engine completes normally.
- Test 2 produces a run with both `runs_meta` rows + populated per-run
  tables.

---

## Step 10 — End-to-end integration test

**Goal:** A single, gated test that runs a full backtest with persistence
enabled and verifies every table is populated correctly. Skipped when no
QuestDB is reachable.

### Files

- `tests/test_questdb_integration.cpp`

### Work

Gate on an environment variable so CI without a QuestDB daemon doesn't
fail:

```cpp
#include <cstdlib>

TEST(QuestdbIntegration, EndToEndBacktest)
{
    const char* host = std::getenv("QUESTDB_TEST_HOST");
    if (!host) GTEST_SKIP() << "QUESTDB_TEST_HOST not set — skipping.";

    // 1. Pick a unique run tag so re-runs don't collide
    std::string tag = "itest_" + std::to_string(std::time(nullptr));

    // 2. Build engine_config with persist_enabled=true and
    //    run_tag=tag, pointing at host:9000/9009
    engine_config cfg;
    cfg.persist_enabled = true;
    cfg.questdb_host = host;
    cfg.run_tag = tag;
    cfg.initial_balance = 10000.0;
    // ... usual config

    // 3. Run the engine on a small CSV
    engine e(...);
    e.run();

    // 4. Query QuestDB via HTTP and assert:
    //    - runs_meta has ≥ 2 rows for this tag (begin + end)
    //    - {tag}_orders has > 0 rows
    //    - {tag}_fills has > 0 rows
    //    - all rows have run_tag=tag
    auto res = truetest::questdb::query_exec(host, 9000,
        "SELECT COUNT(*) FROM runs_meta WHERE run_tag='" + tag + "'");
    ASSERT_TRUE(res.has_value());
    // Parse the response JSON, assert count >= 2.

    // 5. Clean up
    for (const char* suffix : {"_orders", "_order_status", "_fills",
                               "_rejections", "_cancellations", "_amendments"})
    {
        truetest::questdb::query_exec(host, 9000,
            std::string("DROP TABLE IF EXISTS ") + tag + suffix);
    }
    truetest::questdb::query_exec(host, 9000,
        "DELETE FROM runs_meta WHERE run_tag='" + tag + "'");
}
```

Add to CMake under `BUILD_TESTS` gated on `ENABLE_QUESTDB`:

```cmake
if(ENABLE_QUESTDB)
    list(APPEND TEST_SOURCES tests/test_questdb_integration.cpp)
endif()
```

### Tests / Verification

```bash
# Without QuestDB — test is skipped
ctest --test-dir build -R "QuestdbIntegration" -V

# With QuestDB
docker run --rm -d --name truetest-questdb \
    -p 9000:9000 -p 9009:9009 -p 8812:8812 \
    questdb/questdb:latest
sleep 5
QUESTDB_TEST_HOST=127.0.0.1 ctest --test-dir build -R "QuestdbIntegration" -V
docker stop truetest-questdb
```

### Done when

- Without QuestDB: test is `SKIPPED`, not `FAILED`.
- With QuestDB: test passes and cleans up its tables.

---

## Step 11 — Documentation update

**Goal:** Docs accurately describe the new persistence path.

### Files

- `CLAUDE.md`
- `docs/instructions.md`
- `README.md`
- `docs/licenses.md` (no deps added, but note zero-dep rationale)

### Work

**CLAUDE.md — project structure tree, add under `src/data/`:**

```
├── data/
│   ├── ...existing entries...
│   └── questdb/                        # QuestDB persistence (#ifdef HAS_QUESTDB)
│       ├── tcp_client.h/.cpp           # raw POSIX socket client
│       ├── http_client.h/.cpp          # HTTP/1.1 POST /exec for DDL
│       ├── ilp_writer.h/.cpp           # line protocol + batching + reconnect
│       ├── schema.h/.cpp               # CREATE TABLE IF NOT EXISTS DDL (7 tables)
│       ├── run_tag.h/.cpp              # per-run table-prefix generator
│       └── store.h/.cpp                # QuestdbStore facade
```

And under `src/engine/`:

```
    │   ├── questdb_worker.h            # drains questdb_ring (#ifdef HAS_QUESTDB)
```

**CLAUDE.md — CMake flags section, add row:**

```
  -DENABLE_QUESTDB=ON \       # QuestDB per-run persistence (raw sockets, no new deps)
```

**CLAUDE.md — new architecture section after "Portfolio checkpointing":**

```markdown
### QuestDB persistence
When `-DENABLE_QUESTDB=ON` and the runtime `--persist` flag is set, every
order lifecycle event (submission, status transition, fill, rejection,
cancellation, amendment) is written to QuestDB. Each session creates
six per-run tables prefixed with `{run_tag}` plus one row in the permanent
`runs_meta` table. Transport is hand-rolled raw sockets — HTTP on port
9000 for DDL, InfluxDB Line Protocol on port 9009 for ingest. Writes run
on a dedicated `QuestDbWorker` thread, off the event loop. If the daemon
is unreachable at startup, a warning is printed and the session continues
without persistence (hard-fail is a future TODO). See `docs/db.md` for the
full schema and implementation plan.
```

**docs/instructions.md** — add a new `## 23. QuestDB Persistence` section
(filling the hole left by the removed WebSocket UI section) containing:

- Overview + when to use
- CLI flags table for `--persist`, `--run-tag`, `--questdb-host`,
  `--questdb-ilp-port`, `--questdb-http-port`, `--run-notes`
- Complete table schema reference (link to Appendix A in `db.md`)
- Example queries (count fills, compare two runs, list all runs)
- Health-check behaviour
- Restart the local daemon instructions
- Add the TOC entry back pointing to it

**README.md** — feature list gets a new line:

```
Optional features (PostgreSQL, Binance live streaming, QuestDB persistence,
sanitisers, benchmarks, shared library) …
```

### Done when

- `git diff` shows coherent doc updates.
- `grep -n questdb docs/instructions.md` lists your new section.

---

## Step 12 — Final validation

**Goal:** Pre-merge checklist.

### Verification

```bash
# Clean default build (QuestDB off)
rm -rf build
cmake -B build -DBUILD_TESTS=ON
cmake --build build -j$(nproc)
ctest --test-dir build -j$(nproc)
#   Expected: same pass/fail as before this PR (DataHandler.HasBarData
#   is the single pre-existing failure).

# Clean QuestDB-enabled build
rm -rf build
cmake -B build -DBUILD_TESTS=ON -DENABLE_QUESTDB=ON
cmake --build build -j$(nproc)
ctest --test-dir build -j$(nproc)
#   Expected: same pass count + all new QuestDB unit tests.
#   test_questdb_integration is SKIPPED without QUESTDB_TEST_HOST.

# Full integration run
docker run --rm -d --name truetest-questdb \
    -p 9000:9000 -p 9009:9009 questdb/questdb:latest
sleep 5
QUESTDB_TEST_HOST=127.0.0.1 ctest --test-dir build -R "QuestdbIntegration"

# Manual end-to-end
./build/engine_backtest --provider local --path market_data.csv \
    --strategy mean-reversion --persist --run-tag pre_merge_check
curl -s "http://127.0.0.1:9000/exec?query=SELECT+COUNT(*)+FROM+pre_merge_check_orders" | jq

docker stop truetest-questdb

# Soft-warning path
./build/engine_backtest --provider local --path market_data.csv \
    --strategy mean-reversion --persist
#   Expected: warning printed, backtest still completes, exit 0.
```

### Done when

- Both builds pass tests.
- Integration test passes against live QuestDB.
- Soft-warning path exits cleanly.
- Docs updated.
- No `HAS_WEB_UI` / `ENABLE_WEB_UI` regressions (those stay deleted).

---

## Appendix A — Full DDL (verbatim)

**`runs_meta_ddl()`:**

```sql
CREATE TABLE IF NOT EXISTS runs_meta (
    run_tag          SYMBOL CAPACITY 10000 INDEX,
    started_at       TIMESTAMP,
    ended_at         TIMESTAMP,
    mode             SYMBOL CAPACITY 4,
    binary           SYMBOL CAPACITY 4,
    strategy         SYMBOL CAPACITY 64,
    symbol           SYMBOL CAPACITY 1024,
    params           STRING,
    initial_equity   DOUBLE,
    final_equity     DOUBLE,
    total_orders     LONG,
    total_fills      LONG,
    total_rejections LONG,
    notes            STRING
) TIMESTAMP(started_at) PARTITION BY MONTH
```

**`{prefix}_orders`:**

```sql
CREATE TABLE IF NOT EXISTS {prefix}_orders (
    ts               TIMESTAMP,
    run_tag          SYMBOL CAPACITY 10000,
    order_id         LONG,
    symbol           SYMBOL CAPACITY 1024,
    side             SYMBOL CAPACITY 2,
    type             SYMBOL CAPACITY 4,
    tif              SYMBOL CAPACITY 4,
    qty              DOUBLE,
    price            DOUBLE,
    stop_price       DOUBLE,
    strategy_name    SYMBOL CAPACITY 64,
    opener_order_id  LONG,
    initial_status   SYMBOL CAPACITY 8
) TIMESTAMP(ts) PARTITION BY DAY
```

**`{prefix}_order_status`:**

```sql
CREATE TABLE IF NOT EXISTS {prefix}_order_status (
    ts               TIMESTAMP,
    run_tag          SYMBOL CAPACITY 10000,
    order_id         LONG,
    old_status       SYMBOL CAPACITY 8,
    new_status       SYMBOL CAPACITY 8,
    reason           STRING
) TIMESTAMP(ts) PARTITION BY DAY
```

**`{prefix}_fills`:**

```sql
CREATE TABLE IF NOT EXISTS {prefix}_fills (
    ts               TIMESTAMP,
    run_tag          SYMBOL CAPACITY 10000,
    fill_id          LONG,
    order_id         LONG,
    opener_order_id  LONG,
    symbol           SYMBOL CAPACITY 1024,
    side             SYMBOL CAPACITY 2,
    qty              DOUBLE,
    price            DOUBLE,
    remaining_qty    DOUBLE,
    fee              DOUBLE,
    strategy_name    SYMBOL CAPACITY 64,
    source           SYMBOL CAPACITY 8
) TIMESTAMP(ts) PARTITION BY DAY
```

**`{prefix}_rejections`:**

```sql
CREATE TABLE IF NOT EXISTS {prefix}_rejections (
    ts               TIMESTAMP,
    run_tag          SYMBOL CAPACITY 10000,
    order_id         LONG,
    symbol           SYMBOL CAPACITY 1024,
    side             SYMBOL CAPACITY 2,
    qty              DOUBLE,
    price            DOUBLE,
    strategy_name    SYMBOL CAPACITY 64,
    reason           SYMBOL CAPACITY 32,
    reason_detail    STRING
) TIMESTAMP(ts) PARTITION BY DAY
```

**`{prefix}_cancellations`:**

```sql
CREATE TABLE IF NOT EXISTS {prefix}_cancellations (
    ts               TIMESTAMP,
    run_tag          SYMBOL CAPACITY 10000,
    order_id         LONG,
    symbol           SYMBOL CAPACITY 1024,
    strategy_name    SYMBOL CAPACITY 64,
    reason           SYMBOL CAPACITY 16
) TIMESTAMP(ts) PARTITION BY DAY
```

**`{prefix}_amendments`:**

```sql
CREATE TABLE IF NOT EXISTS {prefix}_amendments (
    ts               TIMESTAMP,
    run_tag          SYMBOL CAPACITY 10000,
    order_id         LONG,
    symbol           SYMBOL CAPACITY 1024,
    old_price        DOUBLE,
    new_price        DOUBLE,
    old_qty          DOUBLE,
    new_qty          DOUBLE
) TIMESTAMP(ts) PARTITION BY DAY
```

---

## Appendix B — ILP format cheatsheet

General shape:

```
table_name,tag1=v1,tag2=v2 field1=<value>,field2="string" 1700000000000000000
```

Rules:

- Exactly one space between the tag block and the field block.
- Exactly one space between the field block and the timestamp.
- Tags are comma-separated; no space around `=`.
- Fields are comma-separated; no space around `=`.
- String field values **must** be wrapped in `"..."`.
- Numeric field values are bare (`42`, `3.14`, `1.23e-4`).
- Boolean field values are `t` or `f`.
- Integer (LONG) field values need a trailing `i`: `42i`.
- Timestamp is **nanoseconds** since Unix epoch.
- Line must terminate with `\n`.

Escape map:

| Char in tag key/value or field key | Write as |
|---|---|
| space ` `                           | `\ `     |
| comma `,`                           | `\,`     |
| equals `=`                          | `\=`     |

In string field values only `"` and `\` need escaping (`\"`, `\\`).

Example row for `{prefix}_orders` (where prefix is `run_20260424`):

```
run_20260424_orders,run_tag=run_20260424,symbol=BTCUSDT,side=buy,type=limit,tif=gtc,strategy_name=mean-reversion,initial_status=pending order_id=12345i,qty=0.001,price=50000.0,stop_price=0.0,opener_order_id=0i 1714000000000000000
```

---

## Appendix C — Testing against a local QuestDB

Preferred: run the official Docker image.

```bash
docker run --rm -d --name truetest-questdb \
    -p 9000:9000 \
    -p 9009:9009 \
    -p 8812:8812 \
    questdb/questdb:latest

# Wait for readiness
until curl -fs http://127.0.0.1:9000/exec?query=SELECT+1 >/dev/null; do
    sleep 0.5
done

# Your work here…

docker stop truetest-questdb
```

Ports:

- **9000** — HTTP endpoint: DDL (`GET /exec?query=...`), web console at `/`.
- **9009** — InfluxDB Line Protocol ingest (TCP).
- **8812** — PostgreSQL wire protocol (not used by the engine; handy if
  you want to run `psql` against your data for ad-hoc analysis).

Web console (browser): `http://127.0.0.1:9000/`. Useful for poking at
tables after a run.

Drop everything from a run:

```bash
for t in orders order_status fills rejections cancellations amendments; do
    curl -s "http://127.0.0.1:9000/exec?query=DROP+TABLE+IF+EXISTS+${RUN_TAG}_${t}"
done
curl -s "http://127.0.0.1:9000/exec?query=DELETE+FROM+runs_meta+WHERE+run_tag='${RUN_TAG}'"
```

---

## Conventions summary

- Every file in `src/data/questdb/` and `src/engine/questdb_worker.h`
  starts with `#ifdef HAS_QUESTDB` and ends with `#endif`.
- Every engine hot-path integration point uses
  `#ifdef HAS_QUESTDB` + `if (questdb_active_)` double-guard — compile-time
  for the feature, runtime for the session-level opt-in.
- No new external dependencies. The raw socket path is intentional.
- Worker threading mirrors `LoggingWorker` exactly.
- Error handling at boundaries: log a clear message, continue without
  persistence, do not crash the run.

---

## Out of scope (do NOT do in this PR)

- Removing SQLite / PostgreSQL backends — that's a follow-up PR.
- Hard-fail when QuestDB is unreachable — future TODO.
- Market-data tables (bar, tick, L2 snapshots).
- Equity-curve snapshot table.
- PGWire-based query helpers from inside the engine.
- Remote QuestDB with TLS.
