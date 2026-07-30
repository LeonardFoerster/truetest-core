# Web UI

A browser dashboard for TrueTest — a live trading cockpit and a backtest-review
report, rendering the same data as the ncurses TUI. Opt-in, off the hot path,
and **read-only**: nothing in this surface can place, cancel, or flatten an
order. Operator controls remain on the audited ncurses hotkeys.

Design by Claude Design; implemented on the `feature/web-ui` branch.

---

## Quick start

```bash
# Build with the web server (fetches civetweb; websockets on, no TLS/Boost)
cmake -B build -DENABLE_WEB=ON
cmake --build build -j$(nproc)

# Build the SPA bundle (Node 18+); outputs to src/web/assets/
cd src/web/frontend && npm ci && npm run build && cd -

# Live cockpit (shadow): serve the bundle + stream live snapshots
./build/engine_shadow --provider binance --symbol btcusdt --stream trade \
  --web --web-port 8080 --web-assets src/web/assets
# → open http://127.0.0.1:8080/

# Backtest review: serves the final report until Ctrl-C
./build/engine_backtest --provider local --path data/market_data.csv --strategy sma \
  --web --web-assets src/web/assets
```

Frontend dev (hot reload, proxies to a running engine on :8080):

```bash
cd src/web/frontend && npm run dev   # http://localhost:5173
```

With no engine reachable, the UI falls back to a bundled fixture after ~2.5s and
shows a **Demo data** badge — useful for design work without a backend.

---

## Flags (`-DENABLE_WEB=ON` only)

| Flag | Default | Meaning |
|------|---------|---------|
| `--web` | off | Serve the read-only web UI for this session |
| `--web-port N` | `8080` | Listen port |
| `--web-bind ADDR` | `127.0.0.1` | Bind address — localhost by default; never `0.0.0.0` implicitly |
| `--web-token TOK` | none | Require a bearer token on `/api/*` and `/stream` (header `Authorization: Bearer TOK` or `?token=TOK`) |
| `--web-assets DIR` | none | Serve a built SPA bundle at `/`; omit for API/WS only |

---

## Endpoints

| Route | What |
|-------|------|
| `GET /` + assets | The SPA (when `--web-assets` is set) |
| `GET /api/snapshot` | Current `SnapshotFrame` (one-shot) |
| `GET /api/results` | `ResultsReport` — `engine.get_analytics().generate_report()` |
| `WS /stream` | Live `SnapshotFrame` JSON: one frame on connect, then at the poll rate (~10 Hz) |

Two screens: **Live Dashboard** (account, equity+drawdown, positions, open-lot
SL↔TP brackets, L2 ladder, fills tape, per-strategy cards, risk gauges, system
health) and **Backtest Review** (metric board, benchmark, equity-vs-benchmark
with brush + trade markers, sortable/filterable blotter, P&L histogram,
per-symbol / per-strategy breakdowns). Connection states: Connected · HALTED
(from `risk.halted`) · Reconnecting · Demo data (offline fixture).

---

## Architecture

The web layer is a third consumer of the engine's existing read-only seam — it
adds **no** hot-path work and touches **none** of the frozen live-safety files.

```
engine event loop ──(100ms)── dashboard_snapshot  ──snapshot_dashboard()──┐
                                                                          │
  src/web/ (ENABLE_WEB, own thread)                                       │
   • SnapshotPoller: snapshot_dashboard() → snapshot_to_json() ─ WS /stream
   • report_to_json(generate_report())           ────────────── REST /api/results
   • civetweb HTTP+WS server                                              │
                                                                          ▼
  src/web/frontend (React + Vite + TS)                          browser SPA
   • wire.ts        : TS types mirroring the emitted JSON
   • adapters/*     : engine wire shapes → component props (the only place
                      field-name / scale / semantic differences are resolved)
   • data/useLiveFeed, useResults : WS + REST hooks (fixture fallback offline)
```

- **C++** (`src/web/`): `snapshot_json.{h,cpp}` (`dashboard_snapshot` →
  `SnapshotFrame`), `report_json.{h,cpp}` (`AnalyticsReport` → `ResultsReport`,
  including the full trades blotter), `web_server.{h,cpp}` + `web_config.h`.
  JSON is hand-rolled (`json_emit.h`) — no nlohmann, no hot-path involvement.
  Serializers are faithful field-name projections of the structs.
- **Frontend** (`src/web/frontend/`): the Claude Design components, unchanged
  visually; only their data source moved to the adapted feed. Plain CSS design
  tokens (IBM Plex Sans/Mono, oklch palette) — no Tailwind.
- **Contract**: `src/web/frontend/src/fixtures/{snapshot,report}.json` are
  engine-shaped samples emitted by `src/web/tools/dump_fixtures.cpp`. The
  fixture module applies the adapters, doubling as a compile-time contract check
  (`tsc` fails if the serializer shape and the adapters drift apart).

### Scale conventions worth knowing

The serializers emit engine-native conventions; the adapters reconcile them:
position `qty` is signed (negative = short → split to side + abs); drawdown is a
positive percent (→ negative fraction for the chart); **win-rate is a percent
(0–100)** (`sub_analytics::win_rate()` × 100 → divided by 100 for display);
sides are single chars (`'L'`/`'S'`, `'B'`/`'S'`).

---

## Safety notes

- **Read-only by construction.** `web_server` registers no control routes on any
  target, including `engine_live`. The `read_only` flag is explicit at the
  callsite to make the intent visible.
- **localhost by default.** Binds `127.0.0.1`; expose beyond the host only behind
  a reverse proxy (TLS terminates there — civetweb is built without SSL).
- **Bounded subscribers.** `/stream` caps concurrent clients (`max_ws_clients`)
  so a connection flood can't stall the broadcast cadence.
- **No frozen-surface contact.** No edits to `engine.cpp`'s loop, `tt_target.h`,
  `*kill_switch*`/`*dead_mans_switch*`/`*reconciler*`, `src/threading/`, or
  `src/risk/`; no `LIVE_SAFETY_CCB_APPROVED` token required. Verified against
  `check-hotpath-json.sh` and `check-live-safety-freeze.sh`.

---

## Known limitations / follow-ups

- The SPA bundle is built manually (`npm run build`); there is no CMake target
  wiring `npm` into the C++ build yet, and `--web-assets` has no compiled-in
  default — pass the directory explicitly.
- No CI job yet exercises `-DENABLE_WEB=ON` (the repo has no `.github/workflows`
  in-tree at time of writing).
- `/stream` writes are serial under one lock (safe against civetweb's
  connection teardown); fine for a handful of localhost operator clients, not
  designed for many remote viewers.
- TLS is intentionally out of scope (reverse-proxy concern).
