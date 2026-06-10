# AdaptiveHybridStrategy

**Adaptive Hybrid HFT Strategy** for the TrueTest C++ Trading Engine.

**Status note (2026, integrated from monte-carlo branch)**: Lower-priority work (see root `todo.md` A-01..A-07). MC simulation (now mainline) exercises strategies including this one for robustness (MC-05). Strategy is registered + L2 dispatch present (with `LIVE_SAFETY_CCB_APPROVED` comment in engine) + test + example JSON; usable for backtests/MC experiments. However, code contains multiple "compilable demo" / "simplified decision" / "real version" / "placeholder" / "v1" / always-on `enable_onchain_mock` caveats (see `src/strategy/adaptive_hybrid_strategy.cpp` + .h + config.h). On-chain is mock only; no `take_pending_exit_intents`; simplified RiskValidator/equity/L2 paths. Full hot-path polish deferred. See code comments + root `todo.md` A-* for exact items. Spec here remains useful reference.

- **Primary Mode**: Predictive Order-Book Imbalance Market Making
- **Trigger Mode**: Micro-Momentum Scalping on strong On-Chain Volume Spikes
- **Defensive Mode**: Automatic reduction / pause of quoting on low liquidity, manipulation signals, or latency violations
- **Target Environment**: 20 ms end-to-end decision window (Frankfurt colocation), small-cap coins (TRON/TRX and lower-liquidity names)

This document explains **every configuration variable, flag, internal state variable, and behavioral flag** in detail.

---

## 1. Overview & Philosophy

The strategy combines two alpha sources:

1. **Order-book imbalance** (predictive market making around a volume-weighted micro-price)
2. **On-chain volume spikes** (leading indicator for small-cap flow into CEX)

It operates with extremely strict risk gates and automatically switches into a defensive posture when conditions deteriorate.

All decisions are made **deterministically** on the engine's single hot-path thread. Cross-thread communication (On-Chain signals) uses only lock-free SPSC ring buffers.

---

## 2. Operating Modes (`Mode` enum)

```cpp
enum class Mode : uint8_t {
    MAKER_IMBALANCE   = 0,   // Default predictive MM
    SCALPER_MOMENTUM  = 1,   // Aggressive scalping on confirmed on-chain spike + momentum
    DEFENSIVE         = 2    // Reduced size, wider spreads, or no new orders
};
```

| Mode                  | When Activated                                      | Behavior                                      | Sizing          |
|-----------------------|-----------------------------------------------------|-----------------------------------------------|-----------------|
| `MAKER_IMBALANCE`     | Normal conditions, moderate imbalance               | Quote around micro-price with dynamic spread  | `maker_size_frac` |
| `SCALPER_MOMENTUM`    | Strong on-chain spike **AND** favorable imbalance momentum | Directional lean or more aggressive quoting   | `taker_size_frac` (higher) |
| `DEFENSIVE`           | `\|imbalance\| > defensive threshold`, thin book, latency breach, or manipulation detected | Minimal or zero new exposure, wide spreads    | Strongly reduced or zero |

The current mode is exposed via `get_indicator_values()` as `mode`.

---

## 3. Configuration Parameters (`AdaptiveHybridConfig`)

All parameters live in `AdaptiveHybridConfig` and can be set via:

- JSON config file (loaded at construction)
- `--param key=value` on the CLI (overrides at runtime)
- `set_param()` / `get_param_schema()` programmatically

### 3.1 Imbalance & Spread Parameters

