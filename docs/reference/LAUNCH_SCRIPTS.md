# Launch Scripts — Development & Testing Guide

This guide explains the redesigned launch scripts for TrueTest. They are now **simplified, task-focused, and configuration-driven**.

## 🎯 Quick Start

```bash
# ImGui desk development (rapid iteration)
./launch-desk.sh --desk-debug-ui

# Quick test run (shadow mode, all default)
./launch-default.sh

# Performance backtest (no UI, focused on metrics)
./launch-bench.sh --symbol ethusdt

# Unit tests only (no engine)
./launch-test.sh --filter "market*"

# Live provider connection (real data, paper trading)
./launch-live.sh --provider bitget
```

---

## 📋 Configuration System

### `.env` File (Local Config)

Copy `.env.example` to `.env` and customize:

```bash
cp .env.example .env
# Edit .env for your preferences
```

Key variables:

```bash
# Task preset: dev, bench, test, live
TT_TASK=dev

# Market data
TT_PROVIDER=binance-futures
TT_SYMBOL=btcusdt
TT_STREAM=trade
TT_DEPTH_STREAM=depth5

# Engine mode: shadow (safe), live (real money), backtest
TT_MODE=shadow

# Desk UI options
DESK_ENABLED=true
DESK_DEBUG_UI=false
DESK_LAYOUT=

# Build options
TT_PRESET=linux-dev
TT_BUILD_JOBS=8
TT_SKIP_BUILD=false
```

### Environment Variable Overrides

```bash
# Override defaults
TT_SYMBOL=ethusdt ./launch-default.sh

# Multiple overrides
TT_PROVIDER=bitget TT_SYMBOL=ethusdt ./launch-default.sh

# Task-based config
TT_TASK=bench ./launch-default.sh
```

### CLI Flag Precedence

CLI flags override environment variables:

```bash
# CLI flag wins
TT_SYMBOL=btcusdt ./launch-live.sh --symbol ethusdt  # → ethusdt
```

---

## 🚀 Scripts Overview

### `launch-default.sh` — Default Launcher

**Purpose:** General-purpose launcher for development.

**Best for:** Quick testing, prototyping, mixed workflows.

**Default config:**
- Task: `dev`
- Preset: `linux-dev`
- Provider: `binance-futures`
- Mode: `shadow`
- UI: ImGui Desk (enabled)

**Usage:**

```bash
# Start with defaults
./launch-default.sh

# Override symbol
./launch-default.sh --symbol ethusdt

# Use as config source
TT_TASK=bench TT_SYMBOL=ethusdt ./launch-default.sh

# Pass multiple flags
./launch-default.sh --symbol ethusdt --provider bitget --mode shadow
```

---

### `launch-desk.sh` — ImGui Desk Development

**Purpose:** Rapid UI iteration for ImGui desk.

**Best for:** UI development, layout testing, debug visualization.

**Features:**
- Debug UI mode (wireframes, performance stats)
- Layout preset switching
- Optimized for quick rebuilds

**Default config:**
- Task: `dev`
- UI: ImGui Desk (always enabled)
- Mode: `shadow`

**Usage:**

```bash
# Basic desk setup
./launch-desk.sh

# Enable debug visualization
./launch-desk.sh --desk-debug-ui

# Load layout preset
./launch-desk.sh --desk-layout mobile.h

# Debug mode + different symbol
./launch-desk.sh --desk-debug-ui --symbol ethusdt

# Skip rebuild (for rapid iterations)
TT_SKIP_BUILD=true ./launch-desk.sh
```

**Available Options:**

| Flag | Value | Description |
|------|-------|-------------|
| `--desk-debug-ui` | — | Enable debug visualization |
| `--desk-layout` | `file` | Load layout preset |
| `--symbol` | `pair` | Trading symbol |
| `--provider` | `exchange` | Market data provider |
| `--help` | — | Show help |

---

### `launch-live.sh` — Live Provider Connection

**Purpose:** Test real market data feeds with live exchanges.

**Best for:** Provider integration testing, paper trading, market monitoring.

**Safety:** Default is `shadow` mode (paper trading). Use `--mode live` with caution.

**Default config:**
- Task: `live`
- Provider: `binance-futures`
- Mode: `shadow` (safe)
- UI: ImGui Desk (enabled)

**Providers:**

| Provider | Best For | Notes |
|----------|----------|-------|
| `binance-futures` | General purpose | Most liquid, highest fees |
| `bitget` | Data quality | Good coverage, lower fees |
| `bitunix` | Emerging pairs | Niche symbols |

**Usage:**

```bash
# Test Binance (shadow mode, safe)
./launch-live.sh

# Use Bitget
./launch-live.sh --provider bitget

# Monitor different symbol
./launch-live.sh --symbol ethusdt

# Paper trading with real Bitget data
./launch-live.sh --provider bitget --symbol ethusdt

# DANGER: Real trading (requires risk controls)
TT_MODE=live ./launch-live.sh
```

