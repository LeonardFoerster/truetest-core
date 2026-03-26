#!/usr/bin/env bash
set -euo pipefail

# ── TrueTest All-in-One Start Script ────────────────────────────────────────
#
# Usage:
#   ./start.sh                        Build + start engine + open UI
#   ./start.sh --dev                  Build + engine + frontend dev server (hot reload)
#   ./start.sh --build-only           Build everything, don't start
#   ./start.sh --no-build             Skip build, start directly
#   ./start.sh --binance <symbol>     Live Binance trade stream (e.g. btcusdt)
#   ./start.sh --binance-kline <sym>  Live Binance kline_1m stream
#   ./start.sh --provider local --path market_data.csv --strategy sma
#
# Script flags:
#   --dev             Use Vite dev server (hot reload) instead of static build
#   --build-only      Build everything, don't start
#   --no-build        Skip build, start directly
#   --no-browser      Don't auto-open the browser
#   --ws-port <port>  WebSocket port (default: 8765)
#   --binance <sym>   Shortcut: --provider binance --symbol <sym> --stream trade
#   --binance-kline <sym>  Shortcut: --provider binance --symbol <sym> --stream kline_1m
#
# The UI includes a timeframe selector (1s–1h) that controls how trade-stream
# ticks are aggregated into OHLCV bars on the chart. Chart updates are
# incremental for low-latency rendering during live streaming.
#
# Engine arguments are passed through after the script flags.
# ────────────────────────────────────────────────────────────────────────────

ROOT="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$ROOT/build"
WEB_DIR="$ROOT/web"
BINARY="$BUILD_DIR/truetest"
WS_PORT=8765
VITE_PORT=5173

# Defaults
MODE="full"          # full | dev | build-only | no-build
ENGINE_ARGS=()
OPEN_BROWSER=true
ENABLE_BINANCE=false

# ── Parse flags ─────────────────────────────────────────────────────────────

while [[ $# -gt 0 ]]; do
    case "$1" in
        --dev)
            MODE="dev"
            shift
            ;;
        --build-only)
            MODE="build-only"
            shift
            ;;
        --no-build)
            MODE="no-build"
            shift
            ;;
        --no-browser)
            OPEN_BROWSER=false
            shift
            ;;
        --ws-port)
            WS_PORT="$2"
            shift 2
            ;;
        --binance)
            ENABLE_BINANCE=true
            ENGINE_ARGS+=(--provider binance --symbol "$2" --stream trade)
            shift 2
            ;;
        --binance-kline)
            ENABLE_BINANCE=true
            ENGINE_ARGS+=(--provider binance --symbol "$2" --stream kline_1m)
            shift 2
            ;;
        --help|-h)
            head -26 "$0" | tail -22
            exit 0
            ;;
        *)
            ENGINE_ARGS+=("$1")
            shift
            ;;
    esac
done

# ── Colors ──────────────────────────────────────────────────────────────────

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

# ── Cleanup on exit ────────────────────────────────────────────────────────

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

# ── Build C++ engine ────────────────────────────────────────────────────────

build_engine() {
    local cmake_flags=(-DENABLE_WEB_UI=ON)
    if [[ "$ENABLE_BINANCE" == true ]]; then
        cmake_flags+=(-DENABLE_BINANCE=ON)
    fi

    info "Configuring C++ engine... (flags: ${cmake_flags[*]})"
    cmake -B "$BUILD_DIR" "${cmake_flags[@]}" -Wno-dev "$ROOT" > /dev/null 2>&1

    info "Building C++ engine..."
    local cores
    cores=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
    cmake --build "$BUILD_DIR" -j "$cores" 2>&1 | tail -3

    [[ -x "$BINARY" ]] || fail "Binary not found at $BINARY"
    ok "Engine built: $BINARY"
}

# ── Build frontend ──────────────────────────────────────────────────────────

build_frontend() {
    if [[ ! -d "$WEB_DIR/node_modules" ]]; then
        info "Installing frontend dependencies..."
        (cd "$WEB_DIR" && npm install --silent)
    fi

    info "Building frontend..."
    (cd "$WEB_DIR" && npx vite build --outDir "$BUILD_DIR/web-dist" --emptyOutDir > /dev/null 2>&1)
    ok "Frontend built: $BUILD_DIR/web-dist/"
}