| Parameter                    | Default | Min–Max     | Description |
|-----------------------------|---------|-------------|-------------|
| `imbalance_long_threshold`  | 0.22    | 0.01–0.80   | If `imbalance_ewma` > this value → bias quotes long |
| `imbalance_short_threshold` | -0.22   | -0.80–-0.01 | If `imbalance_ewma` < this value → bias quotes short |
| `imbalance_defensive_abs`   | 0.65    | 0.30–0.95   | Absolute threshold that forces `DEFENSIVE` mode (extreme one-sided book) |
| `spread_min_pct`            | 0.0012  | 0.0001–0.01 | Minimum dynamic spread (12 bps) |
| `spread_max_pct`            | 0.0045  | 0.001–0.02  | Maximum dynamic spread (45 bps) when imbalance is strong |
| `imb_ewma_alpha`            | 0.28    | 0.05–0.90   | Smoothing factor for imbalance EWMA (higher = more reactive) |
| `top_k_levels`              | 8       | 3–50        | Number of book levels used for imbalance and liquidity calculations |

**Dynamic Spread Formula** (simplified):
```text
spread = clamp( spread_min + (spread_max - spread_min) * min(1.0, |imb_ewma| / 0.6) , spread_min, spread_max )
```

### 3.2 Position Sizing

| Parameter          | Default | Min–Max    | Notes |
|--------------------|---------|------------|-------|
| `maker_size_frac`  | 0.0035  | 0.0005–0.02 | % of current equity per maker-style trade (normal imbalance MM) |
| `taker_size_frac`  | 0.0065  | 0.001–0.03  | Higher size used when `SCALPER_MOMENTUM` mode is active |

**Rule**: Size is always capped by the per-coin inventory limit.

### 3.3 Inventory & Risk Limits

| Parameter                  | Default | Min–Max   | Description |
|----------------------------|---------|-----------|-------------|
| `inventory_max_pct`        | 0.04    | 0.005–0.15 | Hard per-coin notional exposure limit as % of equity |
| `small_cap_inventory_pct`  | 0.025   | 0.005–0.10 | Tighter limit used when `small_cap_mode = true` |
| `small_cap_mode`           | true    | bool      | Enables tighter inventory + higher manipulation sensitivity |
| `max_global_exposure_pct`  | 0.18    | 0.05–0.60 | Coarse global notional limit (defense-in-depth) |
| `max_impact_bps`           | 6.0     | 0.5–50    | Maximum estimated slippage (in bps) the RiskValidator will accept |
| `max_cancel_to_trade`      | 0.60    | 0.1–2.0   | Maximum acceptable cancel-to-trade ratio before manipulation flag |
| `max_open_orders`          | 40      | 5–200     | Safety cap on concurrent open orders |

### 3.4 On-Chain Spike Detector

| Parameter                          | Default | Min–Max   | Description |
|------------------------------------|---------|-----------|-------------|
| `spike_z_threshold`                | 3.5     | 1.5–8.0   | z-score above which a volume spike is considered "strong" |
| `spike_lookback_s`                 | 15.0    | 2–120     | Rolling window (seconds) for spike detection |
| `spike_ewma_alpha`                 | 0.25    | 0.05–0.8  | Smoothing factor on on-chain volume baseline |
| `require_positive_imbalance_for_scalp` | true | bool   | Scalping mode only allowed when on-chain spike agrees with CEX imbalance direction |

### 3.5 Latency & Defensive Behavior

| Parameter         | Default | Min–Max   | Description |
|-------------------|---------|-----------|-------------|
| `max_latency_ms`  | 20.0    | 1–100     | End-to-end decision latency threshold. Above this → immediate `DEFENSIVE` mode |

### 3.6 Misc

| Parameter              | Default     | Description |
|------------------------|-------------|-------------|
| `rng_seed`             | 0xC0FFEE42  | Seed for any internal randomness (jitter, etc.) |
| `enable_onchain_mock`  | true        | Enables the mock on-chain thread + `inject_spike()` test hook |

---

## 4. Internal State Variables (per symbol)

These live inside `AdaptiveHybridStrategy`:

