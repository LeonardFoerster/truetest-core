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
has_footprint_tick_size=false
no_footprint=false
for arg in "${engine_args[@]}"; do
  [[ "$arg" == "--provider" || "$arg" == --provider=* ]] && has_provider=true
  [[ "$arg" == "--desk" ]] && has_desk=true
  [[ "$arg" == "--symbol" || "$arg" == --symbol=* ]] && has_symbol=true
  [[ "$arg" == "--stream" || "$arg" == --stream=* ]] && has_stream=true
  [[ "$arg" == "--depth-stream" || "$arg" == --depth-stream=* ]] && has_depth_stream=true
  [[ "$arg" == "--mode" || "$arg" == --mode=* ]] && has_mode=true
  [[ "$arg" == "--footprint-tick-size" || "$arg" == --footprint-tick-size=* ]] && has_footprint_tick_size=true
  [[ "$arg" == "--no-footprint" ]] && no_footprint=true
done
[[ "$has_provider" == true ]] || engine_args=(--provider "$provider" "${engine_args[@]}")
[[ "$has_desk" == true ]] || engine_args=(--desk "${engine_args[@]}")
[[ "$has_symbol" == true ]] || engine_args=(--symbol btcusdt "${engine_args[@]}")
[[ "$has_stream" == true ]] || engine_args=(--stream trade "${engine_args[@]}")
# depth5 enables the combined event-stream path (trade + L2) so the desk
# receives snapshots; without it, binance-futures falls through to CsvTickParser.
[[ "$has_depth_stream" == true ]] || engine_args=(--depth-stream depth5 "${engine_args[@]}")
[[ "$has_mode" == true ]] || engine_args=(--mode shadow "${engine_args[@]}")
# The footprint desk panel (auto-activated by --desk) needs a resolved tick
# size before it will collect anything (footprint.md #2.1). Binance's
# provider does not report instrument metadata today (no get_instrument()
# override, unlike Bitget), so resolve_footprint_tick_size() always comes
# back "unavailable" and the panel silently stays empty unless
# --footprint-tick-size supplies one explicitly. Default it here for the
# script's own default symbol (btcusdt, Binance USDⓈ-M tick size 0.10) so a
# stock `./launch-default.sh` run gets a working footprint out of the box;
# skip it whenever the caller picked a different symbol/provider/tick size,
# or opted out with --no-footprint, since 0.10 would be wrong or unused then.
# has_provider must also be false: if the caller passed --provider
# explicitly, $provider (env-default only) can't tell us what venue is
# actually in play, so don't guess.
if [[ "$has_footprint_tick_size" == false && "$no_footprint" == false \
      && "$has_symbol" == false && "$has_provider" == false \
      && "$provider" == binance* ]]; then
  engine_args=(--footprint-tick-size 0.10 "${engine_args[@]}")
fi

exec "$build_dir/engine_shadow" "${engine_args[@]}"
