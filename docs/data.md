# Data Pipeline Redesign Plan

**File**: `core/docs/data.md`  
**Purpose**: Detailed, phased execution plan for redesigning the market-data path so storage is independent of formats, and a single **DataWrapper** façade can accept CSV, Parquet, historical APIs, and more — without weakening hot-path, layer, or live-safety invariants.  
**Status**: **IMPLEMENTED through D-07 + D-10 core** (2026-07-31). D-08 Parquet and D-09 historical API deferred (need human dep/network approval).  
**Scope**: `src/data/**`, `src/providers/data_bridge.h`, related sinks/parsers/transports, engine **batch iteration and streaming feed wiring** only (not full engine decomposition).  
**Out of scope**: QuestDB order-audit egress (keep `IOrderAuditSink`); venue live-safety freeze surface except where engine feed loops must change.

### Implementation status (2026-07-31)

| Phase | Status | Notes |
|-------|--------|-------|
| D-00 Characterization | Done | Inventory via workflow agents; tests extended |
| D-01 Domain + IMarketSink | Done | `market_types.h`, `market_sink.h`, `MarketSeries::on_bar/on_tick` |
| D-02 Engine read API | Done | `bar_at` / `tick_at` / `bar_count`; freeze-minimal engine.cpp edit |
| D-03 MarketSeries private store | Done | SoA private; `using data_handler = MarketSeries`; `load_from_csv` moved out |
| D-04 IMarketSource + DataBridge | Done | `market_source.h`; DataBridge `load_into`; IDataSource shim |
| D-05 DataWrapper | Done | `from_path` / `from_paths` / `from_uri` / `from_source`; API uses wrapper |
| D-06 Stream retain policy | Done | `retain_streamed=false` default; tests cover both modes |
| D-07 Dead code | Done | **Deleted** BinaryCacheSource + WebSocketDataSource |
| D-08 Parquet | Deferred | Needs human approval for Arrow/Parquet dep |
| D-09 Historical API | Deferred | Prefer provider-local history; no generic HTTP client yet |
| D-10 Timestamps | Done | `bar_ts_` primary sort; engine uses stored `ts` when `seed==0` |
| D-11 Cleanup | Partial | Typedefs kept one cycle; this status table is the freeze note |

---

## References (Read Before Starting)

| Doc / path | Why |
|------------|-----|
| `AGENTS.md` (core) §4–7 | Hot path, layers, provider extension point |
| `docs/architecture/04-performance.md` | Zero-alloc / capacity rules |
| `docs/architecture/01-target-architecture.md` | High-level data flow |
| `docs/architecture/02-model.md` | Anti-patterns |
| `docs/reference/01-instructions.md` | CLI providers, MC flags |
| `docs/reference/02-user-manual.md` | Operator-facing architecture |
| `docs/engine.md` | Engine decomp; touch `run*` carefully |
| `docs/reference/03-db.md` | QuestDB = audit, not market ingress |
| `src/data/market_series.*`, `market_types.h`, `data_wrapper.*` | New store + façade (was data_handler/data_loader) |
| `src/providers/data_bridge.h`, `transport.h`, `parser.h` | Existing multi-format skeleton |
| `src/providers/local/csv_parser.h`, `provider_sink.h`, `provider_convert.h` | Parse / sink boundary |
| `src/engine/engine.cpp` (`run`, `run_tick_data`, `run_streaming`) | Consumers of layout |
| `src/simulation/monte_carlo_controller.*` | Synthetic fill + reuse |
| `src/bin/main.inc`, `src/api/truetest_api.cpp` | Ownership / factory sites |
| `scripts/check-layer-deps.sh`, `check-hotpath-json.sh` | Mandatory gates after `src/` edits |

Cross-reference work items as: `core/docs/data.md#D-03` (phase / wave id).

---

## 1. Goals (Non-Negotiable)

1. **Independent store** — market series storage has zero knowledge of CSV, Parquet, HTTP, WebSocket, or venue APIs.
2. **Multi-format façade** — a `DataWrapper` (or equivalent name) is the single composition root that can open:
   - local CSV (bars and ticks)
   - Parquet / columnar files (when dependency approved)
   - historical HTTP/API feeds
   - existing `IProvider` batch/stream transports
   - synthetic / MC paths
   - future formats without editing the store or engine loop body
3. **Canonical domain records** — all formats decode to shared `Bar` / `Tick` (and later optional L2) types; sources never write `db_data_*` columns directly.
4. **Preserve behaviour** for current CSV + provider paths unless a phase explicitly documents a deliberate change (e.g. stream retain policy).
5. **Preserve hot-path discipline** — load/decode remains cold; batch iteration stays zero-extra-alloc (views into SoA); no JSON / heavy fmt on event loop.
6. **Preserve layering** — no `HAS_BINANCE` / venue leakage into `src/data` store types; provider remains sole **venue** extension point.
7. **Net complexity reduction** — delete dual stacks (legacy `CsvDataSource` vs `DataBridge`) and unwired dead ends (`BinaryCacheSource` / `WebSocketDataSource`) by wiring *or* removing, not by adding a third parallel path.
8. **Determinism** — same seed + same resolved series ⇒ same backtest; MC per-trial seeding unchanged; timestamps become first-class on the series over string date sort.

