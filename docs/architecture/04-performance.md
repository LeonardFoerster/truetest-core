# TrueTest Performance & Technical Capacities

**Status**: Authoritative technical reference for performance characteristics, limits, and tuning. Complements `docs/reference/01-instructions.md`, `docs/reference/02-user-manual.md`, `summary.md`, and the benchmark suite.

This document describes the concrete technical capacities of the engine: threading model, memory allocation discipline, concurrency primitives, throughput expectations, latency characteristics, and hard limits.

---

## 1. Core Performance Philosophy

The engine is built for **deterministic, low-jitter, allocation-free** processing of market events on the critical path:

- **Zero heap allocations on the hot path** (enforced via `ObjectPool`, pre-warming, `forbid_runtime_grow`).
- **No `nlohmann/json` on the hot path** (CI-enforced by `scripts/check-hotpath-json.sh`).
- **Lock-free SPSC-only** inter-thread communication.
- **Single-producer** rings (engine event loop is the sole producer).
- **CPU affinity + isolated hot path** (Core 0 / dedicated event thread by default).
- **Pre-warmed bounded resources** before the first market event.

Non-critical work (logging, analytics snapshots, QuestDB ILP, TUI, market-maker replenishment) is always offloaded.

---

## 2. Threading Model & Presets

### Thread Presets (`--thread-preset`)

Auto-selected from detected physical cores (see `src/threading/thread_preset.h` and `thread_config.h`):

| Preset      | Physical Cores | Workers                          | Description                              | Hot Path Location |
|-------------|----------------|----------------------------------|------------------------------------------|-------------------|
| `inline`    | 1–2            | none (single-threaded)           | Minimal; everything on event loop        | Main thread       |
| `light`     | 3              | ObserverWorker (combined)        | Light observability                      | Core 0            |
| `standard`  | 4–5            | LoggingWorker + RiskStatsWorker  | Balanced default for most backtests      | Core 0            |
| `full`      | 6–7            | Logging + Risk + Stats           | Separate risk & stats                    | Core 0            |
| `extended`  | 8+             | + MarketMakerWorker              | Full offload (incl. MM replenishment)    | Core 0            |

- Event loop thread is pinned first (configurable via `pin_event_loop`).
- Worker threads receive subsequent cores.
- Use `--no-pin` to disable affinity entirely (useful under containers / noisy neighbors).
- Spin policy (`--spin-policy`): `adaptive` (default), `spin`, `yield`.

`preset_worker_count()`, `preset_has_separate_risk()`, etc. helpers exist for introspection.

### Rings & Worker Handoff

- All rings are `RingBuffer<event_pointer, N, Policy>` (power-of-2 size).
- Default: **65536 slots** (`DEFAULT_RING_SIZE`, `ring_buffer_capacity` in config).
- Configurable per engine: `engine_config.ring_buffer_capacity`.
- Policies:
  - `SpinWait` (default for most): busy-spin.
  - `DropOldest`: drop oldest on full + count drops (used for some non-critical).
  - `AssertFull`: throw on overflow.
- Per-ring diagnostics (high watermark, drop count) exposed in snapshots and debug reports.
- Safety rings (`risk`, `observer`, `risk_stats`) use `ring_drop_policy::halt_on_drop` in shadow/live modes.
- Worker threads drain their inbound ring with adaptive backoff (`_mm_pause` / `yield`).

`WorkerWatchdog` monitors critical heartbeat sources (e.g. futures DMS) with 3× heartbeat timeout → terminal halt.

---

## 3. Memory & Allocation Model

### Primary Object Pools (event types)

Implemented in `src/types/object_pool.h` (`BlockSize = 4096` slots per block by default):

- `market_pool_`, `tick_pool_`, `order_pool_`, `fill_pool_`
- `l2_update_pool_`, `l2_snapshot_pool_`
- `rejection_pool_`, `cancel_pool_`, `amend_pool_`, `funding_pool_`

Each event acquires a slot via placement-new. Its final `shared_ptr` drop
returns the slot directly to a multi-producer free stack; a small atomic gate
serializes stack pops, so worker-side returns neither wait for a later engine
drain nor reserve a per-pool deferred-return buffer.

**Pre-warm settings** (`pool_prewarm_settings` in `engine_config` and `prewarm_object_pools()`):

Default pre-warm (conservative for most workloads):
- market/tick/fill/l2_snapshot/rejection/... : 1 block
- order / l2_update / amend: 2 blocks
- orderbook_order_blocks: **128** blocks (~524k order slots)

The orderbook default is intentionally large and can dominate RAM when many
books exist. It lives on the frozen engine-config surface; reducing it needs
the CCB/T3 safety process rather than an ad-hoc low-memory tweak.

`forbid_runtime_grow = true` (default): exhaustion → `pool_exhausted` exception → halt (terminal in safety modes).

