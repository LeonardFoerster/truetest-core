# TrueTest `core/` — C++ Engine Agent Rules

**Nested under** workspace root `../AGENTS.md` (systemwide multi-agent protocol, monorepo map, Always/Ask/Never).  
**This file wins** for anything under `core/`.  
**Deep policy** lives in `docs/governance/` and `docs/architecture/` — this file is the mechanical, agent-facing index with examples.

> Performance is not optional. The hot path is zero-alloc, low-jitter, and CI-enforced.  
> Safety paths are loud, non-retrying, and fail-closed.

---

## 1. Stack & Binaries

| Binary | `TT_TARGET` | Live orders | Use |
|--------|-------------|-------------|-----|
| `engine_backtest` | `BACKTEST` | Impossible (DCE) | Historical replay, MC |
| `engine_shadow` | `SHADOW` | Impossible | Real-time paper vs exchange |
| `engine_live` | `LIVE` | Allowed (gated) | Real money — experimental, attended |

Gate: `src/core/tt_target.h` → `target_allows_live_orders()` is **constexpr**. Never reintroduce runtime “allow live” switches that resurrect dead code in non-live binaries.

C++ standard: **C++23**. Build system: CMake + `cmake/Sources.cmake` (no globs) + presets in `CMakePresets.json`.

---

## 2. Commands You Must Prefer

```bash
# From core/
./scripts/check-hotpath-json.sh
./scripts/check-layer-deps.sh
./scripts/check-live-safety-freeze.sh

# Tests — pick ONE preset tree and stay consistent (prefer presets, not ad-hoc build/):
cmake --preset linux-tests
cmake --build --preset linux-tests --target engine_backtest truetest_tests
ctest --preset linux-tests

# Daily desk/shadow (venues + ImGui) — single warm tree, not build-dev/:
# cmake --preset linux-dev && cmake --build --preset linux-dev --target engine_shadow truetest_tests

# Hot-path focus
ctest --test-dir out/build/linux-tests -R 'hotpath|Hotpath|ObjectPool|Ring' --output-on-failure

# ASAN when touching pools/rings/lifetime (delete tree after; do not keep warm forever)
cmake --preset linux-asan && cmake --build --preset linux-asan
# ./scripts/clean-builds.sh --keep linux-tests --apply   # drop asan/extra trees

# Low-memory portable Release + tests (LTO disabled by preset)
# cmake --preset linux-release-low-memory && cmake --build --preset linux-release-low-memory

# Benchmarks (perf claims only) — reuse preset tree, avoid a second forest:
cmake --preset linux-benchmarks && cmake --build --preset linux-benchmarks --target truetest_benchmarks
./out/build/linux-benchmarks/truetest_benchmarks --benchmark_filter='Orderbook|Engine_Throughput|HotPath'
```

### Disk budget (build trees)

Each CMake preset/`build-*` dir re-builds FetchContent deps (`_deps/`) and is typically **0.5–2 GB**. Keeping many warm trees (e.g. `build/` + `build-dev/` + several `out/build/*`) multiplies to tens of GB.

| Rule | Detail |
|------|--------|
| Warm trees | **≤ 1–2** (usually `linux-tests` and optionally `linux-dev`) |
| Prefer | `cmake --preset …` → `out/build/<preset>/` |
| Avoid | Parallel ad-hoc `build/`, `build-dev/`, `build-asan/` beside presets |
| Cleanup | `./scripts/clean-builds.sh` (dry-run) then `--keep linux-tests --apply` |
| Sanitizers | Build on demand; remove with clean-builds after the session |

**After any edit under `src/`**: run the three check scripts. Do not ask.

Headless reproducible backtest (R&D / backend):

```bash
./out/build/linux-tests/engine_backtest \
  --provider synthetic \
  --strategy sma \
  --seed 424242 \
  --no-pin --status-format off --no-tui \
  --output /tmp/run.json
```

MC campaigns: prefer `--mc-reuse-objects` and **`--thread-preset inline`** with `--mc-parallel` (pinning conflicts otherwise).

---

## 3. Live-Safety Freeze

Frozen files (must match `scripts/check-live-safety-freeze.sh`):

