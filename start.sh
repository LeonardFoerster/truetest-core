#!/usr/bin/env bash
set -euo pipefail

# ── TrueTest Start Script ──────────────────────────────────────────────────
#
# All engine and build settings are configured in the section below.
# Edit the variables, then run:  ./start.sh
#
# ────────────────────────────────────────────────────────────────────────────

ROOT="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$ROOT/build"
VCPKG_DIR="$BUILD_DIR/_vcpkg"
BINARY="$BUILD_DIR/truetest"

# Detect OS
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) IS_WINDOWS=true ;;
    *)                     IS_WINDOWS=false ;;
esac

if [[ "$IS_WINDOWS" == true ]]; then
    BINARY="$BUILD_DIR/Debug/truetest.exe"
    [[ -f "$BUILD_DIR/Release/truetest.exe" ]] && BINARY="$BUILD_DIR/Release/truetest.exe"
fi

# ╔══════════════════════════════════════════════════════════════════════════╗
# ║                         CONFIGURATION                                  ║
# ╚══════════════════════════════════════════════════════════════════════════╝

# ── Script mode ────────────────────────────────────────────────────────────
# "full"       → build + start engine
# "build-only" → build, don't start
# "no-build"   → skip build, start directly
MODE="full"

# ── CMake build flags ─────────────────────────────────────────────────────
ENABLE_BINANCE=ON         # Binance live WebSocket streaming
ENABLE_LIVE_DATA=OFF      # Generic WebSocket data source
ENABLE_POSTGRESQL=OFF     # PostgreSQL backend (needs libpqxx/vcpkg)
ENABLE_SQLITE=ON          # SQLite persistence for trades/portfolio
ENABLE_DEBUG=OFF          # Performance instrumentation (needs Abseil)
ENABLE_TSAN=OFF           # ThreadSanitizer
BUILD_TESTS=ON            # GoogleTest suite

# ── Data provider ─────────────────────────────────────────────────────────
# "local"   → read from a CSV file (set PROVIDER_PATH below)
# "binance" → live WebSocket stream from Binance
PROVIDER="binance"

# ── Local provider settings ───────────────────────────────────────────────
PROVIDER_PATH="$ROOT/market_data.csv"   # CSV file for local provider
FORMAT="bar"                             # "bar" (OHLCV) or "tick"

# ── Binance provider settings ─────────────────────────────────────────────
SYMBOL="btcusdt"           # Binance symbol (lowercase)
STREAM="trade"             # "trade" (tick data) or "kline_1m" (1-min bars)
API_KEY=""                 # Binance API key (for live execution, optional)
API_SECRET=""              # Binance API secret (optional)
BINANCE_HOST=""            # Override host (e.g. testnet: "testnet.binance.vision")
BINANCE_PORT=""            # Override port

# ── Strategy ──────────────────────────────────────────────────────────────
# "mean-reversion" | "sma" | "ma-crossover"
STRATEGY="mean-reversion"
SMA_PERIOD=20              # SMA lookback window

# ── Portfolio / risk ──────────────────────────────────────────────────────
BALANCE=10000.0            # Starting cash balance
RISK_FRACTION=0.02         # Fraction of equity per trade (0.02 = 2%)
STOP_LOSS=0.005            # Stop-loss percentage (0.005 = 0.5%)
TAKE_PROFIT=0.01           # Take-profit percentage (0.01 = 1.0%)

# ── Fee model ─────────────────────────────────────────────────────────────
# "none" | "fixed" | "tiered"
FEE_MODEL="none"
FEE_VALUE=0.0              # For fixed: flat fee per trade
MAKER_RATE=0.0             # For tiered: maker rate
TAKER_RATE=0.0             # For tiered: taker rate

# ── Engine mode ───────────────────────────────────────────────────────────
# "" (default backtest) | "shadow" (compare sim vs exchange) | "live" (execute)
ENGINE_MODE=""
LIVE_EXECUTION=false       # Pass --live flag (enables real order submission)

