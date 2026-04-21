# Deepdive Implementation TODO

Implements `docs/target-architecture.md` against the existing `hft-engine` codebase.
Each phase assumes the matching section of `prerequisites.md` is complete. Every
top-level item begins with a suggested Claude Code prompt; sub-bullets are
acceptance criteria the agent should satisfy.

> **Ordering rule:** do *not* start a phase until the previous phase is green in
> CI. Phases 0–3 harden the foundation; 4–7 build the trading path; 8–11 layer
> on analytics and operations.

---

## Phase 0 — Finish prerequisites

- [ ] Execute every action in `prerequisites.md` §§1–11. Nothing in Phase 1 is
  safe until: clean-slate builds pass on C++20, the directory graph (§10) is
  enforced by CMake `OBJECT` libraries, the dep catalog (§11) is unified, and
  the branch discipline (§9) is in place.
  - Claude Code: `Work through prerequisites.md top-to-bottom. Commit each section as its own atomic change. Stop and ask before starting Phase 1.`

### Phase 0 gating checklist (must all be true before Phase 1 starts)

- [ ] `master` tagged `pre-deepdive-<date>`; rollback point verified.
- [ ] `phase/01-build` branch created off current `pre_transform`.
- [ ] Branch-protection rules active on `master` and `pre_transform`.
- [ ] CI matrix green on `pre_transform` for: default, Binance, PG, ASAN,
      UBSAN. Nightly TSAN scheduled.
- [ ] `src/<module>/` layout landed; `tt_<module>` OBJECT libraries with
      explicit PUBLIC deps; sibling-include linter active.
- [ ] simdjson, prometheus-cpp FetchContent declarations present but gated;
      LibTorch documented in `docs/build.md` as a `find_package` dep.
- [ ] `nlohmann::json` usage confined to config loader + C API + tests
      (grep check in CI).
- [ ] Credential-file pre-commit hook rejecting `*.pem`, `.env`, `live*.toml`.

---

## Phase 1 — Project structure & build (deepdive §1)

- [ ] **Reshape the source tree to match §1.1.**
  - Claude Code: `Move BacktestEngine/src → src. Create src/{core,network,execution,risk,strategy,db,logging,ml}. Relocate existing files into the most appropriate bucket. Split current core/engine.* so that types-only code lives in src/core and orchestration lives in src/engine.`
  - `src/core/` compiles without any dep on sibling directories.
  - All tests still pass.
- [ ] **Wire three-target build (§1.3).** `engine_backtest`, `engine_shadow`,
  `engine_live` from a shared `engine_core` OBJECT library. Live is the only
  target that links against the REST credential store.
- [ ] **Add PGO pipeline (§1.2).** Two-stage build: `-fprofile-generate` →
  shadow run → `-fprofile-use`. Wrap behind `ENABLE_PGO=ON`.
  - Claude Code: `Add a CMake option ENABLE_PGO that, when set, produces -fprofile-generate for stage 1 and consumes .gcda files for stage 2. Document the workflow in docs/pgo.md.`
- [ ] **Add `config/{backtest,shadow,live}.toml`** loaded via a single
  `load_config()` function. Live config must never be committed; enforce via
  `.gitignore` and a pre-commit hook.

## Phase 2 — Memory architecture (deepdive §2)