| Variable / Structure          | Type                          | Thread Writer     | Description |
|-------------------------------|-------------------------------|-------------------|-------------|
| `imb_engines_[sym]`           | `ImbalanceEngine`             | Engine hot path   | Maintains local top-K L2 view + all derived signals (EWMA imbalance, micro-price, etc.) |
| `inventory_pct_[sym]`         | `double` (signed)             | Engine hot path (only via `on_fill`) | Current notional exposure / equity for the symbol. **Never mutated outside `on_fill`** |
| `modes_[sym]`                 | `Mode`                        | Engine hot path   | Current operating mode |
| `defensive_mode_`             | `std::atomic<bool>`           | Any (via release) | Global defensive flag. Set on latency or manipulation breach |
| `latency_hist_`               | `LatencyHistogram` (atomic bins) | Engine hot path | Lock-free p50/p99/p999 + max latency tracking |
| `rejection_cnt_`              | `RejectionCounter` (atomics)  | Engine hot path   | Per-reason rejection counters |
| `onchain_ring_`               | `RingBuffer<OnChainSignal>`   | OnChain thread (producer) + Engine (consumer) | Lock-free SPSC queue for volume spike signals |
| `last_mids_[sym]`             | `double`                      | Engine hot path   | Last known mid price (used for sizing) |

**Critical Invariant**: `inventory_pct_` is only written inside `on_fill()`. This guarantees exact reconciliation with the engine's `portfolio_`.

---

## 5. Rejection Reasons (`RejectionReason` enum)

```cpp
enum class RejectionReason : uint8_t {
    NONE = 0,
    GLOBAL_RISK,
    PER_COIN_INVENTORY,
    EXCESSIVE_SLIPPAGE,
    MANIPULATION_DETECTED,
    LATENCY_VIOLATION,
    ...
};
```

| Value                     | Trigger Condition                                      | Effect |
|---------------------------|--------------------------------------------------------|--------|
| `GLOBAL_RISK`             | Total portfolio exposure or drawdown limits breached   | No order |
| `PER_COIN_INVENTORY`      | After proposed trade, `\|inventory_pct\|` would exceed limit | No order |
| `EXCESSIVE_SLIPPAGE`      | Estimated market impact of the trade > `max_impact_bps` | No order |
| `MANIPULATION_DETECTED`   | High cancel rate, layering, spoofing, or quote stuffing observed | Enter `DEFENSIVE`, no order |
| `LATENCY_VIOLATION`       | Measured or p99 decision latency > `max_latency_ms`    | Enter `DEFENSIVE`, no order |

All rejections increment the corresponding counter and are visible in `get_indicator_values()` as `rejects_total` + per-reason values.

---

## 6. The Exact 9-Step Event Flow

(See also the detailed comments in `adaptive_hybrid_strategy.cpp`.)

1. **L2 Update arrives** (`on_l2_update`)
2. **Atomic Local Book Update** → `ImbalanceEngine::on_l2_update`
3. **Latency Sampling** → `LatencyHistogram::record_ns`, defensive flag may be raised
4. **Imbalance Calculation** → `imbalance_score()`, `micro_price()`, `dynamic_spread_pct()`
5. **On-Chain Drain** → non-blocking `try_pop` from `onchain_ring_`
6. **5-Gate Risk Validation** (strict sequential order):
   - Global Risk
   - Per-Coin Inventory
   - Slippage Estimation (L2 depth walk)
   - Manipulation Detection
   - Latency Check
7. **Green Light?** → only then proceed
8. **Order Emission** → construct `order_event` and return it
9. **Post-Execution** → `on_fill` updates inventory atomically + metrics

---

## 7. Exposed Indicators (`get_indicator_values(symbol)`)

The strategy returns a vector of key/value pairs for monitoring, TUI, and analytics:

| Key                  | Meaning                                      | Source |
|----------------------|----------------------------------------------|--------|
| `imbalance_ewma`     | Current smoothed imbalance (-1.0 … +1.0)     | `ImbalanceEngine` |
| `micro_price`        | Volume-weighted micro price                  | `ImbalanceEngine` |
| `spread_pct`         | Currently applied dynamic spread             | `ImbalanceEngine` |
| `thin_book`          | 1.0 if book is considered thin               | `ImbalanceEngine` |
| `mode`               | 0 = Maker, 1 = Scalper, 2 = Defensive        | Strategy state |
| `inventory_pct`      | Current signed exposure / equity             | Strategy state |
| `rejects_total`      | Total rejections since start                 | `RejectionCounter` |
| `latency_p99_ns`     | Approximate p99 decision latency             | `LatencyHistogram` |
| `defensive`          | 1.0 if global defensive flag is set          | `defensive_mode_` atomic |

