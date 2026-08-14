<p align="center">
  <img src="docs/assets/truetest-mark.jpg" width="140" alt="TrueTest mark" />
</p>

<h1 align="center">TrueTest</h1>

<p align="center">
  <strong>Modular C++23 trading engine</strong> for reproducible backtesting,<br/>
  divergence-aware shadow trading, and gated live execution — one tree, three binaries.
</p>

<p align="center">
  <img src="https://img.shields.io/badge/C%2B%2B-23-22D3EE?style=for-the-badge&logo=cplusplus&logoColor=0B1220" alt="C++23" />
  <img src="https://img.shields.io/badge/CMake-3.22%2B-F59E0B?style=for-the-badge&logo=cmake&logoColor=0B1220" alt="CMake" />
  <img src="https://img.shields.io/badge/platform-Linux_preferred-0B1220?style=for-the-badge&labelColor=22D3EE" alt="Linux preferred" />
  <img src="https://img.shields.io/badge/tests-1000%2B-22D3EE?style=for-the-badge&logo=googletest&logoColor=0B1220" alt="1000+ tests" />
</p>

<p align="center">
  <img src="https://img.shields.io/badge/backtest-TT__TARGET-64748B?style=flat-square" alt="backtest" />
  <img src="https://img.shields.io/badge/shadow-TT__TARGET-22D3EE?style=flat-square" alt="shadow" />
  <img src="https://img.shields.io/badge/live-gated-F59E0B?style=flat-square" alt="live gated" />
  <img src="https://img.shields.io/badge/venues-Binance%20%7C%20Bitget%20%7C%20Bitunix-0B1220?style=flat-square&labelColor=334155" alt="venues" />
  <img src="https://img.shields.io/badge/hot_path-zero--alloc-22D3EE?style=flat-square&labelColor=0B1220" alt="zero-alloc" />
</p>

---

> **Intended use** — Private personal research and retail tool only. Not enterprise or institutional software for others.  
> **Mature paths** — Monte Carlo, high-fidelity backtest, shadow divergence analysis.  
> **Live** (`engine_live`) — experimental, tiny-size, fully attended, your own risk. Phase 0/1 are personal discipline and evidence practices.  
> **Primary platform** — **Linux** (best supported). Windows and macOS are secondary (presets exist for Windows; macOS builds via toolchain + Homebrew).  
> **This tree** — C++ engine package **`core/`** inside the TrueTest workspace (sibling packages: `backend/`, `UI/`, …). This README documents **only** the engine.

Three binaries share one codebase and differ only by compile-time `TT_TARGET`. Live-order paths are **physically eliminated** (DCE) in non-live targets via `src/core/tt_target.h` → `target_allows_live_orders()`.

**Config precedence:** explicit CLI flags → `--config` JSON file → hard defaults. There is **no** `TRUETEST_CONFIG` environment variable.

| | Binary | `TT_TARGET` | Live orders | Primary use |
|:---:|:-------|:------------|:------------|:------------|
| <img src="https://img.shields.io/badge/-backtest-64748B?style=flat-square" alt="" /> | `engine_backtest` | `BACKTEST` | Impossible | Historical replay, MC campaigns |
| <img src="https://img.shields.io/badge/-shadow-22D3EE?style=flat-square" alt="" /> | `engine_shadow` | `SHADOW` | Impossible | Real-time paper vs exchange |
| <img src="https://img.shields.io/badge/-live-F59E0B?style=flat-square" alt="" /> | `engine_live` | `LIVE` | Allowed (gated) | Real-money execution with safeguards |

---

## Feature list (shipped)

What exists **in this tree today** (not aspirational):

### Engine & modes
- Three compile-time targets: `engine_backtest` · `engine_shadow` · `engine_live`
- Headless / CI-friendly runs (`--no-tui`, `--status-format`, `--dry-run`, `--dump-config`)
- Multi-strategy runs (`--strategy sma,mean-reversion`)
- Platform protective exits: `--exit-policy` (`floor` default) · `--sl` · `--tp`
- Thread presets: `inline` · `light` · `standard` · `full` · `extended` (+ pin / spin policy)
- C API embed (`BUILD_SHARED_LIB` → `libtruetest.so`)