---

## 2. Non-Goals

- Replacing `IProvider` for live/shadow venue connectivity.
- Using QuestDB as a market-data warehouse for ingress (unless a separate, explicit product design lands later).
- Arrow/Parquet zero-copy into the engine hot loop in v1 (decode cold into SoA is enough).
- Unifying L2 book construction into the series store (L2 stays on provider event / orderbook path).
- Softening validation or silently accepting corrupt rows as success without counters.
- Adding new third-party deps without human approval (`AGENTS.md`).

---

## 3. Current State (Baseline — 2026-07)

### 3.1 Inventory (`src/data`)

| Component | Role today | Problem |
|-----------|------------|---------|
| `data_handler` | SoA bars + AoS ticks, validation, **and** `load_from_csv` | God object; public mutable vectors |
| `IDataSource` | `load_data(shared_ptr<data_handler>)` | Couples every source to concrete store |
| `CsvDataSource` / `TickCsvDataSource` | Legacy direct loaders | Duplicate of DataBridge + CSV parsers |
| `BinaryCacheSource` | TTBC bar cache decorator | Compiled, **unwired**, bypasses validation on hit |
| `WebSocketDataSource` | Push WS under `HAS_LIVE_DATA` | **Unwired**; superseded by venue transports |
| `date_parse` | String → `time_point` | Used at engine run time for bar ts when `seed==0` |
| `questdb/*` | Order/run audit ILP | Correctly separate; **do not fold into market series** |

### 3.2 Parallel “modern” stack (`src/providers`)

```
IProvider → IDataTransport → IDataParser<T> → sink_fn → data_handler
                ↑
         DataBridge<T>  (IDataSource for batch + run_streaming for live)
```

This is the **right shape** for multi-format batch/stream, but:

- Transport is **string/frame** oriented (`read_line` / `read_frame`) — poor fit for Parquet.
- Sinks still take `shared_ptr<data_handler>` and know the concrete type.
- Engine batch path still indexes public `db_data_*` columns.

### 3.3 Primary consumers

| Consumer | How it uses data |
|----------|------------------|
| `engine::run` | Prefers ticks if present; else indexes bar SoA columns → `market_event` |
| `engine::run_tick_data` | Iterates `tick_data` |
| `engine::run_streaming` | `DataBridge::run_streaming(handler, on_record)` — still **appends** into handler |
| `MonteCarloController` | `load_into_queue` from `SyntheticPath` (bars only today); `reset` reuse |
| `main.inc` | Owns `shared_ptr<data_handler>`; provider mode builds bridges; legacy CSV path remains |
| `truetest_api` | Bar CSV via `CsvDataSource` only |
| Tests | Many helpers construct `data_handler` and poke columns / `load_into_queue` |

### 3.4 Pain summary

1. Store ↔ format coupling (`load_from_csv` on the buffer).  
2. Layout-as-API (`db_data_open_value[i]` everywhere).  
3. Dual ingress stacks (legacy sources vs DataBridge).  
4. Text-only transport assumption.  
5. Stream path retains all records (unbounded growth).  
6. Dead / half-built wrappers (binary cache, generic WS source).  
7. Timestamps as date **strings** + parse at run (fragile multi-format story).

---

## 4. Target Architecture

### 4.1 Layer diagram

```
┌──────────────────────────────────────────────────────────────────┐
│ CLI / API / backend job / tests                                  │
│   DataWrapper::from_uri(spec) | from_provider(...) | from_series │
└────────────────────────────┬─────────────────────────────────────┘
                             │
              ┌──────────────┴──────────────┐
              │ batch                         │ stream
              ▼                               ▼
┌─────────────────────────────┐   ┌──────────────────────────────┐
│ IMarketSource::load_into    │   │ IMarketSource::stream_into   │
│   (CSV, Parquet, HTTP, …)   │   │   or Provider + DataBridge   │
└──────────────┬──────────────┘   └──────────────┬───────────────┘
               │ on_bar / on_tick                 │ on_bar / on_tick
               ▼                                  ▼
┌─────────────────────────────┐   ┌──────────────────────────────┐
│ MarketSeries : IMarketSink  │   │ Engine callbacks (IMarketSink│
│  (validate, SoA/AoS store)  │   │  process_single_*; retain?)  │
└──────────────┬──────────────┘   └──────────────────────────────┘
               │ IBarSeries / ITickSeries read API
               ▼
┌─────────────────────────────┐
│ engine::run / run_tick_data │
└─────────────────────────────┘

QuestDB ── IOrderAuditSink only (unchanged; not on this diagram’s ingress path)
```