---

## 8. Thread-Safety Guarantees

- **Hot path (L2 / tick / market / fill)**: Single writer (engine event-loop thread).
- **On-Chain signals**: Produced by dedicated thread, consumed via SPSC `RingBuffer` with proper `acquire`/`release`.
- **Defensive flag**: `std::atomic<bool>` with release-store / acquire-load.
- **All counters & histograms**: `std::atomic<uint64_t>` (relaxed for counts).
- **No locks, no allocations, no shared_ptr in the decision path**.

---

## 9. How to Apply the Strategy (Step-by-Step)

This section explains in detail **how you can actually use and apply** the `AdaptiveHybridStrategy` in practice — from first experiments in backtest to safe paper trading and (with extreme caution) live deployment.

### 9.1 Prerequisites

Before you can use the strategy effectively:

1. **Build the engine** with the strategy included (already done in this workspace):
   ```bash
   cmake -B build -DBUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Release
   cmake --build build -j
   ```

2. **L2 Market Data is mandatory** — this is an order-book imbalance strategy.
   - You **must** use `--depth-stream` (e.g. `depth20@100ms` or `depth10@100ms`).
   - Without L2 the strategy will see almost no signals and stay mostly flat.

3. **The L2 dispatch patch** (already applied in this session) must be present in `src/engine/engine.cpp` so that `on_l2_update()` is actually called on the strategy.

4. Recommended starting symbol: `TRXUSDT` (or `TRXUSDT` on futures) — good liquidity for a small-cap name with real on-chain activity.

---

### 9.2 Quick Start – First Backtest Run (Recommended First Step)

```bash
./build/engine_backtest \
  --strategy adaptive-hybrid \
  --provider binance \
  --symbol TRXUSDT \
  --depth-stream depth20@100ms \
  --mode backtest \
  --param small_cap_mode=1 \
  --param maker_size_frac=0.0035 \
  --param taker_size_frac=0.006 \
  --param spike_z_threshold=3.8 \
  --param max_latency_ms=20 \
  --from 2026-04-01 --to 2026-04-07
```

**What you will see:**
- The strategy registers automatically via `REGISTER_STRATEGY`.
- It will mostly stay in `MAKER_IMBALANCE` mode.
- You can observe `imbalance_ewma`, `mode`, `inventory_pct`, and `rejects_total` in the dashboard.

---

### 9.3 Using a JSON Configuration File (Best Practice)

Create a file `adaptive_hybrid_config.json`:

```json
{
  "small_cap_mode": true,
  "imbalance_long_threshold": 0.24,
  "imbalance_short_threshold": -0.24,
  "imbalance_defensive_abs": 0.58,
  "maker_size_frac": 0.0035,
  "taker_size_frac": 0.0065,
  "inventory_max_pct": 0.035,
  "small_cap_inventory_pct": 0.022,
  "spike_z_threshold": 3.8,
  "spike_lookback_s": 12,
  "max_latency_ms": 18,
  "max_impact_bps": 5.5,
  "top_k_levels": 10,
  "rng_seed": 424242
}
```

**Load it** (the loader looks for the file in the current working directory by default, or you can extend the loader to accept a path via `--adaptive-hybrid-config`).

The strategy will automatically use the values from the JSON at construction time. CLI `--param` values still override individual keys after loading.

---

### 9.4 Overriding Parameters at Runtime (Most Flexible)

You can mix JSON + CLI overrides:

```bash
./build/engine_backtest \
  --strategy adaptive-hybrid \
  --param maker_size_frac=0.0028 \
  --param spike_z_threshold=4.2 \
  --param max_latency_ms=15 \
  ...
```

