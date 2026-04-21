# 11 — Split `main.cpp` and `engine.cpp`

## Goal

Break up the two largest source files in the tree:

- `BacktestEngine/src/main.cpp` — **1,346 lines**
- `BacktestEngine/src/core/engine.cpp` — **2,566 lines**

Both have grown organically and mix concerns that should be separate.
Splitting them is a prerequisite for the provider-pure refactor
([01-provider-pure-engine.md](01-provider-pure-engine.md)) and for any
future contributor to be productive.

## Context — `main.cpp`

Today it contains:

1. All CLI11 argument definitions (~200 lines).
2. JSON config file loader with per-flag `was_set` overlay (~200 lines).
3. TUI menu.
4. Replay mode wiring.
5. Provider-based run mode wiring.
6. CSV-based run mode wiring.
7. Strategy instantiation.
8. Fee / risk / threading config assembly.
9. Results export.

## Context — `engine.cpp`

1. Ring setup and worker startup.
2. Bar loop (`run()`).
3. Tick loop (`run_tick_data()`).
4. Replay loop (`run_replay()`).
5. Streaming bar loop (`run_streaming`).
6. Streaming tick loop (`run_streaming`).
7. WS command processing + snapshot broadcasts.
8. Stop-loss / take-profit enforcement.
9. Latency queue draining.
10. Checkpoint read/write.
11. SQLite run-metadata logging.
12. Multi-strategy dispatch helpers.

## Instructions — `main.cpp` split

1. **Create `BacktestEngine/src/cli/`** with:
   - `cli_args.h/.cpp` — all CLI11 definitions, returns a populated
     `RawCliOptions` struct. One file, one responsibility.
   - `cli_config.h/.cpp` — merges CLI options with JSON config file
     (uses the validator from [09-config-schema.md](09-config-schema.md)).
   - `cli_modes.h/.cpp` — mode dispatchers: `run_backtest(cfg)`,
     `run_replay(cfg)`, `run_live(cfg)`, `run_shadow(cfg)`, `run_tui()`.
   - `cli_export.h/.cpp` — results export (`export_results`).

2. **`main.cpp` shrinks to ~50 lines**:

   ```cpp
   int main(int argc, char** argv) {
       auto raw = cli::parse_args(argc, argv);
       if (raw.dump_config) return cli::dump_config(raw);
       if (raw.dry_run)     return cli::dry_run(raw);
       auto cfg = cli::resolve_config(raw);
       if (!cfg.ok) { cli::print_errors(cfg); return 1; }
       return cli::dispatch_mode(cfg.value);
   }
   ```

## Instructions — `engine.cpp` split

1. **`engine.cpp` stays as the orchestrator** (construction, destruction,
   `run()` entry points that delegate). ~500 lines target.

2. **Create these files under `core/`**:
   - `engine_run_bar.cpp` — `run()` and bar processing helpers.
   - `engine_run_tick.cpp` — `run_tick_data()` and tick helpers.
   - `engine_run_streaming.cpp` — both `run_streaming()` overloads.
   - `engine_run_replay.cpp` — replay path.
   - `engine_order_pipeline.cpp` — `process_order`, `route_order`,
     `check_pending_stops`, `unwind_positions`, `dispatch_extras_*`.
   - `engine_persistence.cpp` — checkpoint + SQLite run metadata.
   - `engine_web.cpp` — all `#ifdef HAS_WEB_UI` bodies.

3. **Do not break `engine.h`**. The public API stays untouched. All splits
   are implementation-only; member functions move between `.cpp` files
   but keep their declarations in `engine.h`.

4. **Private helper visibility**: if a helper was a private member function
   only used inside one `.cpp`, convert it to a file-local `static` function
   and remove it from `engine.h`.

## Acceptance criteria

- `main.cpp` ≤ 100 lines.
- `engine.cpp` ≤ 600 lines.
- No public API changes — every existing caller of `engine` and the CLI
  behaves identically.
- Full test suite + golden regression pass with zero diffs.
- Build time on an incremental rebuild of one mode file is faster than
  before (objective: single-mode rebuild no longer compiles the whole
  engine translation unit).

## Out of scope

- Replacing CLI11 with something else.
- Converting `main.cpp` into a library. The binary still ships.
- Any behavioural change. This task is a pure refactor.
