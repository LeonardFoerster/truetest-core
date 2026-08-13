#!/usr/bin/env bash
# Local development launcher. It builds the safe shadow binary, then connects
# to one compiled venue for market data (select with TT_PROVIDER).
set -euo pipefail

root_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
build_dir="${TT_BUILD_DIR:-$root_dir/build-dev}"
build_jobs="${TT_BUILD_JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || printf '2')}"
provider="${TT_PROVIDER:-binance-futures}"

cmake -S "$root_dir" -B "$build_dir" \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTS=ON \
  -DENABLE_IMGUI=ON \
  -DENABLE_LIVE_DATA=ON \
  -DENABLE_BINANCE=ON \
  -DENABLE_BITGET=ON \
  -DENABLE_BITUNIX=ON \
  -DENABLE_DEBUG=OFF \
  -DENABLE_BENCHMARKS=OFF \
  -DENABLE_NATIVE_OPT=OFF
cmake --build "$build_dir" --target engine_shadow truetest_tests -j "$build_jobs"

engine_args=("$@")
has_provider=false
has_desk=false
has_symbol=false
has_stream=false
has_depth_stream=false
has_mode=false
for arg in "${engine_args[@]}"; do
  [[ "$arg" == "--provider" || "$arg" == --provider=* ]] && has_provider=true
  [[ "$arg" == "--desk" ]] && has_desk=true
  [[ "$arg" == "--symbol" || "$arg" == --symbol=* ]] && has_symbol=true
  [[ "$arg" == "--stream" || "$arg" == --stream=* ]] && has_stream=true
  [[ "$arg" == "--depth-stream" || "$arg" == --depth-stream=* ]] && has_depth_stream=true
  [[ "$arg" == "--mode" || "$arg" == --mode=* ]] && has_mode=true
done
[[ "$has_provider" == true ]] || engine_args=(--provider "$provider" "${engine_args[@]}")
[[ "$has_desk" == true ]] || engine_args=(--desk "${engine_args[@]}")
[[ "$has_symbol" == true ]] || engine_args=(--symbol btcusdt "${engine_args[@]}")
[[ "$has_stream" == true ]] || engine_args=(--stream trade "${engine_args[@]}")
# depth5 enables the combined event-stream path (trade + L2) so the desk
# receives snapshots; without it, binance-futures falls through to CsvTickParser.
[[ "$has_depth_stream" == true ]] || engine_args=(--depth-stream depth5 "${engine_args[@]}")
[[ "$has_mode" == true ]] || engine_args=(--mode shadow "${engine_args[@]}")
# No --footprint-tick-size default here: the desk now opens on the single
# Monitor page (positions/orders, health, risk) and no longer shows the
# footprint/orderflow panel by default — that page is benched, not even
# reachable via the command palette. Pass --footprint-tick-size yourself if
# you re-enable Orderflow in desk_layout_model.h's desk_pages.

exec "$build_dir/engine_shadow" "${engine_args[@]}"