# ── Start engine ────────────────────────────────────────────────────────────

start_engine() {
    local args=(--web-ui --ws-port "$WS_PORT")

    # If no provider/path/strategy given, use defaults
    if [[ ${#ENGINE_ARGS[@]} -eq 0 ]]; then
        if [[ -f "$ROOT/market_data.csv" ]]; then
            args+=(--provider local --path "$ROOT/market_data.csv" --strategy sma)
            info "Using default: local provider, market_data.csv, SMA strategy"
        fi
    else
        args+=("${ENGINE_ARGS[@]}")
    fi

    info "Starting engine on ws://localhost:$WS_PORT ..."
    info "Engine args: ${args[*]}"
    if [[ "$ENABLE_BINANCE" == true ]]; then
        info "Binance live mode — use the timeframe selector in the UI to change bar interval"
    fi
    "$BINARY" "${args[@]}" &
    PIDS+=($!)
    ok "Engine PID: ${PIDS[-1]}"
}

# ── Start frontend dev server ───────────────────────────────────────────────

start_dev_server() {
    info "Starting Vite dev server on http://localhost:$VITE_PORT ..."
    (cd "$WEB_DIR" && npx vite --port "$VITE_PORT" --host) &
    PIDS+=($!)
    ok "Vite PID: ${PIDS[-1]}"
}

# ── Serve built frontend ───────────────────────────────────────────────────

start_static_server() {
    local dist="$BUILD_DIR/web-dist"
    [[ -f "$dist/index.html" ]] || fail "Frontend not built. Run without --no-build first."

    # Try python3, then python, then npx serve
    if command -v python3 &>/dev/null; then
        info "Serving frontend via python3 on http://localhost:$VITE_PORT ..."
        (cd "$dist" && python3 -m http.server "$VITE_PORT" --bind 127.0.0.1) &>/dev/null &
        PIDS+=($!)
    elif command -v python &>/dev/null; then
        (cd "$dist" && python -m http.server "$VITE_PORT" --bind 127.0.0.1) &>/dev/null &
        PIDS+=($!)
    else
        warn "No static server available (python3 not found). Open $dist/index.html manually."
        return
    fi
    ok "Frontend served at http://localhost:$VITE_PORT"
}

# ── Open browser ───────────────────────────────────────────────────────────

open_browser() {
    [[ "$OPEN_BROWSER" == true ]] || return 0
    local url="http://localhost:$VITE_PORT"

    sleep 1
    if command -v xdg-open &>/dev/null; then
        xdg-open "$url" 2>/dev/null &
    elif command -v open &>/dev/null; then
        open "$url" 2>/dev/null &
    fi
}

# ── Main ────────────────────────────────────────────────────────────────────

echo -e "${BOLD}${BLUE}"
echo "  ╔══════════════════════════════════╗"
echo "  ║   TrueTest Trading Dashboard     ║"
echo "  ╚══════════════════════════════════╝"
echo -e "${NC}"

case "$MODE" in
    full)
        build_engine
        build_frontend
        start_engine
        start_static_server
        open_browser
        echo ""
        ok "All systems running. Press Ctrl+C to stop."
        wait
        ;;
    dev)
        build_engine
        # Ensure node_modules exist
        [[ -d "$WEB_DIR/node_modules" ]] || (cd "$WEB_DIR" && npm install --silent)
        start_engine
        start_dev_server
        open_browser
        echo ""
        ok "Dev mode running. Frontend has hot reload. Press Ctrl+C to stop."
        wait
        ;;
    build-only)
        build_engine
        build_frontend
        echo ""
        ok "Build complete. Run with --no-build to start."
        ;;
    no-build)
        [[ -x "$BINARY" ]] || fail "Engine not built. Run without --no-build first."
        start_engine
        if [[ -f "$BUILD_DIR/web-dist/index.html" ]]; then
            start_static_server
        else
            warn "Frontend not built. Run with --dev for hot reload or build first."
        fi
        open_browser
        echo ""
        ok "Running. Press Ctrl+C to stop."
        wait
        ;;
esac