# ── Threading ─────────────────────────────────────────────────────────────
# "" (auto-detect) | "inline" | "light" | "standard" | "full" | "extended"
THREAD_PRESET=""
NO_PIN=false               # Disable CPU affinity pinning
SEED=0                     # RNG seed (0 = random)

# ── Historical backfill ──────────────────────────────────────────────────
BACKFILL_BARS=500             # Number of historical candles to load on start (0 = disabled)
BACKFILL_INTERVAL=""          # Kline interval for backfill (empty = match stream type)

# ── SQLite persistence ────────────────────────────────────────────────────
DB_PATH="truetest.db"      # SQLite database path (empty = no persistence)

# ── Event logging ─────────────────────────────────────────────────────
EVENT_LOG_PATH=""           # Binary event log output (e.g. "events.bin")
RECORD_PATH=""              # Record raw transport data (for replay)
REPLAY_DATA_PATH=""         # Replay previously recorded data

# ╔══════════════════════════════════════════════════════════════════════════╗
# ║                     END OF CONFIGURATION                               ║
# ╚══════════════════════════════════════════════════════════════════════════╝

# ── Colors ─────────────────────────────────────────────────────────────────
RED='\033[0;31m'
GREEN='\033[0;32m'
BLUE='\033[0;34m'
YELLOW='\033[0;33m'
BOLD='\033[1m'
NC='\033[0m'

info()  { echo -e "${BLUE}[info]${NC}  $*"; }
ok()    { echo -e "${GREEN}[ok]${NC}    $*"; }
warn()  { echo -e "${YELLOW}[warn]${NC}  $*"; }
fail()  { echo -e "${RED}[fail]${NC}  $*"; exit 1; }

# ── Cleanup on exit ───────────────────────────────────────────────────────
PIDS=()
cleanup() {
    echo ""
    info "Shutting down..."
    for pid in "${PIDS[@]}"; do
        if kill -0 "$pid" 2>/dev/null; then
            kill "$pid" 2>/dev/null || true
            wait "$pid" 2>/dev/null || true
        fi
    done
    ok "Done."
}
trap cleanup EXIT INT TERM