### Data, realism & research
- Local CSV OHLCV + tick, multi-path
- Binary zstd event log record/replay (`--log-events` / `--replay`)
- Realism models: latency, impact, walked-book, queue (L2), probabilistic fills, synthetic MM book
- Monte Carlo: GBM generator, multi-trial campaigns, `--mc-reuse-objects`, experimental `--mc-parallel` (with `inline` threads)
- Synthetic / `montecarlo` provider

### Venues (opt-in at build time)
| Venue | CLI names | Maturity |
|:------|:----------|:---------|
| **Binance** | `binance`, `binance-futures` | Golden path (spot + USDT-M) |
| **Bitget** | `bitget`, `bitget-futures` | Landed UTA USDT-M · demo via `--demo` |
| **Bitunix** | `bitunix`, `bitunix-futures` | Phase 0–1 MD + paper/shadow (live routing refused) |
| **Local / synthetic** | `local`, `synthetic` | Always available |

### Safety & risk
- Compile-time live-order gate (`TT_TARGET` / DCE)
- Reconciler, Dead Man’s Switch, kill-switch, venue `IRiskCheck`
- Terminal `halt_flag_`, user-data WS as source of truth, `WorkerWatchdog`
- Phase 1 mechanical freeze (10 files + `LIVE_SAFETY_CCB_APPROVED`)

### Strategies & indicators
- Strategies: `sma` · `ma-crossover` · `mean-reversion` · `breakout` · `coiled-spring` · `larry_connor` · `hedge-demo` · `adaptive-hybrid` · `structure-continuation`
- Indicators: SMA, EMA, RSI, Stochastic, Bollinger, ATR, swing detection, rolling extremes

### Observability & UI
- zstd binary logs + rotating text logs
- Optional QuestDB ILP (`ENABLE_QUESTDB`, `--persist`, `--persist-strict`)
- Rich ncurses TUI (shadow/live)
- Optional read-only web UI (`ENABLE_WEB`, `--web` + token on shadow/live)
- 1000+ GoogleTest cases, golden regression, hot-path / layer / freeze gate scripts

---

## Upcoming features (planned / not shipped)

Tracked in [`docs/todos/`](docs/todos/) and governance — **do not treat as available**:

| Track | Examples |
|:------|:---------|
| **Phase 0** | Tiny-size mainnet futures validation — **0/15** qualifying sessions ([`reports/phase0/`](reports/phase0/)) |
| **Risk / DMS** | Further R-*/S-* items (funding analytics, DMS flatten automation, etc.) |
| **Venues** | Bitunix live path · further Bitget freeze expansion · Bybit/Gate **archived** (not on master) · COIN-M inverse · hedge mode |
| **Engine** | `engine.cpp` decomposition waves ([`docs/internal/engine-decomposition.md`](docs/internal/engine-decomposition.md)) |
| **Data** | Parquet / external API sources deferred ([`docs/internal/data-pipeline.md`](docs/internal/data-pipeline.md)) |
| **Ops / Go-Live** | 9-row capital gate, Prometheus drills, formal CCB size increases |
| **Other** | Multi-symbol / cross-margin risk, richer language bindings, Solana/Drift keeper (research only) |

Venue design notes: [`docs/platforms/`](docs/platforms/).

---

## Installation & build

### Requirements

| Component | Notes |
|:----------|:------|
| **CMake** | ≥ **3.22** |
| **C++23** | GCC **13+** or Clang **16+** (Linux/macOS); MSVC **2022** (Windows) |
| **Git** | FetchContent pulls CLI11, zstd, nlohmann/json (and GTest/Benchmark/civetweb/Abseil when enabled) |
| **ncurses (wide)** | Required to **link** `engine_shadow` / `engine_live` (rich TUI) |
| **Boost + OpenSSL** | Required for `ENABLE_BINANCE` / `ENABLE_BITGET` / `ENABLE_BITUNIX` / `ENABLE_LIVE_DATA` |
| **Node.js + npm** | Only if you build the web SPA (`ENABLE_WEB` + `web_assets`) |

Core third-party headers/libs for a minimal CSV backtest are **FetchContent**-pulled (no system Boost needed). Shadow/live and venues need system packages below.