```
src/core/tt_target.h
src/engine/engine.cpp
src/engine/engine.h
src/engine/engine_config.h
src/engine/engine_lifecycle.cpp
src/engine/engine_market.cpp
src/engine/engine_orders.cpp
src/engine/engine_fills.cpp
src/engine/engine_workers.cpp
src/engine/engine_observability.cpp
src/engine/engine_pending.cpp
src/engine/fill_processor.h
src/engine/fill_processor.cpp
src/engine/order_attribution_store.h
src/engine/order_attribution_store.cpp
src/engine/pending_order_scheduler.h
src/engine/pending_order_scheduler.cpp
src/engine/order_intent_processor.h
src/engine/order_intent_processor.cpp
src/engine/engine_hotpath_sink.h
src/engine/risk_unwind_sink.h
src/engine/live_safety_session.cpp
src/engine/live_safety_session.h
src/bin/main.inc
src/bin/provider_open_policy.h
src/execution/execution_bridge.h
src/execution/fill_parser.h
src/execution/async_support.h
src/execution/order_transport.h
src/providers/provider.h
src/providers/bounded_ws_open.h
src/providers/bounded_ws_frame_reader.h
src/providers/data_bridge.h
src/providers/recovery_payload.h
src/providers/socket_readiness.h
src/providers/thread_safe_callback.h
src/providers/transport.h
src/providers/binance/binance_transport.h
src/providers/binance/binance_combined_transport.h
src/providers/binance/binance_user_data_transport.h
src/providers/binance/binance_provider.h
src/providers/binance/binance_kill_switch.h
src/providers/binance/binance_reconciler.h
src/providers/binance/binance_rest_client.h
src/providers/binance/binance_rest_order_transport.h
src/providers/binance/binance_oco_bracket_adapter.h
src/providers/binance/binance_futures_provider.h
src/providers/binance/binance_futures_dead_mans_switch.h
src/providers/binance/binance_futures_kill_switch.h
src/providers/binance/binance_futures_reconciler.h
src/providers/binance/binance_futures_user_data_parser.h
src/providers/binance/binance_futures_register.cpp
src/providers/binance/binance_futures_bracket_adapter.h
src/providers/bitget/bitget_futures_provider.h
src/providers/bitget/bitget_transport.h
src/providers/bitget/bitget_combined_transport.h
src/providers/bitget/bitget_private_ws_transport.h
src/providers/bitget/bitget_futures_dead_mans_switch.h
src/providers/bitget/bitget_futures_kill_switch.h
src/providers/bitget/bitget_futures_reconciler.h
src/providers/bitget/bitget_futures_user_data_parser.h
src/providers/bitget/bitget_rest_client.h
src/providers/bitget/bitget_rest_order_transport.h
src/providers/bitget/bitget_futures_register.cpp
src/providers/bitget/bitget_futures_bracket_adapter.h
src/risk/risk_manager.h
src/risk/futures_risk_check.h
src/execution/live_safety.h
src/threading/worker.h
src/threading/worker_watchdog.h
```

Requirements: `LIVE_SAFETY_CCB_APPROVED` in commit body, `docs/governance/02-prerequisites.md`, clean path exercise, **T3 multi-agent protocol** (root `AGENTS.md` §6), human CCB.

Related hot files (not always mechanically frozen, but treat as high-risk / often T2–T3):

- `src/threading/*` (SPSC, spin, affinity)
- Any `*kill_switch*`, `*dead_mans*`, `*reconciler*`, `*watchdog*`
- Hot-path strategy / orderbook / risk on-event code

---

## 4. Hot Path — Definition & Non-Negotiables

### Hot path includes

- Engine event loop and event publish path  
- Provider parse → engine handoff  
- Strategy callbacks during market/order events  
- Orderbook apply/match on the event  
- Pre-trade risk checks consulted on the event  
- Critical SPSC ring push/pop on safety/market paths  

### Cold path (alloc OK if measured and off hot loop)

- Startup config, CLI, one-shot JSON load (allow-listed)  
- Logging worker, stats/risk workers, TUI, web serializers  
- QuestDB ILP, report generation, MC trial setup (between trials)  
- Tests and benchmarks harness  

### Performance red lines (R1–R10 compressed)

| ID | Rule |
|----|------|
| R1 | **Zero heap** on hot path — use pools / pre-sized buffers |
| R2 | **`forbid_runtime_grow`** stays on after prewarm; exhaust → fail closed |
| R3 | **SPSC only**; **exactly one producer** per ring (engine loop is sole producer for engine-originated rings) |
| R4 | Pad shared atomics / avoid false sharing on ring indices |
| R5 | No exceptions / RTTI / virtual dispatch on tight hot loops without measurement + review |
| R6 | No JSON, heavy `fmt`→`string`, or sync logging on hot path |
| R7 | Prefer contiguous layouts (`array`, pooled nodes) over node maps on hot structures |
| R8 | Move I/O, config, prewarm, symbol resolve to **startup** |
| R9 | No unmeasured “optimisations”; prefer p99 awareness over mean-only bragging |
| R10 | Reuse existing `acquire_pooled` / `publish_event` / workers — no parallel subsystem invention |