This is the fastest way to experiment with different risk profiles.

---

### 9.5 Testing with Synthetic On-Chain Spikes (Very Useful)

The strategy ships with a mock on-chain feed for reproducible testing:

```cpp
// In a custom test or paper harness
auto strat = std::make_shared<AdaptiveHybridStrategy>(cfg);

// Inject a strong positive spike on TRX
strat->inject_onchain_spike("TRXUSDT", 4.9, 8500000.0);

// Now feed realistic L2 updates — the strategy should consider SCALPER_MOMENTUM mode
```

In the dashboard you should see the mode flip and (if imbalance agrees) larger taker-sized orders.

---

### 9.6 Paper / Shadow Trading (Strongly Recommended Before Live)

```bash
./build/engine_shadow \
  --strategy adaptive-hybrid \
  --symbol TRXUSDT \
  --depth-stream depth20@100ms \
  --param small_cap_mode=1 \
  --param inventory_max_pct=0.025
```

**Watch especially:**
- `defensive` flag
- `rejects_total` and individual rejection reasons
- `inventory_pct` never exceeding your configured limit
- `latency_p99_ns`

Run for at least 4–8 hours with real market data before considering live.

---

### 9.7 Live Trading – Safety Checklist

**Never** go live with this strategy without having done the following:

- [ ] Multiple successful shadow runs with real Binance data
- [ ] `small_cap_mode=true` and conservative sizing (`maker_size_frac ≤ 0.004`)
- [ ] `max_latency_ms` set to 15–18 (gives headroom)
- [ ] `max_impact_bps` ≤ 5.0 for small caps
- [ ] Very tight `small_cap_inventory_pct` (2.0–2.5%)
- [ ] On-chain feed either disabled or extremely well validated
- [ ] You have the L2 dispatch patch compiled in (with CCB approval)
- [ ] You are running on a properly pinned core in Frankfurt (or equivalent low-latency location)
- [ ] You have monitoring for `defensive`, `rejects_total`, and inventory in real time

Even then, start with **very small risk** and expect the strategy to be in `DEFENSIVE` mode a significant percentage of the time on thin names.

---

### 9.8 Programmatic Usage (Embedding / Custom Harnesses)

```cpp
#include "strategy/adaptive_hybrid_strategy.h"
#include "strategy/strategy_registry.h"

// Option A – via registry (recommended)
auto strat = StrategyRegistry::instance().create("adaptive-hybrid");

// Option B – with explicit config
AdaptiveHybridConfig cfg;
cfg.maker_size_frac = 0.002;
cfg.spike_z_threshold = 4.0;

auto strat2 = std::make_shared<AdaptiveHybridStrategy>(cfg);

// Feed L2 data directly (useful in custom simulators)
strat2->on_l2_update( make_l2_event("TRXUSDT", tick_side::bid, 0.1198, 12500000) );

// Inject on-chain signals for testing
strat2->inject_onchain_spike("TRXUSDT", 5.1, 12000000.0);
```

---

### 9.9 Recommended Starting Configuration for TRXUSDT (2026)

```json
{
  "small_cap_mode": true,
  "maker_size_frac": 0.003,
  "taker_size_frac": 0.0055,
  "inventory_max_pct": 0.03,
  "small_cap_inventory_pct": 0.022,
  "imbalance_long_threshold": 0.23,
  "imbalance_short_threshold": -0.23,
  "imbalance_defensive_abs": 0.55,
  "spike_z_threshold": 3.9,
  "max_latency_ms": 17,
  "max_impact_bps": 4.8,
  "top_k_levels": 8
}
```

Start conservative, then gradually relax parameters only after you have solid statistics on rejection rates and realized edge.

---

### 9.10 Monitoring Dashboard – What to Watch

When the strategy is running, pay attention to these indicators (exposed via `get_indicator_values`):