---

### Linux <img src="https://img.shields.io/badge/preferred-22D3EE?style=flat-square&labelColor=0B1220" alt="preferred" />

Primary development and CI target. Prefer **CMake presets** → binaries in `out/build/<preset>/`.

**Disk budget:** each preset/ad-hoc tree re-builds FetchContent deps and is often **0.5–2 GB**. Keep **≤ 1–2 warm trees** (typically `linux-tests`, optionally `linux-dev` for desk/shadow). Do not leave `build/`, `build-dev/`, and several `out/build/*` trees around at once. Cleanup:

```bash
./scripts/clean-builds.sh                              # dry-run list
./scripts/clean-builds.sh --keep linux-tests --apply   # drop everything else
```

#### Arch Linux / Manjaro / EndeavourOS

```bash
# Base toolchain + TUI
sudo pacman -S --needed base-devel cmake git ninja \
  gcc clang \
  ncurses

# Venues / live networking (Binance, Bitget, Bitunix, LIVE_DATA)
sudo pacman -S --needed boost openssl

# Optional: web frontend build
sudo pacman -S --needed nodejs npm
```

```bash
git clone https://github.com/LeonardFoerster/truetest-core.git
cd truetest-core

# Recommended: tests + full unit suite
cmake --preset linux-tests
cmake --build --preset linux-tests -j"$(nproc)"
ctest --test-dir out/build/linux-tests --output-on-failure

# Venues example (Binance + QuestDB)
# cmake --preset linux-binance-questdb && cmake --build --preset linux-binance-questdb -j"$(nproc)"
```

#### Debian / Ubuntu / Mint / Pop!_OS

```bash
sudo apt update
sudo apt install -y \
  build-essential cmake git ninja-build pkg-config \
  g++-13 clang-16 \
  libncurses-dev \
  libboost-all-dev libssl-dev \
  nodejs npm   # optional, web UI only
```

> Use a **C++23** compiler. On older Ubuntu, install a newer GCC/Clang from the distro toolchain packages or a PPA if the default `g++` is &lt; 13.

```bash
git clone https://github.com/LeonardFoerster/truetest-core.git
cd truetest-core

cmake --preset linux-tests
cmake --build --preset linux-tests -j"$(nproc)"
# binaries: out/build/linux-tests/engine_*
```

Daily desk/shadow (venues + ImGui) — **one** tree, not a second `build-dev/`:

```bash
cmake --preset linux-dev
cmake --build --preset linux-dev -j"$(nproc)" --target engine_shadow truetest_tests
# or: ./launch-default.sh
```

Ad-hoc `build/` is legacy/one-off only (do not keep warm next to presets):

```bash
cmake -B build -DBUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
# when finished: rm -rf build   # or ./scripts/clean-builds.sh --keep linux-tests --apply
```

#### Fedora / RHEL / Alma / Rocky

```bash
sudo dnf groupinstall -y "Development Tools" "C Development Tools and Libraries"
sudo dnf install -y \
  cmake git ninja-build \
  gcc gcc-c++ \
  ncurses-devel \
  boost-devel openssl-devel \
  nodejs npm   # optional
```

On RHEL-family, ensure the default GCC is **≥ 13** (or enable a Toolset / clang with C++23).

```bash
git clone https://github.com/LeonardFoerster/truetest-core.git
cd truetest-core

cmake --preset linux-tests
cmake --build --preset linux-tests -j"$(nproc)"
```

#### Common Linux feature builds

```bash
# Binance + QuestDB + tests
cmake --preset linux-binance-questdb && cmake --build --preset linux-binance-questdb -j"$(nproc)"

# Bitget / Bitunix / all venues
cmake --preset linux-bitget   && cmake --build --preset linux-bitget -j"$(nproc)"
cmake --preset linux-bitunix  && cmake --build --preset linux-bitunix -j"$(nproc)"
cmake --preset linux-venues   && cmake --build --preset linux-venues -j"$(nproc)"

# Web UI (needs npm for SPA)
cmake --preset linux-web && cmake --build --preset linux-web -j"$(nproc)"
cmake --build --preset linux-web --target web_assets   # if npm is available

# Sanitizers / release-native
cmake --preset linux-asan && cmake --build --preset linux-asan -j"$(nproc)"
cmake --preset linux-release-native && cmake --build --preset linux-release-native -j"$(nproc)"
```