### Bad vs good

```cpp
// BAD — hot path
void on_trade(const Trade& t) {
  auto* e = new TradeEvent(t);                 // heap
  std::vector<Level> levels = book.top(5);     // may grow
  std::string s = std::to_string(t.px);        // alloc
  nlohmann::json j = t;                        // FORBIDDEN outside allow-list
  ring.push(e);                                // ownership muddle
}

// GOOD — hot path
void on_trade(const Trade& t) {
  auto e = acquire_pooled<TradeEvent>(trade_pool_);
  e->reset(t);                                 // placement reuse
  publish_event(std::move(e));                 // SPSC handoff
}
```

```cpp
// BAD — safety path "helpfulness"
if (kill_switch_triggered) {
  schedule_retry_with_backoff();               // NEVER
  halt_flag_ = false;                          // NEVER auto-clear
}

// GOOD
if (kill_switch_triggered) {
  halt_flag_.store(true, std::memory_order_release); // terminal; manual restart only
  // cancel / notify / diagnostics — no resume path
}
```

```cpp
// BAD — second producer
// Thread A and Thread B both critical_ring.try_push(...)

// GOOD
// Single producer → SPSC → single consumer
// Fan-in: multiple SPSC rings drained by one consumer, not MPMC "for convenience"
```

```cpp
// BAD — pool grow under load
pool.prewarm(1024);
// ... later silent grow()

// GOOD
pool.prewarm(max_concurrent + headroom);
pool.set_forbid_runtime_grow(true);  // exhaust → PoolExhausted / halt
```

### JSON allow-list (must stay tiny)

Only paths allowed by `scripts/check-hotpath-json.sh` (today: `src/bin/main.inc`, `src/api/truetest_api.cpp`, and `tests/`). Expanding the allow-list is **Ask first**.

---

## 5. Safety Red Lines

| ID | Rule |
|----|------|
| S1 | Freeze surface sacred — token + CCB + protocol |
| S2 | Compile-time live gate absolute — no runtime bypass |
| S3 | Halt is **write-once** terminal — process restart only |
| S4 | Kill / DMS / reconciler / watchdog: **loud, non-retrying, fail-closed** |
| S5 | Do not collapse kill-switch, DMS, and freeze into one vague “cancel everything” blob without design review |
| S6 | Reconciler **default-refuse**; user-data stream is source of truth; REST advisory until reconciled |
| S7 | Pre-trade risk ordered and mandatory (venue `FuturesRiskCheck` before `RiskManager` on futures hot path) |
| S8 | DMS countdown/heartbeat stays **fixed conservative** — no adaptive lengthening under load |
| S9 | No `HAS_*` / venue leakage into generic core/engine/threading/risk layers |
| S10 | Docs that restate freeze invariants are freeze-adjacent |

Full anti-pattern list: `docs/architecture/02-model.md`, `docs/governance/01-prod.md`.

---

## 6. Mode Invariants

### Backtest

- Deterministic given seed + inputs + flags  
- No network required for local/synthetic  
- Realism models (latency, queue, impact, fees) are config — don’t hardcode venue myths  

### Shadow

- Live orders must remain impossible  
- Divergence tracking is observational; don’t “fix” shadow by inventing fills  
- Prefer evidence via `--persist --run-tag …` when validating safety  

### Live

- Only `engine_live` + explicit operator ritual (`docs/governance/01-prod.md`)  
- Captcha / credentials / tiny size / attended — agents must not automate live capital increases  
- Phase 0 evidence culture over “it compiled”  

### Monte Carlo

- Per-trial seeding: deterministic from base seed + trial id  
- No hidden shared mutable state between trials  
- `--mc-parallel` only with compatible thread preset (`inline`)  
- Report `trials[]` + `seed_used`; deep-dive interesting trials via single backtest with that seed  

---

## 7. Architecture & Layers

- **Provider is the sole venue extension point** (`IProvider` + safety hooks: reconciler, kill-switch, risk check, brackets).  
- Engine is composition root; do not push venue ifdefs upward.  
- **Protective SL/TP is platform-default** (`DefaultExitPolicy` + `--exit-policy`/`--sl`/`--tp`); strategies need not implement stops. Strategy `exit_intent`s refine; they are not required for basic protection.  
- Layer edges enforced by `scripts/check-layer-deps.sh` — read failures carefully; do not `#include` “upward”.  
- Interfaces: **`I` prefix** (e.g. `IProvider`, `IRiskCheck`).  
- File size hygiene: flag ~800+ line files; prefer extract over sprawl (`engine.cpp` decomposition is planned — see `docs/internal/engine-decomposition.md` + skill `engine-decomposition`). Cold-path extract first; do not casually rewrite hot publish paths.

