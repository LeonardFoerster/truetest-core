# Step 1: Persistent State (Trade History, Portfolio, PnL)

## Goal

Wire a SQLite database into the engine's fill/portfolio pipeline so that all trades, portfolio snapshots, and equity history survive engine restarts. Expose stored history to the web UI via new WS message types.

SQLite is chosen over PostgreSQL for this step because it requires zero external dependencies (single-file DB), keeps the "zero deps by default" convention, and is sufficient for a single-engine SaaS instance. PostgreSQL migration can follow later behind the existing `ENABLE_POSTGRESQL` flag.

---

## Phase 1: SQLite Storage Layer

### 1.1 Add SQLite3 dependency to CMake

**File:** `CMakeLists.txt`

- Add a new CMake option: `option(ENABLE_SQLITE "SQLite persistence for trades/portfolio" ON)`
- When ON, find SQLite3: `find_package(SQLite3 REQUIRED)`
- Define `HAS_SQLITE` compile definition
- Link `SQLite::SQLite3` to the `truetest` target
- SQLite is ON by default (unlike PostgreSQL) because it has no external server dependency

### 1.2 Create `BacktestEngine/src/data/sqlite_store.h`

A new header-only class `SqliteStore` that owns a `sqlite3*` connection.

**Constructor:** `SqliteStore(const std::string& db_path = "truetest.db")`
- Opens or creates the database file
- Calls `create_tables()` on first open
- Enables WAL mode: `PRAGMA journal_mode=WAL;`
- Enables foreign keys: `PRAGMA foreign_keys=ON;`

**Tables to create (in `create_tables()`):**

```sql
CREATE TABLE IF NOT EXISTS fills (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    timestamp   INTEGER NOT NULL,       -- epoch milliseconds
    order_id    INTEGER NOT NULL,
    symbol      TEXT NOT NULL,
    side        TEXT NOT NULL,           -- 'buy' or 'sell'
    quantity    REAL NOT NULL,
    price       REAL NOT NULL,
    commission  REAL NOT NULL DEFAULT 0,
    created_at  INTEGER NOT NULL DEFAULT (strftime('%s','now') * 1000)
);

CREATE TABLE IF NOT EXISTS portfolio_snapshots (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    timestamp   INTEGER NOT NULL,       -- epoch milliseconds
    cash        REAL NOT NULL,
    equity      REAL NOT NULL,
    positions   TEXT NOT NULL,           -- JSON array: [{"symbol":"X","qty":1.0,"cost_basis":100.0}]
    total_trades INTEGER NOT NULL,
    created_at  INTEGER NOT NULL DEFAULT (strftime('%s','now') * 1000)
);

CREATE TABLE IF NOT EXISTS equity_curve (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    timestamp   INTEGER NOT NULL,
    equity      REAL NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_fills_timestamp ON fills(timestamp);
CREATE INDEX IF NOT EXISTS idx_fills_symbol ON fills(symbol);
CREATE INDEX IF NOT EXISTS idx_equity_timestamp ON equity_curve(timestamp);
```

**Methods:**

```cpp
// Write path (called from engine hot loop — must be fast)
void insert_fill(const fill_event& f);
void insert_portfolio_snapshot(double cash, double equity,
    const std::string& positions_json, std::size_t total_trades,
    int64_t timestamp_ms);
void insert_equity_point(int64_t timestamp_ms, double equity);

// Read path (called on client connect or explicit request)
std::string query_fills_json(const std::string& symbol = "",
    int limit = 200, int64_t since_ms = 0);
    // Returns JSON array of fill objects

std::string query_equity_json(int limit = 500);
    // Returns JSON array of {timestamp, equity} objects

std::string query_last_portfolio_json();
    // Returns single portfolio snapshot JSON object
```

**Write batching:**
- Use a prepared statement cache (store `sqlite3_stmt*` for each INSERT)
- Wrap every N inserts in a transaction (e.g., `BEGIN`/`COMMIT` every 100 fills)
- Use `insert_fill` immediately (fills are infrequent), but batch `insert_equity_point` (called per bar)

### 1.3 Create `BacktestEngine/src/data/sqlite_store.cpp`

Implement all methods. Use `sqlite3_prepare_v2` + `sqlite3_bind_*` + `sqlite3_step` pattern. For JSON output from queries, use the existing snprintf style (no JSON library).

---

## Phase 2: Wire into Engine

### 2.1 Add store to `engine_config.h`

**File:** `BacktestEngine/src/core/engine_config.h`

Add field:
```cpp
std::string db_path;  // SQLite database path (empty = no persistence)
```

### 2.2 Add store to `engine.h` and `engine.cpp`

**File:** `BacktestEngine/src/core/engine.h`

Add under `#ifdef HAS_SQLITE`:
```cpp
#include "../data/sqlite_store.h"
std::unique_ptr<SqliteStore> store_;
```

**File:** `BacktestEngine/src/core/engine.cpp`

- In the constructor: if `config_.db_path` is not empty, create `store_`
- In `process_order()` after a fill is applied to portfolio: call `store_->insert_fill(f)`
- In `process_single_bar()` / `process_single_tick()`: after processing, periodically call `store_->insert_equity_point(ts, equity)` and `store_->insert_portfolio_snapshot(...)` (throttle to once per bar or once per N seconds)
- In `send_state_snapshot()`: if store exists, query last portfolio and equity curve, broadcast as new message types