### 4.2 Responsibility split

| Piece | Responsibility | Must not |
|-------|----------------|----------|
| **Domain types** (`Bar`, `Tick`, side enums) | Canonical records shared by sources, store, engine conversion | Include I/O or venue headers |
| **MarketSeries** (evolved `data_handler`) | Storage, validation, reserve/reset, sort, read views | Know formats, files, sockets |
| **IMarketSink** | Append / process interface | Own transport |
| **IMarketSource** | Open external data → emit domain records into a sink | Touch engine or `db_data_*` |
| **DataWrapper** | Spec/URI factory, multi-file merge, options, cache decorator | Become a second buffer |
| **IProvider + transports** | Venue and live/shadow streams | Grow file-format parsers for CSV/Parquet historical |
| **Engine** | Iterate series or stream callbacks → existing event loop | Include Parquet/Arrow/HTTP client headers |

### 4.3 Naming (recommended)

Prefer clear names; keep temporary typedefs during migration:

| New name | Legacy alias (during migration) |
|----------|----------------------------------|
| `MarketSeries` | `using data_handler = MarketSeries;` (phase-local) |
| `Bar` | Align with / replace `bar_record` over time |
| `Tick` | Align with / replace `tick_record` |
| `DataWrapper` | New façade (no legacy name) |
| `IMarketSource` | Replaces `IDataSource` |
| `IMarketSink` | New; `MarketSeries` + engine adapters implement it |

Use project naming conventions already in tree (`I` prefix for interfaces). Exact file layout suggestion:

```
src/data/
  market_types.h          # Bar, Tick, side, MarketRecord variant (optional)
  market_series.h/.cpp    # store (ex-data_handler + data_loader)
  market_sink.h           # IMarketSink
  market_source.h         # IMarketSource
  data_wrapper.h/.cpp     # façade + URI factory
  sources/
    csv_bar_source.*
    csv_tick_source.*
    parquet_bar_source.*  # later, behind ENABLE / optional dep
    http_history_source.* # later
  date_parse.h            # keep / evolve
  questdb/                # unchanged role
```

Register all new TUs in `cmake/Sources.cmake` (no globs).

---

## 5. Target Interfaces (Normative Sketch)

These are **design contracts** for implementers. Signatures may be adjusted for C++23 style, but semantics must hold.

### 5.1 Domain types

```cpp
// src/data/market_types.h  (header-only preferred)
enum class tick_side : uint8_t { bid = 0, ask = 1, unknown = 2 };

struct Bar {
    std::chrono::system_clock::time_point ts{};
    std::string symbol;   // Phase later: SymbolId via symbol_table
    double open = 0, high = 0, low = 0, close = 0;
    int64_t volume = 0;
    // Optional metadata (interval, source id) only if needed — keep POD-ish
};

struct Tick {
    std::chrono::system_clock::time_point ts{};
    std::string symbol;
    double price = 0;
    int64_t quantity = 0;
    tick_side side = tick_side::unknown;
};
```

**Rules:**

- Prefer **`time_point` on the series**, not date strings as the primary key.
- During migration, string dates may still be accepted at CSV parse time and converted once at load (not every engine iteration forever).
- `provider_convert` maps provider types ↔ domain types in one place.

### 5.2 Sink

```cpp
class IMarketSink {
public:
    virtual ~IMarketSink() = default;
    // return false = reject record (validation); sources continue unless policy says fail-closed
    virtual bool on_bar(const Bar& bar) = 0;
    virtual bool on_tick(const Tick& tick) = 0;
};
```

Optional later: `on_l2(...)` — only if L2 batch materialization is required; default is provider/event path.

### 5.3 Source

```cpp
struct LoadStats {
    std::size_t accepted = 0;
    std::size_t rejected = 0;
    std::string message;
};

class IMarketSource {
public:
    virtual ~IMarketSource() = default;

    // Batch: push all records into sink; return false on hard failure (I/O, schema)
    virtual bool load_into(IMarketSink& sink, LoadStats* stats = nullptr) = 0;

    // Stream: optional; default false = not supported
    virtual bool supports_stream() const { return false; }
    virtual bool stream_into(IMarketSink& sink, std::atomic<bool>* halt = nullptr,
                             LoadStats* stats = nullptr) {
        (void)sink; (void)halt; (void)stats;
        return false;
    }
};
```

**Hard rule:** sources **must not** include `market_series.h` if avoidable — only `IMarketSink` + domain types.

### 5.4 Series store (read + write)