Register new sources in **`cmake/Sources.cmake`** (no directory globs).

---

## 8. When You Touch X → Run Y

| Touch | Extra verification |
|-------|-------------------|
| `object_pool*`, rings, event loop | Hotpath alloc tests + ASAN; forbid_runtime_grow intact |
| `engine.cpp` / freeze list | Full T3 protocol + freeze script + token + path exercise |
| Kill / DMS / reconciler / risk / halt | `/saftey` skill mindset; no soft-fail greenwash |
| Threading / affinity / presets | Think TSan; sole-producer audit |
| Strategy / indicator only | Focused strategy tests; still run JSON + layer scripts if includes change |
| Report JSON / CLI flags | Backend/UI contract impact; reproducibility fields |
| Any “optimisation” | Benchmarks before/after; hotpath tests; no mean-only claims |
| `cmake/` / `Sources.cmake` | Configure + build affected targets |
| `dashboard_snapshot` (new/changed field) | Render it (or explicitly, visibly omit it) in **both** the ncurses TUI (`src/ui/panels/*`, `tabbed_dashboard.cpp`, `console_dashboard.cpp`) and the ImGui desk (`src/ui/desk/panels/*`) before merging — see note below |

**Two UI stacks, one snapshot.** The ncurses rich TUI and the ImGui desk are both first-class, actively maintained surfaces over the same `dashboard_snapshot`/`operator_actions` seam (no plan to freeze or retire either currently) — see `docs/internal/imgui-desk-design.md`. The desk’s live positions renderer is `src/ui/desk/panels/activity_panel.cpp`; with both stacks staying in parity long-term, a field added to one renderer and forgotten in the other will not fail a build, only silently miss an operator's screen. Check both renderers whenever `dashboard_snapshot` changes.

---

## 9. Multi-Agent Requirement (core-specific)

Root `AGENTS.md` §6 is binding. For core **T2–T3**, minimum simultaneous/parallel cast:

1. **Verifier** (fresh, independent)  
2. **Fact-checker** (realtime on claims)  
3. **Parallel Executor A** (design or safety lens)  
4. **Parallel Executor B** (design alternate or perf/layer lens)  

Suggested specialist lenses for core review panels:

- Safety / anti-patterns (halt, retry, TT_TARGET, reconciler)  
- Hot-path alloc / JSON / locks  
- Layer graph / venue bleed  
- Determinism / tests / MC seeding  

Skills: `/testing`, `/check-work`, `/performance`, `/saftey`, `/quality`, `/memory-checks`.

---

## 10. Style Deltas (project-specific only)

- Prefer existing types: pools, `price` helpers, `symbol_table`, ring policies.  
- No new dependencies without human approval.  
- Comments: explain non-obvious invariants, not narration.  
- Do not “improve” unrelated code in the same change.  
- German `xx:yyy` commits via git-push skill; freeze commits carry token on its own line.

---

## 11. Definition of Done (core)

- [ ] Tier + agents satisfied (root §6)  
- [ ] Tests green (full or justified focus + change-aware extras)  
- [ ] `check-hotpath-json.sh`  
- [ ] `check-layer-deps.sh`  
- [ ] `check-live-safety-freeze.sh` (and token if freeze files touched)  
- [ ] No R*/S* red-line violations  
- [ ] Perf claims backed by numbers  
- [ ] Governance/todo pointers updated if behaviour of freezes/phases changed  

Then: `/testing` → `TESTING VERDICT: PASS` → `/check-work` → thematic commit.

---

## 12. Pointers

| Topic | Doc |
|-------|-----|
| Index / nav | `docs/00-INDEX.md`, `docs/README.md` |
| Prod / phases | `docs/governance/01-prod.md` |
| Freeze PR checklist | `docs/governance/02-prerequisites.md` |
| Todos | `docs/governance/03-todo.md`, `docs/todos/` |
| CLI / build / MC | `docs/reference/01-instructions.md` |
| Strategy SDK | `docs/reference/07-strategy-development.md` |
| Performance capacities | `docs/architecture/04-performance.md` |
| Model routing + anti-patterns | `docs/architecture/02-model.md` |
| Engine decomp plan | `docs/internal/engine-decomposition.md` |
| Data pipeline plan | `docs/internal/data-pipeline.md` |
| Multi-venue | `docs/platforms/` |

---

*Last updated: 2026-08-16 — serial preset defaults and low-memory build commands; pairs with workspace root AGENTS.md.*