| CMake option | Needs extra system deps |
|:-------------|:------------------------|
| (minimal) | compiler + cmake + git |
| Shadow/live link | **ncurses** (wide) |
| `ENABLE_BINANCE` / `BITGET` / `BITUNIX` / `LIVE_DATA` | **Boost** + **OpenSSL** |
| `ENABLE_QUESTDB` | none (raw sockets) |
| `ENABLE_WEB` | FetchContent civetweb; **npm** only for SPA assets |
| `BUILD_TESTS` / `ENABLE_BENCHMARKS` / `ENABLE_DEBUG` | FetchContent (GTest / Benchmark / Abseil) |

---

### macOS <img src="https://img.shields.io/badge/secondary-64748B?style=flat-square" alt="secondary" />

No dedicated CMake preset (Linux/Windows only in `CMakePresets.json`). Use an ad-hoc build tree.

```bash
# Homebrew
brew install cmake git ninja \
  ncurses \
  boost openssl@3 \
  node   # optional, web UI

# Prefer Apple Clang or Homebrew LLVM with C++23
export CMAKE_PREFIX_PATH="$(brew --prefix openssl@3):$(brew --prefix ncurses)"
```

```bash
git clone https://github.com/LeonardFoerster/truetest-core.git
cd truetest-core

cmake -B build \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTS=ON \
  -DENABLE_BINANCE=ON   # optional; needs Boost+OpenSSL
cmake --build build -j"$(sysctl -n hw.ncpu)"
ctest --test-dir build --output-on-failure
```

Notes:
- Shadow/live still require **ncurses**; Homebrew’s `ncurses` may need `CMAKE_PREFIX_PATH` as above.
- `-DENABLE_NATIVE_OPT=ON` is valid but ties the binary to the build machine CPU.

---

### Windows <img src="https://img.shields.io/badge/secondary-64748B?style=flat-square" alt="secondary" />

Presets: `windows-ninja` (Ninja + toolchain on `PATH`) and `windows-vs-2022` (VS 17 2022, x64).

```powershell
# Prerequisites (examples)
# - Visual Studio 2022 with "Desktop development with C++"
# - CMake 3.22+, Git
# - Optional: vcpkg for Boost/OpenSSL when enabling venues
#   vcpkg install boost-asio boost-beast openssl
#   cmake ... -DCMAKE_TOOLCHAIN_FILE=[vcpkg]/scripts/buildsystems/vcpkg.cmake
```

```powershell
git clone https://github.com/LeonardFoerster/truetest-core.git
cd truetest-core

cmake --preset windows-vs-2022
cmake --build --preset windows-vs-2022 --config Debug

# Or Ninja (after activating VS dev environment / Ninja on PATH)
cmake --preset windows-ninja
cmake --build --preset windows-ninja
```

Output under `out/build/windows-vs-2022/` or `out/build/windows-ninja/`.

| Caveat | Detail |
|:-------|:-------|
| **Primary CI/dev** | Linux — Windows is best-effort |
| **Rich TUI** | ncurses/PDCurses availability varies; prefer headless flags if link fails |
| **Venues** | Prefer **vcpkg** Boost + OpenSSL; wire via toolchain file |
| **Sanitizers** | Linux-oriented (`linux-asan` / `linux-tsan` presets) |

---

## Architecture (30 seconds)

```text
  Venue / CSV / Synthetic          engine_backtest | engine_shadow | engine_live
           │                              (TT_TARGET compile-time gate)
           ▼
      IProvider  ──parse / transport──►  Engine event loop
           │                                  │
           │                    ┌─────────────┼─────────────┐
           │                    ▼             ▼             ▼
           │               Strategy      Risk / exits   Orderbook
           │                    │             │             │
           │                    └──────► SPSC rings / pools ◄── zero-alloc hot path
           │                                  │
           └──── safety hooks ──► DMS / kill / reconciler / IRiskCheck
                                              │
                         TUI · Web (read-only) · QuestDB · zstd event log
```