### 2.3 Wire CLI flag in `main.cpp`

**File:** `BacktestEngine/src/main.cpp`

- Add `--db <path>` CLI flag, default `"truetest.db"`
- Set `prov_cfg.db_path` from this flag
- Add to `start.sh` config block: `DB_PATH="truetest.db"`

---

## Phase 3: New WS Message Types

### 3.1 Add serialization in `event_json.h`

**File:** `BacktestEngine/src/core/event_json.h`

Add new functions:
```cpp
// Wraps pre-built JSON arrays from SqliteStore queries
inline std::string fills_history_to_json(const std::string& fills_array) {
    return R"({"type":"fills_history","data":)" + fills_array + "}";
}

inline std::string equity_history_to_json(const std::string& equity_array) {
    return R"({"type":"equity_history","data":)" + equity_array + "}";
}
```

### 3.2 Broadcast history on client connect

**File:** `BacktestEngine/src/core/engine.cpp` in `send_state_snapshot()`

After the existing portfolio/analytics broadcast, add:
```cpp
#ifdef HAS_SQLITE
if (store_) {
    ws_worker_->broadcast(event_json::fills_history_to_json(
        store_->query_fills_json("", 200)));
    ws_worker_->broadcast(event_json::equity_history_to_json(
        store_->query_equity_json(500)));
}
#endif
```

### 3.3 Handle `query_fills` command from browser

In `process_ws_commands()`, add a new command handler:
```cpp
else if (cmd.command == "query_fills") {
    // cmd.timeframe reused as symbol filter, cmd.price reused as limit
    int limit = cmd.price > 0 ? static_cast<int>(cmd.price) : 200;
    if (store_) {
        ws_worker_->broadcast(event_json::fills_history_to_json(
            store_->query_fills_json(cmd.timeframe, limit)));
    }
}
```

---

## Phase 4: Frontend Integration

### 4.1 Handle new message types in dispatcher

**File:** `web/src/store/dispatcher.ts`

Add cases:
```typescript
case 'fills_history': {
    const fills = (msg.data as any[]).map((f: any, i: number) => ({
        id: f.id ?? i,
        time: f.timestamp ? f.timestamp / 1000 : 0,
        symbol: f.symbol ?? '',
        side: f.side ?? 'buy',
        quantity: f.quantity ?? 0,
        price: f.price ?? 0,
        commission: f.commission ?? 0,
    }));
    dispatchers.fill({ type: 'BULK_ADD', fills });
    break;
}

case 'equity_history': {
    const points = msg.data as { timestamp: number; equity: number }[];
    for (const p of points) {
        dispatchers.analytics({ type: 'ADD_EQUITY_POINT', value: p.equity });
    }
    break;
}
```

### 4.2 Increase FillStore capacity

**File:** `web/src/store/FillStore.tsx`

Change `MAX_FILLS` from 200 to 1000 so historical fills from the DB aren't immediately truncated.

### 4.3 Add `BULK_ADD` action if not present

Verify `FillStore.tsx` reducer handles `BULK_ADD` — it already does per the exploration. Confirm it deduplicates by `id` to avoid duplicating fills that arrive both from the live stream and the history query.

---

## Phase 5: Restore State on Engine Restart

### 5.1 Load last portfolio state

In `engine.cpp` constructor, after creating `store_`:
```cpp
if (store_) {
    auto last = store_->query_last_portfolio_json();
    if (!last.empty()) {
        // Parse cash and positions from JSON, restore portfolio_ state
        // This allows the engine to resume from where it left off
    }
}
```

This is optional for the first iteration — flag it with a `--resume` CLI option so users explicitly opt in to state restoration.

---

## Testing

1. Unit test `SqliteStore` directly: create in-memory DB (`:memory:`), insert fills, query back, verify JSON output
2. Integration test: run engine with `--db :memory:`, process a few bars, verify fills are persisted
3. Frontend test: connect browser, verify fills_history populates the Fills tab on connect
4. Restart test: run engine, generate trades, stop, restart with same DB path, verify history loads

---

## Files Changed (Summary)

| File | Change |
|------|--------|
| `CMakeLists.txt` | Add `ENABLE_SQLITE` option, find/link SQLite3 |
| `BacktestEngine/src/data/sqlite_store.h` | **NEW** — SQLite storage class |
| `BacktestEngine/src/data/sqlite_store.cpp` | **NEW** — Implementation |
| `BacktestEngine/src/core/engine_config.h` | Add `db_path` field |
| `BacktestEngine/src/core/engine.h` | Add `store_` member |
| `BacktestEngine/src/core/engine.cpp` | Wire store into fill pipeline, snapshot, commands |
| `BacktestEngine/src/core/event_json.h` | Add history JSON wrappers |
| `BacktestEngine/src/main.cpp` | Add `--db` CLI flag |
| `start.sh` | Add `DB_PATH` config variable |
| `web/src/store/dispatcher.ts` | Handle `fills_history`, `equity_history` |
| `web/src/store/FillStore.tsx` | Increase MAX_FILLS, verify BULK_ADD dedup |