**⚠️ Live Mode Safety:**

```bash
# Before running in live mode:
# 1. Read config/live_<provider>.toml
# 2. Verify risk limits
# 3. Test with shadow mode first

# Script will ask for confirmation:
TT_MODE=live ./launch-live.sh
# Type: YES I UNDERSTAND
```

---

### `launch-bench.sh` — Performance & Backtest

**Purpose:** Run engine in backtest mode for performance testing.

**Best for:** Strategy validation, performance profiling, stress testing.

**Features:**
- No UI (faster execution)
- Backtest mode (historical data)
- Optional logging/metrics

**Default config:**
- Task: `bench`
- Mode: `backtest`
- UI: Disabled
- Provider: `bitget`

**Usage:**

```bash
# Basic backtest
./launch-bench.sh

# Different symbol
./launch-bench.sh --symbol ethusdt

# Multiple symbols (run in sequence)
./launch-bench.sh --symbol btcusdt
./launch-bench.sh --symbol ethusdt

# Save output for analysis
./launch-bench.sh --symbol ethusdt --log-output bench_eth.log

# Shadow mode with live data
./launch-bench.sh --mode shadow

# Performance profiling
time ./launch-bench.sh --symbol btcusdt
```

**Backtest Data Sources:**

```bash
./launch-bench.sh --provider binance-futures   # High quality, high fees
./launch-bench.sh --provider bitget            # Recommended balance
./launch-bench.sh --provider bitunix           # Emerging pairs
```

---

### `launch-test.sh` — Unit Tests

**Purpose:** Build and run unit tests without engine binary.

**Best for:** CI/CD, regression testing, test-driven development.

**Features:**
- Fast builds (only test executable)
- Test filtering
- Multiple output formats (text, XML, JSON)
- Flaky test detection

**Default config:**
- Task: `test`
- Builds: `truetest_tests` only
- No engine binary

**Usage:**

```bash
# Run all tests
./launch-test.sh

# Run specific test suite
./launch-test.sh --filter "market*"
./launch-test.sh --filter "desk*"
./launch-test.sh --filter "backtest*"

# Verbose output
./launch-test.sh --verbose
./launch-test.sh --filter "order*" --verbose

# Find flaky tests (run 5 times)
./launch-test.sh --repeat 5

# Generate test report
./launch-test.sh --output xml
./launch-test.sh --output json

# Combined: filter + repeat + verbose
./launch-test.sh --filter "desk*" --repeat 3 --verbose

# Fast syntax check (no build)
TT_SKIP_BUILD=true ./launch-test.sh
```

**Test Filtering:**

```bash
--filter "market*"       # Market data tests
--filter "order*"        # Order management
--filter "desk*"         # Desk/UI tests
--filter "backtest*"     # Backtest engine
--filter "shadow*"       # Shadow mode
--filter "*"             # All tests
```

---

## 🔧 Advanced Usage

### Skip Rebuild (Rapid Iteration)

```bash
# Build once
./launch-desk.sh

# Then iterate without rebuilding (3x faster)
TT_SKIP_BUILD=true ./launch-desk.sh
TT_SKIP_BUILD=true ./launch-desk.sh --desk-debug-ui
```

### Custom Build Directory

```bash
# Use ad-hoc build tree (don't modify CMake preset)
TT_BUILD_DIR=/tmp/truetest-build ./launch-default.sh

# Build with different config
TT_BUILD_DIR=./build-custom TT_PRESET=linux-tests ./launch-test.sh
```

### Parallel Jobs

```bash
# Use all cores
TT_BUILD_JOBS=16 ./launch-default.sh

# Limit (useful if system is busy)
TT_BUILD_JOBS=4 ./launch-default.sh

# Default: auto-detect CPU cores
./launch-default.sh
```

### Task-Based Configuration

Each task has different defaults:

```bash
# dev: Quick iteration, desk enabled, shadow mode
TT_TASK=dev ./launch-default.sh

# bench: No UI, backtest mode, performance focus
TT_TASK=bench ./launch-default.sh

# test: Only tests, no engine
TT_TASK=test ./launch-default.sh

# live: Desk enabled, shadow mode by default
TT_TASK=live ./launch-live.sh
```

---

## 📊 Configuration Examples

### Example 1: Rapid ImGui Development

```bash
# .env
TT_TASK=dev
TT_SYMBOL=btcusdt
TT_PROVIDER=binance-futures
DESK_ENABLED=true
DESK_DEBUG_UI=false
TT_SKIP_BUILD=false

# First run (build + UI)
./launch-desk.sh

# Then iterate (no rebuild)
TT_SKIP_BUILD=true ./launch-desk.sh --desk-debug-ui
TT_SKIP_BUILD=true ./launch-desk.sh
```

### Example 2: Backtest Pipeline

```bash
# Backtest multiple symbols
for symbol in btcusdt ethusdt bnbusdt; do
  ./launch-bench.sh --symbol "$symbol" --log-output "bench_$symbol.log"
done

# Analyze results
grep "performance\|metrics" bench_*.log
```