**Provider is the only venue extension point** (`IProvider` + reconciler / kill-switch / risk-check / brackets). Core layers must not grow `HAS_*` venue ifdefs. Details: [`docs/architecture/`](docs/architecture/) · [`AGENTS.md`](AGENTS.md).

---

## Quick start & typical workflows

Paths below assume a **preset** build (`out/build/…`). For ad-hoc builds, replace with `./build/…`.

### Credentials (env preferred)

Environment variables **win over** `--api-key` / `--api-secret` / `--api-passphrase` (CLI secrets can leak via process lists). **Never commit keys.**

| Venue | Variables |
|:------|:----------|
| Binance (default resolve) | `TRUETEST_BINANCE_API_KEY`, `TRUETEST_BINANCE_API_SECRET` |
| Bitget | `TRUETEST_BITGET_API_KEY`, `…_SECRET`, `…_PASSPHRASE` |
| Bitunix | `TRUETEST_BITUNIX_API_KEY`, `…_SECRET` |

Optional hygiene: `scripts/check-credentials.sh`. Full flags: [`docs/reference/04-flags.md`](docs/reference/04-flags.md).

### Workflows

**1. CSV backtest (minimal build)**

```bash
./build/engine_backtest \
  --provider local \
  --path market_data.csv \
  --strategy sma
```

**2. Headless synthetic / CI-style**

```bash
./out/build/linux-tests/engine_backtest \
  --provider synthetic --strategy sma --seed 424242 \
  --no-pin --status-format off --no-tui \
  --output /tmp/run.json
```

**3. Monte Carlo campaign**

```bash
./out/build/linux-tests/engine_backtest \
  --provider synthetic \
  --strategy sma \
  --monte-carlo --mc-trials 50 \
  --mc-reuse-objects \
  --thread-preset inline \
  --no-pin --status-format off --no-tui
# With --mc-parallel, keep --thread-preset inline (pinning conflicts otherwise).
```

**4. Shadow vs Binance futures (paper; needs `ENABLE_BINANCE`)**

```bash
./out/build/linux-binance-questdb/engine_shadow \
  --provider binance-futures \
  --symbol BTCUSDT \
  --stream trade \
  --depth-stream depth20@100ms \
  --persist --run-tag my_shadow_run
```

**5. Bitget demo / paptrading (needs `ENABLE_BITGET`; not Phase 0 qualifying)**

```bash
./out/build/linux-bitget/engine_shadow \
  --provider bitget-futures \
  --symbol BTCUSDT \
  --stream trade \
  --depth-stream books5 \
  --no-pin --status-format off --no-tui
# Live demo: engine_live + --demo --live + TRUETEST_BITGET_* — see docs/operations/03-bitget-demo.md
```

**6. Live (experimental)** — `--live` + credentials + math captcha on mainnet. Ritual: [`docs/governance/01-prod.md`](docs/governance/01-prod.md).

**7. Web UI (read-only; needs `ENABLE_WEB`)**

```bash
cmake -B build -DENABLE_WEB=ON && cmake --build build -j
cd src/web/frontend && npm ci && npm run build
./build/engine_shadow ... --web --web-token <secret> --web-assets src/web/assets
# → http://127.0.0.1:8080/   (shadow/live require --web-token)
```

---

## Build flags (short)

| Style | Configure | Binaries |
|:------|:----------|:---------|
| **Preset** | `cmake --preset linux-tests` | `out/build/linux-tests/` |
| **Ad-hoc** | `cmake -B build -DBUILD_TESTS=ON` | `build/` |

| Option | Effect |
|:-------|:-------|
| `-DENABLE_BINANCE` / `BITGET` / `BITUNIX` | Venue providers |
| `-DENABLE_QUESTDB` / `WEB` / `DEBUG` | Persistence / web UI / instrumentation |
| `-DENABLE_NATIVE_OPT` | `-march=native` on all three engines (Release) |
| `-DBUILD_TESTS` / `BUILD_SHARED_LIB` | GoogleTest / `libtruetest.so` |

Source lists: **`cmake/Sources.cmake`** (no globs). Full tables above under *Installation*.

---

## Troubleshooting