- `mode` — how often it is in `DEFENSIVE` (should not be > 30–40% in normal conditions)
- `inventory_pct` — must stay well inside your configured limits
- `rejects_total` + breakdown — high `PER_COIN_INVENTORY` or `EXCESSIVE_SLIPPAGE` rejections are normal and healthy for small caps
- `latency_p99_ns` — if this approaches your `max_latency_ms`, something is wrong with the execution path
- `defensive` — should only flip to 1 during clear stress periods

---

### 9.11 Multi-Strategy Usage

You can combine the adaptive hybrid strategy with others:

```bash
--strategy mean-reversion,adaptive-hybrid
```

The engine will call both. The adaptive hybrid will only act on strong L2 + on-chain confluence, while the other strategy can handle different regimes.

---

**Summary – Recommended Learning Path**

1. Backtest on TRXUSDT with default + `small_cap_mode`
2. Experiment with `--param` overrides
3. Run the dedicated test harness (`test_adaptive_hybrid`)
4. 4–8 h shadow run with real data
5. Only then consider tiny live allocation

The strategy is deliberately **defensive by design** — it will frequently choose to do nothing. This is a feature, not a bug, especially on small-cap names.

---

## 10. Exposed Indicators (`get_indicator_values`)

The strategy returns rich telemetry for the dashboard, analytics, and QuestDB:

| Key                  | Meaning                                           | When it is useful |
|----------------------|---------------------------------------------------|-------------------|
| `imbalance_ewma`     | Current smoothed imbalance (-1.0 … +1.0)          | See bias direction |
| `micro_price`        | Volume-weighted fair price                        | Quote reference |
| `spread_pct`         | Currently applied dynamic spread                  | Risk / cost view |
| `thin_book`          | 1.0 if liquidity is poor                          | Defensive trigger |
| `mode`               | 0 = Maker, 1 = Scalper, 2 = Defensive             | Strategy state machine |
| `inventory_pct`      | Current signed notional / equity                  | Hard risk limit |
| `rejects_total`      | Total rejections since start                      | Overall "pickiness" |
| `latency_p99_ns`     | Approximate p99 decision latency                  | 20 ms budget health |
| `defensive`          | 1.0 if the global defensive flag is active        | Emergency state |

---

## 11. Thread-Safety Guarantees

- All hot-path state is written exclusively by the engine event-loop thread.
- On-chain signals arrive via lock-free SPSC `RingBuffer`.
- The `defensive_mode_` flag uses `release` / `acquire` ordering.
- No mutexes or allocations in the 20 ms decision path.

---

## 12. Production & Live Notes

- The strategy is intentionally **conservative**. Expect it to be in `DEFENSIVE` mode frequently on small-cap names.
- Real bottleneck is usually the synchronous REST execution layer, not the strategy logic.
- Always run with `small_cap_mode=true` for coins like TRX and below.
- Replace the mock `OnChainMonitor` with a real low-latency on-chain source before serious live use.
- The L2 dispatch patch in `engine.cpp` (with `LIVE_SAFETY_CCB_APPROVED`) is required for the strategy to receive incremental order book updates.

---

## Appendix: File Locations

| Component                              | Path |
|----------------------------------------|------|
| Main implementation + config struct    | `src/strategy/adaptive_hybrid_strategy.h` |
| Full logic + 9-step flow               | `src/strategy/adaptive_hybrid_strategy.cpp` |
| JSON config loader (init-time only)    | `src/strategy/adaptive_hybrid_config.{h,cpp}` |
| Example configuration                  | `adaptive_hybrid_config.example.json` (root) |
| Unit + harness tests                   | `tests/test_adaptive_hybrid.cpp` |
| L2 dispatch (with CCB comment)         | `src/engine/engine.cpp` |
| This documentation                     | `docs/AdaptiveHybridStrategy.md` |

---

**Maintainer Note**

When you add a new parameter:

1. Add it to `AdaptiveHybridConfig` with sensible defaults and comments.
2. Document it in this file (table + effect).
3. Make it available via `get_param_schema()` / `set_param()`.
4. Add test coverage and update the example JSON.

---

*Document version: 2026-05-22 – complete usage guide added.*