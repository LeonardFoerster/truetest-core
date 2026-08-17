# Web UI

A browser dashboard for TrueTest — a live trading cockpit and a backtest-review
report, rendering the same data as the ncurses TUI. Opt-in, off the hot path,
and **read-only**: nothing in this surface can place, cancel, or flatten an
order. Operator controls remain in the ncurses and ImGui surfaces.

Implemented as the optional `ENABLE_WEB` read-only surface. The ImGui desk is the primary in-process human cockpit; this browser surface remains available for snapshot/report viewing.

---

## Quick start

```bash
# Build with the web server (fetches civetweb; websockets on, no TLS/Boost)
cmake --preset linux-web -DENABLE_BINANCE=ON
cmake --build --preset linux-web --target engine_shadow engine_backtest web_assets

# The explicit web_assets target runs npm ci + npm run build when npm is available.

# Live cockpit (shadow): serve the bundle + stream live snapshots
./out/build/linux-web/engine_shadow --provider binance --symbol btcusdt --stream trade \
  --web --web-port 8080 --web-token replace-with-a-local-token \
  --web-assets src/web/assets
# → open http://127.0.0.1:8080/?token=replace-with-a-local-token

# Backtest review: serves the final report until Ctrl-C
./out/build/linux-web/engine_backtest --provider local --path market_data.csv --strategy sma \
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

**SnapshotFrame schema:** `schema_version` **2** (additive over v1). New blocks for
engine telemetry (Trading desk / operator board):

| Key | Contents |
|-----|----------|
| `memory` | `/proc` RSS/heap when available; pool/ring byte footprints |
| `debug` | `target`/`mode`/`preset`, `worker_count`, ring size/HWM/drops, pool in-use, subsystem errors; optional `stages` on `HAS_DEBUG` builds |

The monorepo Next.js UI does **not** call these endpoints directly — the Fastify
backend proxies them (`CORE_WEB_URL` → `GET /api/engine/snapshot`). Research jobs
still use headless `--status-format off` and report JSON; this stream is for an
attended `engine_* --web` session.

Two screens: **Live Dashboard** (account, equity+drawdown, positions, open-lot
SL↔TP brackets, L2 ladder, fills tape, per-strategy cards, risk gauges, system
health) and **Backtest Review** (metric board, benchmark, equity-vs-benchmark
with brush + trade markers, sortable/filterable blotter, P&L histogram,
per-symbol / per-strategy breakdowns). Connection states: Connected · HALTED
(from `risk.halted`) · Reconnecting · Demo data (offline fixture).

---

## Architecture

The web layer is a third consumer of the engine's existing read-only seam. It
polls the existing snapshot seam off the per-event path.

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
  target, including `engine_live`. `web_config::read_only` defaults to true;
  the server exposes no control handlers.
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

- The SPA bundle has an explicit `web_assets` CMake target when npm is found,
  but it is deliberately not part of the default C++ build. `--web-assets` has
  no compiled-in default, so pass the directory explicitly.
- `/stream` writes are serial under one lock (safe against civetweb's
  connection teardown); fine for a handful of localhost operator clients, not
  designed for many remote viewers.
- TLS is intentionally out of scope (reverse-proxy concern).