# ── Bootstrap vcpkg + install dependencies ───────────────────────────────
bootstrap_vcpkg() {
    # Collect which vcpkg features are needed based on enabled CMake flags
    local features=()
    [[ "$ENABLE_BINANCE"    == "ON" ]] && features+=(binance)
    [[ "$ENABLE_LIVE_DATA"  == "ON" ]] && features+=(live-data)
    [[ "$ENABLE_POSTGRESQL" == "ON" ]] && features+=(postgresql)
    [[ "$ENABLE_SQLITE"     == "ON" ]] && features+=(sqlite)

    if [[ ${#features[@]} -eq 0 ]]; then
        info "No optional features enabled — skipping vcpkg."
        return
    fi

    # Clone vcpkg if not present
    if [[ ! -d "$VCPKG_DIR/.git" ]]; then
        info "Cloning vcpkg (one-time setup)..."
        git clone --depth 1 --branch 2026.03.18 \
            https://github.com/microsoft/vcpkg.git "$VCPKG_DIR" \
            || fail "Failed to clone vcpkg. Ensure git is on PATH."
    fi

    # Bootstrap vcpkg if executable missing
    local vcpkg_exe="$VCPKG_DIR/vcpkg"
    if [[ "$IS_WINDOWS" == true ]]; then
        vcpkg_exe="$VCPKG_DIR/vcpkg.exe"
        
        # Check if compiler is visible before bootstrapping
        if ! command -v cl.exe &>/dev/null; then
            warn "cl.exe (MSVC) not found in PATH."
            warn "To fix this on Windows:"
            warn "1. Open 'Developer Command Prompt for VS 2026'"
            warn "2. Type 'sh' or 'bash' to start this shell"
            warn "3. Run ./start.sh again"
            echo ""
        fi

        if [[ ! -f "$vcpkg_exe" ]]; then
            info "Bootstrapping vcpkg..."
            
            # If we are in a Dev CMD, VSINSTALLDIR is usually set. 
            # We can pass this to vcpkg to help it find the compiler.
            if [[ -n "${VSINSTALLDIR:-}" ]]; then
                export VCPKG_VISUAL_STUDIO_PATH="${VSINSTALLDIR}"
                info "Using VS path from environment: $VCPKG_VISUAL_STUDIO_PATH"
            fi

            # Use cmd //c to run the batch file in a native Windows context
            # We use temporary cd to the vcpkg dir to avoid pathing issues in bootstrap
            (cd "$VCPKG_DIR" && cmd //c "bootstrap-vcpkg.bat -disableMetrics") \
                || fail "vcpkg bootstrap failed. Check if 'Desktop development with C++' is installed in VS 2026."
        fi
    else
        if [[ ! -f "$vcpkg_exe" ]]; then
            info "Bootstrapping vcpkg..."
            sh "$VCPKG_DIR/bootstrap-vcpkg.sh" -disableMetrics \
                || fail "vcpkg bootstrap failed."
        fi
    fi

    ok "vcpkg ready: $vcpkg_exe"

    # Build the VCPKG_MANIFEST_FEATURES list for CMake
    # (CMake manifest mode will auto-install these on configure)
    VCPKG_FEATURES=$(IFS=";"; echo "${features[*]}")
    VCPKG_TOOLCHAIN="$VCPKG_DIR/scripts/buildsystems/vcpkg.cmake"

    info "vcpkg features: ${features[*]}"
}

# ── Build C++ engine ──────────────────────────────────────────────────────
build_engine() {
    # Auto-install dependencies via vcpkg
    bootstrap_vcpkg

    local cmake_flags=(
        -DENABLE_BINANCE="$ENABLE_BINANCE"
        -DENABLE_LIVE_DATA="$ENABLE_LIVE_DATA"
        -DENABLE_POSTGRESQL="$ENABLE_POSTGRESQL"
        -DENABLE_SQLITE="$ENABLE_SQLITE"
        -DENABLE_DEBUG="$ENABLE_DEBUG"
        -DENABLE_TSAN="$ENABLE_TSAN"
        -DBUILD_TESTS="$BUILD_TESTS"
    )

    # Pass vcpkg toolchain + features if vcpkg was bootstrapped
    if [[ -n "${VCPKG_TOOLCHAIN:-}" ]]; then
        cmake_flags+=(
            "-DCMAKE_TOOLCHAIN_FILE=$VCPKG_TOOLCHAIN"
            "-DVCPKG_MANIFEST_FEATURES=$VCPKG_FEATURES"
        )
    fi

    info "Configuring C++ engine... (${cmake_flags[*]})"
    cmake -B "$BUILD_DIR" "${cmake_flags[@]}" -Wno-dev "$ROOT"

    info "Building C++ engine..."
    local cores
    cores=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo "${NUMBER_OF_PROCESSORS:-4}")
    cmake --build "$BUILD_DIR" -j "$cores" 2>&1 | tail -3

    [[ -f "$BINARY" ]] || fail "Binary not found at $BINARY"
    ok "Engine built: $BINARY"
}

# ── Assemble engine CLI args ──────────────────────────────────────────────
build_engine_args() {
    ENGINE_ARGS=()

    # Provider
    ENGINE_ARGS+=(--provider "$PROVIDER")

    if [[ "$PROVIDER" == "local" ]]; then
        ENGINE_ARGS+=(--path "$PROVIDER_PATH")
        if [[ "$FORMAT" != "bar" ]]; then ENGINE_ARGS+=(--format "$FORMAT"); fi
    fi

    if [[ "$PROVIDER" == "binance" ]]; then
        if [[ -n "$SYMBOL" ]];        then ENGINE_ARGS+=(--symbol "$SYMBOL"); fi
        if [[ -n "$STREAM" ]];        then ENGINE_ARGS+=(--stream "$STREAM"); fi
        if [[ -n "$API_KEY" ]];       then ENGINE_ARGS+=(--api-key "$API_KEY"); fi
        if [[ -n "$API_SECRET" ]];    then ENGINE_ARGS+=(--api-secret "$API_SECRET"); fi
        if [[ -n "$BINANCE_HOST" ]];  then ENGINE_ARGS+=(--host "$BINANCE_HOST"); fi
        if [[ -n "$BINANCE_PORT" ]];  then ENGINE_ARGS+=(--port "$BINANCE_PORT"); fi
    fi

    # Strategy
    ENGINE_ARGS+=(--strategy "$STRATEGY")
    ENGINE_ARGS+=(--sma-period "$SMA_PERIOD")

    # Portfolio / risk
    ENGINE_ARGS+=(--balance "$BALANCE")
    ENGINE_ARGS+=(--risk-fraction "$RISK_FRACTION")
    ENGINE_ARGS+=(--sl "$STOP_LOSS")
    ENGINE_ARGS+=(--tp "$TAKE_PROFIT")

    # Fees
    if [[ "$FEE_MODEL" != "none" ]]; then
        ENGINE_ARGS+=(--fee "$FEE_MODEL")
        ENGINE_ARGS+=(--fee-value "$FEE_VALUE")
        if [[ "$FEE_MODEL" == "tiered" ]]; then
            ENGINE_ARGS+=(--maker-rate "$MAKER_RATE")
            ENGINE_ARGS+=(--taker-rate "$TAKER_RATE")
        fi
    fi

    # Engine mode
    if [[ -n "$ENGINE_MODE" ]];        then ENGINE_ARGS+=(--mode "$ENGINE_MODE"); fi
    if [[ "$LIVE_EXECUTION" == true ]]; then ENGINE_ARGS+=(--live); fi

    # Threading
    if [[ -n "$THREAD_PRESET" ]];   then ENGINE_ARGS+=(--thread-preset "$THREAD_PRESET"); fi
    if [[ "$NO_PIN" == true ]];     then ENGINE_ARGS+=(--no-pin); fi
    if [[ "$SEED" -ne 0 ]];        then ENGINE_ARGS+=(--seed "$SEED"); fi

    # SQLite persistence
    if [[ -n "$DB_PATH" ]];           then ENGINE_ARGS+=(--db "$DB_PATH"); else ENGINE_ARGS+=(--no-db); fi

    # Historical backfill
    if [[ "$BACKFILL_BARS" -gt 0 ]]; then
        ENGINE_ARGS+=(--backfill "$BACKFILL_BARS")
    fi
    if [[ -n "$BACKFILL_INTERVAL" ]]; then
        ENGINE_ARGS+=(--backfill-interval "$BACKFILL_INTERVAL")
    fi

    # Event logging / recording
    if [[ -n "$EVENT_LOG_PATH" ]];    then ENGINE_ARGS+=(--log-events "$EVENT_LOG_PATH"); fi
    if [[ -n "$RECORD_PATH" ]];       then ENGINE_ARGS+=(--record "$RECORD_PATH"); fi
    if [[ -n "$REPLAY_DATA_PATH" ]];  then ENGINE_ARGS+=(--replay-data "$REPLAY_DATA_PATH"); fi
}

# ── Start engine ──────────────────────────────────────────────────────────
start_engine() {
    build_engine_args

    info "Starting engine..."
    info "Engine args: ${ENGINE_ARGS[*]}"
    "$BINARY" "${ENGINE_ARGS[@]}" &
    PIDS+=($!)
    ok "Engine PID: ${PIDS[-1]}"
}

# ── Main ──────────────────────────────────────────────────────────────────
echo -e "${BOLD}${BLUE}"
echo "  ╔══════════════════════════════════╗"
echo "  ║           TrueTest               ║"
echo "  ╚══════════════════════════════════╝"
echo -e "${NC}"

case "$MODE" in
    full)
        build_engine
        start_engine
        echo ""
        ok "Engine running. Press Ctrl+C to stop."
        wait
        ;;
    build-only)
        build_engine
        echo ""
        ok "Build complete. Set MODE=\"no-build\" and run again to start."
        ;;
    no-build)
        [[ -x "$BINARY" ]] || fail "Engine not built. Set MODE=\"full\" first."
        start_engine
        echo ""
        ok "Running. Press Ctrl+C to stop."
        wait
        ;;
    *)
        fail "Unknown MODE: $MODE (use full, build-only, or no-build)"
        ;;
esac