### Example 3: Live Provider Testing

```bash
# Test different providers with same symbol
./launch-live.sh --provider binance-futures --symbol ethusdt
./launch-live.sh --provider bitget --symbol ethusdt
./launch-live.sh --provider bitunix --symbol ethusdt
```

### Example 4: CI/CD Test Suite

```bash
# Run all tests with XML output
./launch-test.sh --filter "*" --output xml

# Run specific suites
./launch-test.sh --filter "backtest*" --verbose
./launch-test.sh --filter "desk*" --repeat 3

# Check exit code for CI
if ./launch-test.sh; then
  echo "Tests passed"
else
  echo "Tests failed"
  exit 1
fi
```

---

## 🐛 Troubleshooting

### Build Failures

```bash
# Check requirements
cmake --version
bash --version

# Verbose build output
TT_VERBOSE_BUILD=true ./launch-default.sh

# Clean rebuild
rm -rf out/build/<preset>
./launch-default.sh
```

### Provider Connection Issues

```bash
# Check API status pages
# Binance:    https://status.binance.com
# Bitget:     https://www.bitget.com/status
# BitUNIX:    Check their status page

# Test with different provider
./launch-live.sh --provider bitget
```

### Port Already in Use

```bash
# Find process using port
lsof -i :8080

# Kill process
kill -9 <PID>

# Or use different port
TT_WS_PORT=8081 ./launch-default.sh
```

### Tests Failing Intermittently

```bash
# Find flaky tests (run 5 times)
./launch-test.sh --filter "desk*" --repeat 5 --verbose

# Review output for patterns
```

---

## 🔄 Helper Functions

### Shared Functions (`launch-helpers.sh`)

All scripts source `launch-helpers.sh` for:

- **Logging:** `log_info`, `log_success`, `log_warn`, `log_error`
- **Errors:** `error_exit`, `require_var`
- **Build:** `configure_cmake`, `build_targets`, `build`, `verify_binary`
- **Config:** `load_env_file`, `apply_task_config`, `print_config_summary`
- **Validation:** `verify_repo_structure`, `check_requirements`

### Create Custom Script

```bash
#!/usr/bin/env bash
set -euo pipefail

root_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
source "$root_dir/launch-helpers.sh"

# Load config
load_env_file "$root_dir/.env"

# Use helpers
log_section "My Custom Launcher"
check_requirements
verify_repo_structure "$root_dir"

# Build
build "$root_dir" "$build_dir" "linux-dev" "engine_shadow" 8

log_success "Done!"
```

---

## 📚 Related Documentation

- [CMake Presets](CMakePresets.json) — Build configurations
- [Development Guide](DEVELOPMENT.md) — Workflow & best practices
- [Architecture](ARCHITECTURE.md) — Engine design

---

## 💡 Tips & Best Practices

### For Desk UI Development

1. **Use `launch-desk.sh`** — optimized for UI work
2. **Enable debug mode** — `--desk-debug-ui` shows layout boundaries
3. **Skip rebuilds** — `TT_SKIP_BUILD=true` for rapid iterations
4. **Test multiple symbols** — verify UI adapts to different data

### For Performance Testing

1. **Use `launch-bench.sh`** — no UI overhead
2. **Log output** — `--log-output file.log` for analysis
3. **Run multiple iterations** — catch performance regressions
4. **Monitor system** — `watch -n 1 'top -b -n 1 | head -20'`

### For Testing & CI

1. **Use `launch-test.sh`** — fast builds
2. **Filter tests** — `--filter "pattern*"` for faster feedback
3. **Run multiple times** — `--repeat N` to catch flaky tests
4. **Generate reports** — `--output xml` for CI integration

### For Live/Provider Work

1. **Always test in shadow mode first** — `--mode shadow`
2. **Check provider status** — before connecting
3. **Verify risk limits** — in `config/live_*.toml`
4. **Use `launch-live.sh`** — includes safety confirmations

---

## ❓ FAQ

**Q: Which script should I use daily?**
A: Depends on your task. Use `launch-desk.sh` for UI, `launch-default.sh` for quick tests, `launch-bench.sh` for performance.

**Q: How do I speed up rebuilds?**
A: Use `TT_SKIP_BUILD=true` for rapid iterations (only works if binary exists).

**Q: How do I switch between providers?**
A: Use `--provider` flag or set `TT_PROVIDER` in `.env`.

**Q: Can I run multiple engines simultaneously?**
A: Yes, but check port conflicts. Use `TT_WS_PORT=XXXX` to set different ports.

**Q: How do I capture engine output?**
A: Use redirection: `./launch-default.sh 2>&1 | tee engine.log`

**Q: What's the difference between `shadow` and `live` mode?**
A: `shadow` = paper trading (safe), `live` = real money (dangerous).

---

Last updated: 2026-08-14  
Applies to: `imgui` branch and beyond