```cpp
class MarketSeries final : public IMarketSink {
public:
    bool on_bar(const Bar&) override;
    bool on_tick(const Tick&) override;

    void reserve_bars(std::size_t n);
    void reserve_ticks(std::size_t n);
    void clear();                 // MC reuse: clear content, keep capacity
    void sort_bars_by_time();     // stable; multi-symbol secondary key = symbol

    std::size_t bar_count() const noexcept;
    std::size_t tick_count() const noexcept;
    bool empty() const noexcept;

    // Zero-extra-alloc views for engine batch loops
    struct BarView {
        std::chrono::system_clock::time_point ts;
        std::string_view symbol;
        double open, high, low, close;
        int64_t volume;
    };
    BarView bar_at(std::size_t i) const;
    const Tick& tick_at(std::size_t i) const;

    std::size_t validation_errors() const noexcept;

    // Migration only — delete after engine + cache migrated:
    // [[deprecated]] accessors to columns if absolutely required
};
```

**Layout:** keep bar **SoA** privately (cache-friendly scan). Ticks may stay AoS unless benchmarks justify change.

**Validation (shared by all formats):**

- Bars: positive OHLC, `high >= low`, `volume >= 0` (same spirit as today).
- Ticks: positive price/qty; optional monotonicity policy (document: skip vs fail-closed).
- Rejected rows increment `validation_errors`; load **continues** unless source-level hard failure (0 accepted after open, schema error, I/O error).

### 5.5 DataWrapper façade

```cpp
struct DataLoadOptions {
    std::optional<std::chrono::system_clock::time_point> from;
    std::optional<std::chrono::system_clock::time_point> to;
    std::vector<std::string> symbols;   // empty = all
    bool sort_after_load = true;
    bool fail_if_empty = true;
    bool retain_streamed = false;       // live default: do not grow series
    std::size_t reserve_hint = 0;
    // cache_path, strict validation, etc.
};

class DataWrapper {
public:
    // Factories
    static DataWrapper from_uri(std::string_view uri, DataLoadOptions opt = {});
    static DataWrapper from_paths(const std::vector<std::filesystem::path>& paths,
                                  DataLoadOptions opt = {});
    static DataWrapper from_provider(std::shared_ptr<IProvider> provider,
                                     DataLoadOptions opt = {});
    static DataWrapper from_source(std::unique_ptr<IMarketSource> source,
                                   DataLoadOptions opt = {});

    // Batch materialize
    bool load(MarketSeries& out);

    // Stream into arbitrary sink (engine adapter)
    bool stream(IMarketSink& sink, std::atomic<bool>* halt = nullptr);

    // Advanced
    IMarketSource& source();
};
```

#### URI scheme (v1 proposal)

| Scheme | Example | Source |
|--------|---------|--------|
| `csv:` / path ending `.csv` | `csv:///data/btc_1m.csv` | CSV bar (header sniff or option for tick) |
| `csv+tick:` | `csv+tick:///data/trades.csv` | CSV tick |
| `parquet:` | `parquet:///data/bars/` | Columnar bar source (phase E) |
| `provider:` | `provider:local?path=...` | Existing provider registry |
| `synthetic:` | reserved for tests/MC helpers | Optional |

CLI may keep existing flags (`--provider`, `--data`, …) and map them to `DataWrapper` factories — **do not** require operators to learn URIs on day one; URIs are the stable programmatic surface.

### 5.6 Transport generalization (important for Parquet)

Do **not** force every format through `IDataTransport::read_line`.

| Kind | Abstraction | Formats |
|------|-------------|---------|
| Frame/text | existing `IDataTransport` + `IDataParser<T>` | CSV, NDJSON, many WS lines |
| Columnar file | `IColumnarBatchSource` (new, data layer) | Parquet / Arrow IPC |
| Page iterator | `IHttpHistoryClient` or provider REST | Historical REST |

`DataBridge` remains the frame/text path. Parquet implements `IMarketSource` directly without pretending rows are lines.

---

## 6. Non-Negotiable Invariants

1. **Hot path (engine event loop)**  
   - No heap growth for market series during `run` iteration (pre-reserved or stable capacity).  
   - `BarView` must not allocate (string_view into stored symbol column is OK).  
   - No nlohmann/json, no format decoders on the event loop.

2. **Cold path (load)**  
   - Allocations OK and expected.  
   - Prefer `reserve` from file metadata / options.

3. **Layering**  
   - `src/data` store + generic file/HTTP history sources: no includes of engine, risk, strategy, venue providers.  
   - Venue-specific parsers stay under `src/providers/<venue>/`.  
   - `DataWrapper::from_provider` may live in a thin glue translation unit that is allowed to see both (e.g. `src/providers/provider_data_wrapper.cpp` or `src/data/provider_source.cpp` registered carefully — **follow `check-layer-deps.sh`**; if data→providers is forbidden, put provider-backed source under `providers/` and only keep URI dispatch in a composition TU near `main` / api).

4. **Provider is sole venue extension**  
   - Adding Binance/Bybit history must not create a parallel venue stack inside `src/data`.  
   - Prefer provider methods + existing transport/parser; DataWrapper only selects them.

5. **Live safety**  
   - Streaming halt remains write-once terminal at engine level; wrapper only observes `halt` pointer.  
   - Touching frozen files requires freeze protocol (`docs/governance/02-prerequisites.md`). Prefer feed-loop changes that avoid freeze files when possible; if `engine.cpp` must change, treat as freeze work.