Control blocks for `shared_ptr<T>` are also pooled (`ControlBlockPool`, 64-byte slots, 4096 per block) and wired into the engine event pools. The rebound allocator owns the control-block backing state, while each live object deleter owns its object-pool state. This keeps escaped strong/weak handles valid across facade/engine teardown: a strong handle retains its object storage through its final drop; a weak handle retains only the control-block backing through its own final drop.

Pooling is guarded by the actual rebound control-block size and alignment. A non-fitting implementation type may use the tracked standard-allocator fallback before the pool is frozen; once `forbid_runtime_grow` is enabled, that fallback fails closed with `pool_exhausted` rather than allocating on the event path. The pool-level regression covers zero fallback allocations for its representative event type; engine-wide fallback accounting is a separate observability follow-up.

### Orderbook Node Pool

Separate pool inside `orderbook` (`src/orderbook/orderbook.h`):

- `NODE_BLOCK_SIZE = 4096`
- Intrusive free-list of `order_node`
- Wired via `configure_order_pool()` / `set_order_pool_config()` during engine construction.
- Bodies pooled; control blocks currently fall back to heap in some builds (documented trade-off).

### Global Limits

- `symbol_table`: `kMaxSymbols = 256`
- L2 snapshots/updates: `kL2SnapshotMaxLevels = 20` (Binance depth20 paths)
- WebSocket UI clients (when `ENABLE_WEB`): `max_ws_clients = 16`
- Concurrent MC trials limited by memory + preset constraints (parallel mode strongly recommends `inline` preset)

### Checkpoint & Logging

- Binary portfolio checkpoint: default every **10 000** events
- Event log index interval: **1 000** events

---

## 4. Orderbook

`src/orderbook/orderbook.{h,cpp}` implements price-time priority matching:

- Sorted vectors of price levels (`bid_levels_`, `ask_levels_`).
- Per-level intrusive linked list of orders via pooled nodes.
- `add_order`, `cancel_order`, `modify_order`, match paths are allocation-light.
- Fast L2 ingest paths: `apply_l2_snapshot` / `apply_l2_update`.
- `MarketMaker` seeds synthetic depth around mid for hybrid/paper execution.
- Separate order pool from event pools (see above).

Benchmarks (`benchmarks/bench_main.cpp`):
- `BM_Orderbook_InsertCancel`
- `BM_Orderbook_Match`

---

## 5. Throughput Characteristics

**Design target**: Full Binance spot/futures market data (trade + depth20@100ms) + strategy evaluation + risk + analytics + offload on a modern 6–16 core desktop/laptop.

**Measurement tools**:
- Google Benchmark suite (`-DENABLE_BENCHMARKS=ON`):
  ```bash
  cmake -B build -DENABLE_BENCHMARKS=ON
  cmake --build build --target truetest_benchmarks
  ./build/truetest_benchmarks
  ./build/truetest_benchmarks --benchmark_filter=Orderbook
  ```
- Built-in engine throughput benchmark (`BM_Engine_Throughput_100k`): 100 000 synthetic bars, reports bars/sec.
- Hot-path allocation tests (`tests/test_hotpath_*.cpp`) enforce upper bounds on post-warmup allocations.
- `ENABLE_DEBUG` + `StageTimer` / ring stats for per-run µs breakdowns and occupancy.

**Typical observed (architecture + instrumentation)**:
- Event loop processes bars/ticks at rates far above retail market data volumes.
- Ring occupancy normally stays well below 65536 under normal load; high-watermark and drop counters available for diagnosis.
- Worker handoff median latency: sub-millisecond on pinned cores (subject to hardware + spin policy).

Monte Carlo campaigns benefit from `--mc-reuse-objects` (object reuse across trials) and caution around `--mc-parallel` (pinning conflicts).

---

## 6. Latency Model & Realism Overhead

Configurable realism (ignored or rejected in `--mode live`):

- `ILatencyModel`: strategy-to-eligible delay + optional wire latency.
- `IImpactModel` + `walked_book_impact`.
- `IQueuePositionModel` / `IQueueModel` (requires depth stream): Front/Uniform/Back cancel heuristics.
- `IFillModel`: probabilistic partial fills.
- Bar-mode spread (`--bar-spread-bps`) and realistic resting-price fills.

These add deterministic work to the event path but remain allocation-free and pool-backed.

Live mode uses real exchange timestamps + user-data stream as truth; latency models do not apply.

---

## 7. Persistence & I/O Impact

### Binary Event Log (`--log-events`, zstd)
- Written synchronously from the event loop (or via logging worker).
- Low overhead; index every 1000 events for fast seeking.
- Authoritative replay source (`--replay`).