| Symptom | Likely fix |
|:--------|:-----------|
| CMake/compiler rejects C++23 | GCC ≥ 13 or Clang ≥ 16 (or MSVC 2022) |
| `engine_shadow` / `engine_live` fail to link Curses | Install wide **ncurses** (`libncurses-dev` / `ncurses` / `ncurses-devel`) |
| Configure fails on Boost/OpenSSL | Install system Boost + OpenSSL **or** drop venue `ENABLE_*` flags |
| “Binary not found” after `cmake --preset …` | Presets write to **`out/build/<preset>/`**, not `build/` |
| `--web` exits on shadow/live | Pass **`--web-token`** (required outside backtest) |
| Venue provider “not found” at runtime | Rebuild with matching `ENABLE_BINANCE` / `BITGET` / `BITUNIX` |
| MC + parallel hangs / odd affinity | Use `--thread-preset inline` with `--mc-parallel`; prefer `--no-pin` in containers |

---

## Safety surface (Phase 1 freeze)

| Layer | Frozen paths (exact list in `scripts/check-live-safety-freeze.sh`) |
|:------|:-------------------------------------------------------------------|
| Gate | `src/core/tt_target.h` |
| Engine | `src/engine/engine.cpp` |
| Binance futures | `binance_futures_{provider,dead_mans_switch,kill_switch,reconciler}.h` |
| Risk / exec / threads | `risk_manager.h` · `futures_risk_check.h` · `live_safety.h` · `worker_watchdog.h` |

Edits need **`LIVE_SAFETY_CCB_APPROVED`**, two-person CCB, clean multi-hour `engine_shadow`.  
→ [`docs/governance/01-prod.md`](docs/governance/01-prod.md) · [`docs/governance/02-prerequisites.md`](docs/governance/02-prerequisites.md) · [`AGENTS.md`](AGENTS.md)

| Phase | Status |
|:------|:-------|
| Phase 0 tiny-size mainnet | **0/15** qualifying |
| Phase 1 freeze | **Enforced** |
| Risk / DMS backlog | Partial (`docs/todos/`) |

---

## Documentation map

| Document | Purpose |
|:---------|:--------|
| [`docs/reference/01-instructions.md`](docs/reference/01-instructions.md) | Master how-to |
| [`docs/reference/04-flags.md`](docs/reference/04-flags.md) | CLI flags (`main.inc`) |
| [`docs/governance/01-prod.md`](docs/governance/01-prod.md) | Phases, Go-Live, Phase 0 ritual |
| [`docs/todos/`](docs/todos/) | Detailed backlog |
| [`AGENTS.md`](AGENTS.md) | Agent rules, freeze, hot path |
| [`docs/platforms/`](docs/platforms/) | Venue status |
| [`docs/README.md`](docs/README.md) | Full nav |

---

## Testing, gates & contributing

```bash
cmake --preset linux-tests && cmake --build --preset linux-tests -j
ctest --test-dir out/build/linux-tests --output-on-failure

# After any edit under src/
./scripts/check-hotpath-json.sh
./scripts/check-layer-deps.sh
./scripts/check-live-safety-freeze.sh
```

- **Agents / AI rules:** [`AGENTS.md`](AGENTS.md) (this package) and parent workspace `AGENTS.md` for monorepo scope.
- **Frozen surface PRs:** commit body must contain `LIVE_SAFETY_CCB_APPROVED` + checklist in [`docs/governance/02-prerequisites.md`](docs/governance/02-prerequisites.md).

---

## License & disclaimer

- **License:** No `LICENSE` file is present in this tree at the time of writing. Treat the project as **private personal research software** unless the author publishes an explicit license.
- **Not financial advice.** Nothing here is an offer to manage capital or a guarantee of trading performance.
- **Live trading risks total loss of capital.** Use tiny size, attend the process, and follow Phase 0/1 gates before increasing size.
- **Not enterprise / multi-tenant production software.** See intended use at the top of this file.

---

<p align="center">
  <img src="https://img.shields.io/badge/TrueTest-personal_research_platform-0B1220?style=for-the-badge&labelColor=22D3EE&color=0B1220" alt="personal research" />
</p>

<p align="center">
  <sub>Use it responsibly. Prefer Linux for day-to-day development. All live trading carries risk.</sub>
</p>