6. **QuestDB**  
   - Market redesign never routes market ticks through QuestDB ILP by default.  
   - Keep `IOrderAuditSink` isolation.

7. **MC**  
   - `clear()` / reset keeps capacity.  
   - `reuse_objects_between_trials` ⊕ `parallel_trials` remains mutually exclusive.  
   - Synthetic path fills via `IMarketSink`, not private column writes.

8. **Fail modes**  
   - Empty successful load with `fail_if_empty` → hard failure.  
   - Soft row rejects are counted; all-reject after open → failure.  
   - Cache hits must run the **same** validation as live append (or validate once when writing the cache).

---

## 7. Phased Execution Plan

Execute phases **in order**. Each phase ends with the verification gate in §9. Prefer small PRs. Reference commits as `docs/data.md#D-0N`.

### Phase D-00 — Characterization & inventory (read-only / tests only)

**Intent:** Lock current behaviour before moving code.

**Work:**

1. List all construction and mutation sites of `data_handler` (engine, main, api, MC, tests, binary cache).  
2. Add or extend characterization tests for:
   - CSV bar load row counts + validation skip cases  
   - Tick monotonic skip  
   - Engine prefers ticks when both present  
   - Multi-file local load + `sort_by_date` order  
   - MC `reset` clears content but allows reload  
3. Document golden CSV paths used by hotpath tests.  
4. Explicit decision log: wire vs delete `BinaryCacheSource`, `WebSocketDataSource`.

**Done when:** inventory checked into this doc’s appendix or a short `docs/internal/` note; tests green; no production code change required.

**Risk:** Low.

---

### Phase D-01 — Domain types + `IMarketSink` (additive)

**Intent:** Introduce canonical types and sink without breaking callers.

**Work:**

1. Add `market_types.h` (`Bar`, `Tick`, `tick_side`).  
2. Add `market_sink.h` (`IMarketSink`, `LoadStats`).  
3. Map helpers:
   - `bar_record` ↔ `Bar` (date string → `ts` via `date_parse` at boundary)  
   - `tick_record` ↔ `Tick`  
   - provider bar/tick ↔ domain (extend `provider_convert` or adjacent)  
4. Implement `data_handler::on_bar` / `on_tick` **or** a thin `MarketSeriesSink` adapter that calls `load_into_queue` / `add_tick` so old storage remains.  
5. Unit tests for mapping + validation parity.

**Done when:** new headers used by at least one sink path in tests; no behaviour change in default CLI.

**Risk:** Low.

**Do not:** remove public `db_data_*` yet.

---

### Phase D-02 — Encapsulate store read API; migrate engine batch loop

**Intent:** Engine stops depending on public SoA fields.

**Work:**

1. Add `bar_count`, `tick_count`, `bar_at` → `BarView`, `tick_at` on `data_handler` / `MarketSeries`.  
2. Ensure `BarView` is non-allocating.  
3. Change `engine::run` / `run_tick_data` to use read API only.  
4. Change other direct field peeks (symbol front for config, tests) gradually.  
5. Keep public vectors temporarily as deprecated if needed for binary cache / tests — mark clearly.  
6. If `engine.cpp` is edited: follow freeze protocol when required; keep change minimal (iteration only).

**Done when:**

- Engine batch loops compile without reading `db_data_*` / `tick_data` as public fields (friend/tests may still).  
- Golden / integration / hotpath alloc tests pass.  
- No p99 regression claim without benchmark (§9).

**Risk:** Medium (engine touch). Prefer one PR focused only on this.

---

### Phase D-03 — Privatize storage; rename to `MarketSeries`

**Intent:** True store independence.

**Work:**

1. Privatize bar columns and tick vector.  
2. Move `load_from_csv` **out** of the store into a CSV source (or delete if DataBridge path covers it).  
3. Rename class to `MarketSeries` with `using data_handler = MarketSeries` typedef for one release/PR cycle if desired.  
4. Update MC `reset` → `clear` naming carefully (keep behaviour).  
5. Fix binary cache to use append API or a private serialization friend **that re-validates**.  
6. Update tests to use append / load helpers, not column poking (except dedicated layout tests if any).

**Done when:** no production code outside `src/data` writes SoA columns; `load_from_csv` gone from store.

**Risk:** Medium.

---

### Phase D-04 — `IMarketSource` replaces `IDataSource`; migrate DataBridge

**Intent:** Sources depend on sinks, not on `shared_ptr<data_handler>`.

**Work:**

1. Introduce `IMarketSource`.  
2. Adapt `DataBridge<T>`:
   - batch: parse → convert → `sink.on_bar/on_tick`  
   - stream: same + optional retain into series controlled by options  
