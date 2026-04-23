# hft-engine / TrueTest (Work in Progress)

A modular C++23 engine that starts as a backtesting platform and is reused as
the foundation for shadow trading and live execution. The build produces three
binaries from one source tree — `engine_backtest`, `engine_shadow`,
`engine_live` — differing only in a compile-time `TT_TARGET` define. Live
order submission is gated at compile time so only `engine_live` can place real
orders.

## Where things live

| Document                                           | Purpose                                                               |
|----------------------------------------------------|-----------------------------------------------------------------------|
| [`CLAUDE.md`](CLAUDE.md)                           | Authoritative description of the **current** codebase (build, layout, conventions, stack decisions). |
| [`docs/user-manual.md`](docs/user-manual.md)       | Operator-facing manual: CLI flags, provider modes, runtime behaviour. |
| [`docs/target-architecture.md`](docs/target-architecture.md) | Long-form engineering target (deepdive). Read with the deviations preamble. |
| [`docs/migration.md`](docs/migration.md)           | Running changelog of files touched by the deepdive refactor.          |
| [`prerequisites.md`](prerequisites.md)             | Cleanup / refactor checklist executed before new deepdive work lands. |
| [`todo.md`](todo.md)                               | Phased implementation plan tracking each deepdive section.            |
| [`docs/`](docs/)                                   | Historical design notes (`01-persistent-state.md` … `05-historical-backfill.md`, `refactor/00-overview.md` …). |

## Quick build

```bash
# Default — CSV + SQLite persistence on, no network deps
cmake -B build
cmake --build build
./build/engine_backtest
```

Optional features (PostgreSQL, Binance live streaming, sanitisers,
benchmarks, shared library) are opt-in via `ENABLE_*` CMake flags. The full
matrix is documented in [`CLAUDE.md`](CLAUDE.md#build) and
[`docs/user-manual.md`](docs/user-manual.md).

## Accepted CSV format

```
ID, symbol, date, time (without zone), open, high, low, close, volume
```

Tick-level CSV is also supported via the `local` provider in tick mode — see
the user manual for schema and flags.

## Status

Pre-deepdive refactor in progress on branch `pre_transform`. See
`prerequisites.md` for the prerequisite checklist and `todo.md` for the phased
deepdive plan. `master` stays frozen at the currently-running
Binance/SQLite backtester until a whole phase is green in CI **and** passes a
manual shadow run.
