# 10 — Clock abstraction

## Goal

Introduce an **`IClock`** interface so that every timestamp the engine
generates comes from one swappable source. Today, live / paper / shadow
code paths mix wall-clock and simulated timestamps — shadow-mode
comparison is unreliable as a result.

## Context

- `std::chrono::system_clock::now()` is called in dozens of places:
  event generation, fill timestamping, stop-loss triggers, risk-window
  pruning.
- `BinanceExecutor::poll_live_fills()` uses `system_clock::now()` for
  fill timestamps instead of the exchange `T` field — see
  [04-binance-userdata-stream.md](04-binance-userdata-stream.md).
- Backtests use data-driven timestamps parsed from CSV; but anything
  the engine *generates* (e.g. log events, periodic checkpoints) still
  calls `system_clock::now()` and pollutes determinism.
- Replay mode wants to advance time according to the log's timestamps,
  not wall-clock.

## Instructions

1. **Add `types/clock.h`**:

   ```cpp
   class IClock {
   public:
       virtual ~IClock() = default;
       using time_point = std::chrono::system_clock::time_point;

       // Current time.
       virtual time_point now() const = 0;

       // Advance (simulated clocks only). WallClock ignores.
       virtual void advance_to(time_point /*tp*/) {}

       // Sleep / wait — in replay / simulation this may be instantaneous.
       virtual void sleep_until(time_point tp) { (void)tp; }
   };

   class WallClock : public IClock {
   public:
       time_point now() const override { return std::chrono::system_clock::now(); }
       void sleep_until(time_point tp) override { std::this_thread::sleep_until(tp); }
   };

   class SimulatedClock : public IClock {
   public:
       explicit SimulatedClock(time_point start) : now_(start) {}
       time_point now() const override { return now_; }
       void advance_to(time_point tp) override { if (tp > now_) now_ = tp; }
   private:
       time_point now_{};
   };
   ```

2. **Engine owns a `std::shared_ptr<IClock> clock_`**. Default
   construction uses `WallClock`. `engine_config` gains a
   `std::shared_ptr<IClock> clock;` field; if null at construction,
   engine picks `WallClock` for live/paper and `SimulatedClock` for
   backtest/replay based on `engine_mode`.

3. **Thread in `clock_` everywhere `system_clock::now()` is called**
   inside engine / analytics / risk / workers. Grep:

   ```
   rg 'system_clock::now' BacktestEngine/src | wc -l
   ```

   Each hit is either:
   - (a) a timestamp the engine *generates* → replace with `clock_->now()`,
   - (b) parsing an external timestamp (keep as-is), or
   - (c) a benchmark / debug-only measurement (keep wall-clock).

   Add comments distinguishing the three categories.

4. **Backtest mode**: the engine advances the `SimulatedClock` to the
   timestamp of each incoming bar / tick before dispatching it. Any
   generated event (e.g. stop-loss fill triggered inside the bar) then
   carries a monotonic, reproducible timestamp.

5. **Replay mode**: same as backtest but source is the event log.
   `engine::run_replay()` drives the clock from the log's timestamps.

6. **Tests**:
   - `tests/test_simulated_clock.cpp` — advance_to monotonicity, no rollback.
   - Extend one existing engine test to use `SimulatedClock` and assert
     that a full run produces identical `fill_event` timestamps across
     repeated runs (currently this is flaky at sub-second granularity).

7. **Shadow mode implication**: shadow mode compares a simulated fill
   against a live fill. Both timestamps must come from comparable clocks;
   document that in shadow mode, the simulated adapter uses `WallClock`
   too, so latency is measured against the same time reference.

## Acceptance criteria

- `grep -rn system_clock::now BacktestEngine/src/core/` returns zero hits
  (or only commented debug hits).
- Two successive backtest runs with the same seed produce **identical**
  event-log timestamps (bit-for-bit).
- Live mode still works; nothing about real-time behaviour regresses.

## Out of scope

- Monotonic clock for latency measurement (`steady_clock`) — that is a
  distinct concern; do not mix it with `system_clock`.
- Cross-machine clock synchronisation.
- Exposing a virtual clock through the Python / plugin API.