3. Replace `sink_fn` that captures `data_handler` with domain sinks.  
4. Migrate `CsvDataSource` / `TickCsvDataSource` to `IMarketSource` implementations under `src/data/sources/` (or delete and route all CSV through bridge parsers).  
5. MC synthetic loader becomes a source or direct sink fill helper in simulation layer.  
6. Deprecate `IDataSource`; remove when call sites gone.

**Done when:** `grep IDataSource` clean (or shim only); provider CLI path uses sink-based bridge.

**Risk:** Medium.

---

### Phase D-05 — `DataWrapper` façade + CLI/API mapping

**Intent:** Single programmatic entry for multi-format load.

**Work:**

1. Implement `DataWrapper` factories (`from_paths`, `from_uri` minimal, `from_provider`).  
2. Map `main.inc` ownership:
   - create `MarketSeries`  
   - `DataWrapper(...).load(series)` for batch  
   - stream path: wrapper/provider → engine `run_streaming` with sink adapter  
3. Map `truetest_api` bar CSV to wrapper (behaviour-identical).  
4. Multi-file merge + `sort_bars_by_time` behind options (preserve local multi-path behaviour).  
5. Document CLI flag → wrapper mapping in `docs/reference/01-instructions.md` (short pointer).  
6. Delete dead code paths once equivalent.

**Done when:** headless backtest via wrapper matches prior CSV/provider results on golden inputs.

**Risk:** Medium (CLI surface). Keep flags stable; wrapper is internal composition.

---

### Phase D-06 — Streaming retain policy

**Intent:** Fix unbounded growth on long live/shadow runs.

**Work:**

1. Default `retain_streamed = false` for streaming transports.  
2. When false: stream sink invokes engine processing only; series stays empty or holds last-N if explicitly designed (prefer empty).  
3. When true (debug/backfill): append as today.  
4. Tests for both modes.  
5. Call out behaviour change in changelog / instructions.

**Done when:** long synthetic stream test does not grow series when retain=false; halt wiring unchanged.

**Risk:** Medium (behaviour change) — gate behind option first if needed, then flip default.

---

### Phase D-07 — Dead code decision: BinaryCache + WebSocketDataSource

**Intent:** No half-wired formats.

**BinaryCacheSource — choose one:**

| Option | When |
|--------|------|
| **Wire** as `IMarketSource` decorator or series-level cache behind `DataLoadOptions::cache_path` | If CSV reload time is a measured pain |
| **Delete** | If unused and unloved |

If wired:

- Cache key must include content identity (path + mtime/size or digest), not path alone.  
- On load from cache, run validation or trust only caches written by validating writer.  
- Endian / `size_t` portability documented; prefer fixed-width header fields.  
- Bars first; ticks optional later.

**WebSocketDataSource:**

- Prefer **delete** if venue transports cover live.  
- If kept, reimplement as `IMarketSource::stream_into` and wire a real caller — no orphans.

**Done when:** no unwired production classes in `src/data` without a `// intentionally experimental` + owner comment and build flag.

**Risk:** Low–medium.

---

### Phase D-08 — Parquet source (optional dependency)

**Intent:** Second real format proving the architecture.

**Prerequisites:**

- Human approval for dependency (Arrow/Parquet C++ or minimal parquet reader).  
- CMake option e.g. `ENABLE_PARQUET=OFF` default.  
- Layer check: decoder only under `src/data/sources/`, no engine includes.

**Work:**

1. Define required columns: `ts`, `symbol`, `open`, `high`, `low`, `close`, `volume` (names configurable).  
2. `ParquetBarSource : IMarketSource` reads row groups → `on_bar`.  
3. URI `parquet:` + CLI flag e.g. `--data-format parquet` or path sniff.  
4. Tests with a small fixture file committed or generated in test.  
5. Docs: schema contract + ENABLE flag.

**Done when:** backtest from parquet fixture matches CSV fixture for the same logical series.

**Risk:** Medium (deps, CI size). Keep default builds free of the dependency.

---

### Phase D-09 — Historical API source

**Intent:** HTTP/API history without venue leakage into the store.

**Work:**

1. Prefer implementing history fetch **on the provider** (existing REST clients) exposing a batch transport or a provider-local `IMarketSource`.  
2. Generic `HttpHistorySource` only for **non-venue** CSV/JSON URLs with an explicit column map (research data).  
3. Pagination, rate limits, and failure = hard fail (no silent partial success unless `allow_partial` option).  
4. Never put API keys into URIs logged to disk; use env / config (secret hygiene).

**Done when:** one documented historical path works end-to-end in backtest binary; secrets not logged.

**Risk:** Medium (network flakiness) — tests mock transport.

---

### Phase D-10 — Timestamp & multi-symbol hardening

**Intent:** Make multi-format series trustworthy.

**Work:**

1. Series primary order = `ts` ascending; secondary = symbol.  
2. Remove reliance on string `sort_by_date` for correctness (keep only as migration helper).  
3. Engine `resolve_bar_ts` simplifies to `bar_at(i).ts` (with explicit synthetic stepping only when config demands seed-based synthetic time).  
4. Document policy when both bars and ticks present (today: prefer ticks) — keep unless product changes.  
5. Optional: `symbol_table` / `SymbolId` to cut `std::string` weight on huge series (perf phase; measure first).