- [ ] **Replace the current object pool with the deepdive's `PoolAllocator<T,
  Capacity>`** (§2.2). The new pool is intentionally single-threaded; document
  the ownership contract at the call site.
  - Claude Code: `Replace src/types/object_pool.h with the template PoolAllocator<T, Capacity> from deepdive §2.2. Migrate every caller (Order, Fill, Event allocations) to it. Add a unit test that exhausts the pool and asserts nullptr.`
- [ ] **Cache-line-align hot structs (§2.3).** `Order`, `Fill`, `Tick`,
  `MarketState`, and every queue head/tail pair. Add `static_assert(sizeof)`
  and `static_assert(alignof)` guards so drift is a compile error.
- [ ] **Reserve huge pages and wire `alloc_huge()` (§2.4).** Add `MAP_HUGETLB`
  path with a graceful fallback. Update `docs/deployment.md` with
  `vm.nr_hugepages = 512`.
- [ ] **Backtest data loader uses `mmap` (§2.5).** Replace the current CSV
  loader's `std::getline` loop with a `MmapTickReader` + zero-copy field
  tokenizer.
  - Claude Code: `Implement src/backtest/mmap_tick_reader.{h,cpp} per deepdive §2.5 and retrofit csv_data_source.cpp to use it. Benchmark before/after with Google Benchmark; log the numbers in docs/benchmarks.md.`

## Phase 3 — Networking layer (deepdive §3)

- [ ] **Introduce `EpollReactor` (§3.2)** under `src/network/`. Everything
  that currently uses Boost.Beast's `ioc.run()` should route through this
  reactor instead. Pinned to a single thread.
- [ ] **Apply the §3.3 socket tuning** in a shared
  `configure_trading_socket(fd)` used by both market-data WS and order REST
  sockets.
- [ ] **Adopt simdjson on the market-data hot path (§3.4).** Replace any
  remaining `nlohmann::json` parsing inside `providers/binance/*_parser.h`.
  Persistent `simdjson::ondemand::parser` as a class member; never per-message.
  - Claude Code: `Add simdjson via FetchContent. Rewrite BinanceParser and BinanceDepthParser to use ondemand parsing. Keep the hand-rolled snprintf-based serializers untouched.`
- [ ] **Decide Bitstamp vs Binance** per `prerequisites.md`. If Bitstamp: add
  `src/providers/bitstamp/{transport,parser,executor}` matching the Binance
  provider's shape. If Binance stays: document the deepdive's Bitstamp
  references as exchange-agnostic pseudocode.

## Phase 4 — Order lifecycle & execution (deepdive §4)

- [ ] **Audit `threading/ring_buffer.h` against §4.1.** Fix memory orders,
  add the `alignas(64)` padding on head/tail, enforce power-of-two capacity,
  add a stress test.
- [ ] **Implement `configure_execution_thread()` (§4.2).** `SCHED_FIFO` +
  `sched_setaffinity` + `mlockall`. Gate behind `--rt` flag; fail loud if
  `rtprio 99` limit is not granted.
  - Claude Code: `Add src/execution/thread_tuning.{h,cpp} implementing configure_execution_thread() per deepdive §4.2. Call it from the execution worker entry point only when --rt is passed. Document /etc/security/limits.conf requirements.`
- [ ] **Add vDSO clocks + `rdtsc` helpers (§4.3)** at `src/core/clock.h`.
  Replace every `std::chrono::system_clock::now()` on hot paths.
  - Check `/proc/cpuinfo` for `constant_tsc` at startup; abort if missing on
    live binary.

## Phase 5 — Risk management (deepdive §5)

- [ ] **Rewrite `risk/risk_manager.cpp` to match §5.1**: const-correct,
  branchless-where-possible, `__builtin_expect(…, 0)` on every rejection.
  Preserve the existing halt-flag integration with workers.
- [ ] **Add `CircuitBreaker` (§5.2)** under `src/risk/`. Monitors drawdown,
  error rate, and RTT. Trips the global kill switch via `std::atomic<bool>`.
- [ ] **Wire kill-switch propagation to every worker.** Today the halt flag
  halts the engine loop; it must also block the order queue drain in
  `execution/` and suppress any pending sends.

## Phase 6 — Strategy layer (deepdive §6)

- [ ] **Redefine `IStrategy` to match `StrategyBase` (§6.1).** Methods:
  `on_tick`, `on_fill`, `on_order_update`, `on_timer`, `name/version/description`.
  Strategies return `OrderIntent` objects — they never build `Order` objects
  themselves.
  - Claude Code: `Refactor strategy/strategy_interface.h to match deepdive §6.1. Introduce OrderIntent. Update sma, mean_reversion, ma_crossover strategies to return OrderIntents. Add an IntentTranslator in execution/ that converts intents to pool-allocated orders after risk checks.`
- [ ] **Add `ConfigManager` (§6.2)** with `atomic<shared_ptr<Config>>` and
  `inotify`-driven hot-reload. Wire into each strategy as a dependency.

## Phase 7 — Database layer (deepdive §7)

- [ ] **Author `db/schema.sql`** exactly per §7.2 (ticks, orders, fills,
  pnl_snapshots, system_events + hypertables + continuous aggregates +
  retention/compression policies).
- [ ] **Implement `AsyncDBWriter` (§7.3)** under `src/db/`. Replaces the
  existing `sqlite_store.*` in the live build. SQLite remains available behind
  `ENABLE_SQLITE` for tests.
  - Claude Code: `Implement db/db_writer.{h,cpp} using libpqxx/libpq COPY protocol batched flushes per deepdive §7.3. Route tick, order update, fill, pnl snapshot, and system event records through a single SPSC queue of 65536 slots. Benchmark sustained insert rate.`
- [ ] **Add schema migration runner** (`tools/migrate.cpp`) that applies
  `schema.sql` idempotently on startup.

## Phase 8 — Structured logging (deepdive §8)

- [ ] **Implement `RingBufferLogger` (§8.1).** 264-byte fixed `LogRecord`;
  `AUDIT` level bypasses the ring buffer for synchronous durability.
  - Claude Code: `Create src/logging/logger.{h,cpp} per deepdive §8.1. Replace every existing log call site across the codebase. The drain thread writes NDJSON to a rotating file. Add a unit test for correctness under high contention.`
- [ ] **Thread correlation IDs end-to-end (§8.2).** Assign at strategy signal
  generation; include in every downstream log record and in the `orders` and
  `fills` DB rows.

## Phase 9 — Performance engineering (deepdive §9)

- [ ] **Document `isolcpus`/`nohz_full`/`rcu_nocbs` kernel params (§9.1)** in
  `docs/deployment.md`. Add a startup check that warns if the chosen pinned
  cores are not in `isolcpus`.
- [ ] **Set `performance` CPU governor (§9.2)** via a startup helper script.
- [ ] **Add `LatencyHistogram` (§9.3)** with `rdtsc` brackets around:
  (a) strategy `on_tick`, (b) risk check, (c) order serialization,
  (d) socket write, (e) full signal→ack round-trip. Dump p50/p99/p999 on
  shutdown.
- [ ] **Produce a flamegraph (§9.4)** from a 30-minute shadow run; commit the
  SVG to `docs/profiling/` and record optimization targets.

## Phase 10 — Post-trade DL pipeline (deepdive §10)

- [ ] **Add LibTorch behind `ENABLE_ML=ON` (§10.2).** Only linked into a new
  `engine_ml` binary — never into `engine_live`.
- [ ] **Implement `FeatureExtractor` (§10.3)** pulling from TimescaleDB.
  Parameterized queries only; integration test with a fixture DB.
- [ ] **Implement the TCN model (§10.4)** at
  `src/ml/model.{h,cpp}`. Stack 6 blocks with dilation 1,2,4,8,16,32.
- [ ] **Implement `walk_forward_validate` (§10.5).** No random splits allowed.
  Report Sharpe / max DD / total return per window.
- [ ] **Implement `ModelParameterBridge` (§10.6)** — `atomic<shared_ptr>` of
  derived params read by the live strategy. Staleness check: strategy rejects
  params older than N minutes.

## Phase 11 — Monitoring & observability (deepdive §11)

- [ ] **Expose `/metrics` Prometheus endpoint.** Histograms for latencies;
  gauges for PnL, open orders, WS reconnects; counters for fills and rejects.
  - Claude Code: `Add prometheus-cpp via FetchContent and expose :9100/metrics from engine_shadow and engine_live. Wire metric updates next to every existing log-point.`
- [ ] **Commit a Grafana dashboard JSON** under `monitoring/grafana/` with
  the three-row layout from §11.2.
- [ ] **Author alert rules** for 50%-of-session drawdown, p99 latency > 10ms,
  and 10-second WS silence. Store in `monitoring/alerts.yml`.

## Phase 12 — Operations (deepdive §12)

- [ ] **Add `deploy/hft-engine.service`** systemd unit (§12.1). Include
  `LimitMEMLOCK=infinity`, `LimitRTPRIO=99`, `TimeoutStopSec=30`.
- [ ] **Implement `graceful_shutdown()` (§12.2).** Six-step sequence, AUDIT
  log bookends. Hook to `SIGTERM`.
  - Claude Code: `Implement the graceful_shutdown sequence from deepdive §12.2 in src/engine/shutdown.{h,cpp}. Wire SIGTERM and SIGINT handlers. Integration test: spawn engine_shadow, send SIGTERM, assert all orders cancelled and DB drained.`
- [ ] **ASAN+UBSAN run on every PR (§12.3).** TSAN weekly nightly. Never
  profile under sanitizers — explicitly document this in CONTRIBUTING.md.

---

## Exit criteria for the whole deepdive rollout

- `engine_backtest` replays a full day of BTC/USD ticks with deterministic
  output across three runs.
- `engine_shadow` runs for 24 hours with zero crashes, zero memory growth,
  and zero lost log records.
- p99 of the software path (strategy tick → socket write) is under 100µs on
  the target hardware (verified via `LatencyHistogram` and flamegraph).
- Every circuit breaker trip is reproducible from the TimescaleDB
  `system_events` table plus the NDJSON log using a single correlation ID.
- The DL pipeline produces updated `ModelDerivedParams` weekly, and shadow
  performance under those params beats the baseline by a documented margin in
  walk-forward validation.