### QuestDB ILP (opt-in, `ENABLE_QUESTDB`)
- Direct TCP ILP (port 9009) via `IlpWriter` background thread.
- HTTP DDL (port 9000) for schema.
- Batched; `questdb_flush_cadence` default **150 ms**.
- All `record_*` calls are mutex-protected inside `QuestdbStore` but handoff to background writer is non-blocking.
- Soft-fail by default (warnings); `questdb_strict` available.
- Per-run tables + `runs_meta`; excellent for long campaigns (PARTITION BY DAY/WEEK).

QuestDB writes never block the hot path under normal conditions.

---

## 8. Build & Runtime Tuning for Performance

Key CMake flags:
- `-DENABLE_NATIVE_OPT=ON` → `-march=native` + unrolling on all three engines (Release; portable CI keeps it OFF).
- `-DENABLE_DEBUG=ON` → StageTimer, ring stats, memory/copy trackers, thread utilization (Abseil-based).
- `-DENABLE_BENCHMARKS=ON` → Google Benchmark.
- Release build (`-DCMAKE_BUILD_TYPE=Release`) + LTO where supported.
- Sanitizers mutually exclusive with peak perf.

Runtime:
- `--thread-preset`, `--spin-policy`, `--no-pin`
- `--status-format ndjson|off` for headless high-volume runs.
- Pre-warm tuning via JSON config or future flags (increase `orderbook_order_blocks` for heavy MM or L2 workloads).
- `--questdb-flush-ms` for very long quiet periods.

---

## 9. Hard Limits & Recommendations

| Resource                        | Limit / Default                          | Notes |
|---------------------------------|------------------------------------------|-------|
| Ring capacity                   | 65536 slots (configurable)               | Per ring. Drop or halt policy applies. |
| Object pool block               | 4096 slots                               | Grows in blocks; pre-warm + forbid grow recommended. |
| Symbols                         | 256                                      | `kMaxSymbols` |
| L2 depth                        | 20 levels per side                       | Matches Binance depth20 |
| Web UI WS clients               | 16                                       | Read-only |
| Checkpoint interval             | 10 000 events                            | Configurable |
| QuestDB flush cadence           | 150 ms                                   | Time-based tick |
| DMS heartbeat / countdown       | 8–10 s / 30 s (typical)                  | Configurable per run |
| Kill-switch deadline            | 5 000 ms default                         | Hard deadline for orderly flatten |
| Reconcile tolerance             | 10 bps default                           | Refusal > tolerance |

**Recommendations**:
- Always run with `forbid_runtime_grow=true` + sufficient pre-warm for production/shadow/live.
- Prefer `standard` or `full` preset on 6+ core machines for shadow/live.
- Use depth stream (`depth20@100ms`) + appropriate queue/impact models only when needed (adds measurable work).
- Monitor ring high-watermark and pool grow counts via dashboard / ndjson / QuestDB.
- For MC: `--mc-reuse-objects` + `inline` preset when using parallel.
- Binary logs + QuestDB together give the best audit + analytics surface.

---

## 10. Instrumentation & Verification

- **Benchmarks**: See `benchmarks/bench_main.cpp` header.
- **Hot-path allocation tests**: `tests/test_hotpath_allocs.cpp`, `test_hotpath_pool_prewarm.cpp`, `test_hotpath_alloc_matrix.cpp`.
- **Debug reports**: `ENABLE_DEBUG` + `--status-format` or post-run debug dumps.
- **Ring/pool snapshots**: `dashboard_snapshot` + TUI "Debug" / "Health" tabs.
- **Scripts**: `scripts/check-hotpath-json.sh`, allocation re-baselining via `TRUETEST_REBASELINE_ALLOCS=1`.

To re-verify capacities after changes:
```bash
cmake -B build -DENABLE_BENCHMARKS=ON -DENABLE_DEBUG=ON -DBUILD_TESTS=ON
cmake --build build -j1
ctest --test-dir build -j1 -R Hotpath
./build/truetest_benchmarks
```

---

## 11. Future / Aspirational

Areas tracked in `todo.md` and `prod.md` that affect capacity:
- Further reduction of control-block heap traffic for orderbook orders.
- Vectorized / batched indicator paths.
- NUMA / multi-socket awareness.
- Higher-resolution StageTimer integration in release builds.

Current design already provides strong, measurable, and bounded capacities suitable for serious personal research, high-fidelity backtesting, shadow divergence analysis, and gated tiny-size live execution.

---

**Last updated**: 2026 (synthesized from AGENTS.md, engine sources, threading, object pools, orderbook, config, benchmarks, hot-path tests, and user-manual).

Cross-references: `docs/reference/01-instructions.md` (threading + CLI), `docs/reference/02-user-manual.md` (high-level), `src/engine/engine_config.h`, `src/threading/`, `src/types/object_pool.h`, `benchmarks/bench_main.cpp`.