**Done when:** mixed-symbol multi-file loads are ordered by time; golden seed paths still reproduce.

**Risk:** Medium (determinism). Heavy characterization required.

---

### Phase D-11 — Cleanup & docs freeze

**Intent:** End state hygiene.

**Work:**

1. Remove typedefs/shims (`data_handler`, `IDataSource`) if migration complete.  
2. Update `docs/reference/01-instructions.md`, `02-user-manual.md` data-flow section, `AGENTS.md` pointers if needed.  
3. Update this document status to **Phase D-11 COMPLETE** with date.  
4. Ensure `cmake/Sources.cmake` lists only live TUs.  
5. Final full test + gate scripts.

**Risk:** Low.

---

## 8. Migration Rules (for every phase)

1. **One concern per PR** (store API vs CLI vs parquet dep).  
2. **No drive-by refactors** outside the phase scope.  
3. **Tests first** when changing validation or ordering.  
4. **Adapter over big-bang** — keep old names as typedefs for one cycle.  
5. **Benchmark only with numbers** if claiming faster loads/iteration.  
6. **Register sources** in `cmake/Sources.cmake`.  
7. After any `src/` edit:  
   `./scripts/check-hotpath-json.sh`  
   `./scripts/check-layer-deps.sh`  
   `./scripts/check-live-safety-freeze.sh`  
8. Prefer not expanding JSON allow-list for loaders (load is cold C++; avoid nlohmann on new paths if possible).

---

## 9. Verification Gate (mandatory per phase)

### 9.1 Always

```bash
# From core/
./scripts/check-hotpath-json.sh
./scripts/check-layer-deps.sh
./scripts/check-live-safety-freeze.sh

cmake --preset linux-tests   # or project-equivalent
cmake --build build -j
ctest --test-dir build --output-on-failure
# or:
./build/truetest_tests
./build/truetest_cli_tests
```

### 9.2 When touching pools / engine loops / series layout

```bash
ctest --test-dir build -R 'hotpath|Hotpath|ObjectPool|Ring|engine|Engine|provider|Provider|data|Data' --output-on-failure
# ASAN if lifetime/layout risk:
cmake --preset linux-asan && cmake --build --preset linux-asan -j
```

### 9.3 Behaviour parity (batch CSV)

```bash
./build/engine_backtest \
  --provider synthetic \
  --strategy sma \
  --seed 424242 \
  --no-pin --status-format off --no-tui \
  --output /tmp/data-redesign-smoke.json
# Plus a fixed CSV golden comparison before/after when migrating loaders
```

### 9.4 MC reuse

- With `--mc-reuse-objects` and compatible thread preset, trials still isolate series content via `clear()`.  
- Parallel ⊕ reuse still rejected.

### 9.5 Definition of done (phase)

- [ ] Phase goals met; no scope creep  
- [ ] Gate scripts green  
- [ ] Tests green (full or justified focus + listed extras)  
- [ ] No R*/S* red-line violations (`AGENTS.md`)  
- [ ] Docs pointers updated if operator-visible behaviour changed  
- [ ] This file’s phase status line updated in the PR description  

---

## 10. Anti-Patterns (Reject in Review)

| Anti-pattern | Why |
|--------------|-----|
| `MarketSeries::load_from_parquet()` | Re-god-objects the store |
| Source writes `db_data_open_value.push_back` | Breaks encapsulation |
| Engine `#include <arrow/...>` | Layer violation; cold decode belongs in source |
| Virtual call per bar without measurement on tight loop | Prefer `BarView` + concrete series |
| String `IDataTransport` for Parquet row groups | Wrong abstraction |
| Silent cache load without validation | Corrupt cache → silent bad trades in research |
| `retain_streamed=true` default on live | Memory death |
| New venue HTTP client inside `src/data` for Binance | Bypasses provider extension point |
| QuestDB as market bar source “for convenience” | Conflates audit egress with ingress |
| Expanding hot-path JSON allow-list for loaders | Loaders are cold; keep allow-list tiny |
| Partial migration that adds a **third** stack | Always delete or adapt the old path in the same phase window |

---

## 11. Testing Strategy

| Layer | What to test |
|-------|----------------|
| Unit | Validation matrix; `Bar` mapping; sort order; `clear` capacity; URI parse |
| Source | CSV fixtures; mocked transport frames; parquet fixture (when enabled) |
| Wrapper | Multi-path merge; empty fail; symbol filter; options |
| Engine integration | Existing integration + golden; tick preference; streaming halt |
| Hotpath | Alloc tests still pass on batch run after read-API migration |
| MC | Reuse path fills identical bars; seed stability |
| Negative | Bad schema, truncated file, non-monotonic ticks policy |

Prefer deterministic fixtures under `tests/data/` (or existing golden paths). Do not require network in unit tests.

---

## 12. Performance Notes

- **Batch bars:** keep SoA; `reserve` from options or file metadata.  
- **Load:** columnar Parquet decode should write SoA columns via `on_bar` (simple v1); optimize later if profiling demands bulk insert API (`append_bars(span<Bar>)`) — still through the store, not from engine.  
- **Stream:** zero retain default; avoid per-message `std::string` symbol churn where providers can intern symbols.  
- **Claims:** only with `truetest_benchmarks` / measured load timers — no mean-only bragging (`AGENTS.md` R9).

---

## 13. Dependency & Build Policy

| Feature | Default | Gate |
|---------|---------|------|
| Core series + CSV sources | always on | existing build |
| Parquet | `OFF` | human-approved dep + `ENABLE_PARQUET` |
| HTTP history helper | optional | no secrets in logs; mockable |
| QuestDB | existing `ENABLE_QUESTDB` | unchanged role |

New files → `cmake/Sources.cmake`. Optional sources wrapped in target conditionals consistent with existing `HAS_*` / `ENABLE_*` patterns **without** leaking into engine headers.

---

## 14. Suggested PR / Wave Slice (summary)

| Wave | Phases | Approx. focus |
|------|--------|----------------|
| W0 | D-00 | Characterization |
| W1 | D-01 | Types + sink (additive) |
| W2 | D-02 | Engine read API |
| W3 | D-03 | Privatize / MarketSeries |
| W4 | D-04–D-05 | Sources + DataWrapper + CLI map |
| W5 | D-06–D-07 | Stream retain + dead code |
| W6 | D-08–D-09 | Parquet + API (optional, can split) |
| W7 | D-10–D-11 | Timestamps + cleanup |

Parallelization: D-08/D-09 can proceed after D-05 on separate branches if D-04 sink API is stable. Do not parallelize D-02 with large engine decomp waves without coordination (`docs/engine.md`).

---

## 15. Open Decisions (resolve during D-00 / D-07)

| # | Decision | Resolution (2026-07-31) |
|---|----------|-------------------------|
| 1 | Binary cache | **Deleted** (unwired; validation-bypass on hit) |
| 2 | WebSocketDataSource | **Deleted** (unwired; venue transports supersede) |
| 3 | Tick preference when both loaded | **Keep** (engine prefers ticks) |
| 4 | Typedef duration for `data_handler` | Keep `using data_handler = MarketSeries` one cycle |
| 5 | Parquet library | **Deferred** — human approval required (D-08) |
| 6 | `from_provider` layer placement | File/URI façade in `src/data`; provider glue stays in providers/bin |
| 7 | API tick/stream | Still bar CSV via DataWrapper; tick/stream deferred |
| 8 | Stream retain default | **false** (D-06); `set_retain_streamed(true)` for backfill |
| 9 | Domain side enum name | Keep `data_tick_side` (avoids clash with `core/event.h::tick_side`) |

Further decisions belong in the completing PR body.

## 16. Appendix A — Current coupling map (starting point)

```
data_handler
  ├─ written by: CsvDataSource, TickCsvDataSource, DataBridge sinks,
  │              BinaryCacheSource, MC load_into_queue, tests
  ├─ read by:    engine::run / run_tick_data / run_streaming (append),
  │              main.inc counts, api wrapper state
  └─ owns I/O:   load_from_csv (should move out)

IDataSource
  └─ Csv*, Tick*, BinaryCache*, DataBridge

DataBridge
  └─ main.inc provider mode; engine run_streaming*

QuestDB (orthogonal)
  └─ IOrderAuditSink only
```

Re-run a repo-wide search at start of each phase; this appendix goes stale quickly.

---

## 17. Appendix B — Minimal “first productive PR” recipe

If only one PR can land first after D-00:

1. Add `Bar` / `Tick` / `IMarketSink`.  
2. Adapter: `MarketSeriesSink` → existing `load_into_queue` / `add_tick`.  
3. One CSV path writes **only** through the sink in a test.  
4. No engine change yet.  
5. Gates green.

This proves the hinge without freeze risk.

---

## 18. Appendix C — Success criteria (program complete)

The redesign is **done** when:

1. Adding a new format requires **only** a new `IMarketSource` (+ factory registration + tests), not edits to `MarketSeries` internals or engine loops.  
2. `MarketSeries` has no format-specific methods.  
3. Engine batch path uses `bar_at` / `tick_at` (or equivalent) only.  
4. CSV and at least one non-CSV format (Parquet **or** HTTP history **or** provider batch) load through `DataWrapper`.  
5. Streaming default does not unbounded-retain.  
6. No unwired sources remain.  
7. Gate scripts + full tests green; docs updated.

---

*Last updated: 2026-07-31 — initial redesign instructions from multi-lens data pipeline analysis. Pair with `AGENTS.md` and `docs/engine.md` when touching engine feed loops.*
