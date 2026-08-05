# Footprint History & Live Display — Approved Implementation Plan

**Document type:** Decision-complete engineering plan  
**Status:** Product and architecture decisions approved; implementation and T3 verification pending  
**Last updated:** 2026-08-03  
**Audience:** Implementers, reviewers, verifier  
**Related:** [`cyrex/orderflow/02-footprint-chart.md`](cyrex/orderflow/02-footprint-chart.md) · [`cyrex/00-architecture.md`](cyrex/00-architecture.md) · [`docs/internal/imgui-desk-design.md`](docs/internal/imgui-desk-design.md)

---

## 1. Summary

Build a production-grade footprint subsystem for the configured perpetual-futures venue and symbol. It will collect normalized public trades without affecting trading behavior, reconcile cached/history/live data, and publish immutable viewport-sized presentations to the native desk at 20 Hz.

Defaults:

- Binance USD-M, Bitget USDT-M, and Bitunix futures.
- Current engine symbol only; no UI-driven resubscription.
- One-minute time bars, last 60 bars visible.
- Optional 1s, 5s, 15s, 5m, and quote-notional volume bars.
- 24-hour history request, seven-day maximum.
- Base/quote display toggle, tick grouping Auto or ×1/2/5/10/25.
- 300% diagonal imbalance, per-symbol minimum-volume suggestion, three-level stacked imbalance.
- UTC-configurable CVD reset, default 00:00 UTC.
- Automatic activation with `--desk` in `HAS_IMGUI_DESK` builds; `--no-footprint` will disable collection.
- No playback, multi-venue aggregation, dynamic symbol switching, QuestDB storage, or engine event-log integration in this release.

## 2. Implementation Changes

### 2.1 Public-trade ingress and venue capabilities

- Add a trivially copyable, fixed-size `PublicTrade` ingress record containing event/receive nanoseconds, pre-resolved venue and symbol IDs, native trade ID when available, exact integer price ticks, base-quantity atoms, aggressor side, observation sequence, and provenance flags.
- Enrich `provider::tick` with allocation-free native-ID and exact-decimal fields. Existing engine conversion will ignore the enrichment, preserving strategy and execution behavior.
- Add an optional DataBridge research tap. It may only convert the already-parsed tick into `PublicTrade` and `try_push` it into a prewarmed bounded SPSC ring. It must not allocate, lock, format, log, retry, aggregate, or block.
- Use one DataBridge producer and one `FootprintResearchService` consumer. Ring exhaustion will mark only research data as discontinuous and trigger recovery; it must never halt or slow the engine.
- Extend venue registration with optional `FootprintVenueCapabilities`, instrument-metadata, and `IPublicTradeHistorySource` factories. Do not add research methods to frozen `IProvider` or modify `engine.cpp`.
- Resolve tick size before streaming. Official metadata wins; if unavailable, accept a positive exact-decimal `footprint.tick_size_overrides.<venue>.<symbol>` configuration or `--footprint-tick-size` for the active symbol. Conflicting metadata and override will make the footprint unavailable rather than guessing.
- Keep the existing Binance raw-trade stream unchanged. Do not switch the engine to aggregate trades.

Venue behavior:

- **Binance:** Native raw-trade IDs for live data and exact raw history through the authenticated historical-trades endpoint when read-only credentials are available.
- **Bitget:** Native IDs for live and documented recent public fills; stitch when the recent range overlaps cache/live, otherwise expose the uncovered interval as a gap.
- **Bitunix:** Preserve each observed live print using connection-session ID plus observation sequence. Because its documented public trade payload has no trade ID or public trade-history endpoint, reconnects form explicit continuity boundaries and cannot be silently repaired.
- Credentials remain in existing provider configuration and are never written to the footprint cache.

This capability policy follows the documented venue surfaces: [Binance's official futures connector exposes raw, historical, and aggregate trade endpoints](https://github.com/binance/binance-futures-connector-python/blob/main/binance/um_futures/market.py); [Bitget live public trades include `tradeId`](https://www.bitget.com/api-doc/classic/contract/websocket/public/New-Trades-Channel), while its [futures fills endpoint is recent-data limited](https://www.bitget.com/api-doc/classic/contract/market/Get-Recent-Fills); Bitunix's [public trade channel omits a trade ID](https://www.bitunix.com/api-docs/futures/websocket/public/Trade%20Channel.html), although [instrument precision is available from trading-pair metadata](https://www.bitunix.com/api-docs/futures/market/get_trading_pairs.html).

### 2.2 History, cache, reconciliation, and aggregation

- Start live capture first, buffer incoming trades, load verified cache immediately, and publish it as `BACKFILLING`.
- Fetch missing history up to the live-start watermark, deduplicate native-ID venues, order trades through the two-second reorder window, merge buffered live trades, and atomically transition a contiguous result to `LIVE`.
- On disconnect, overflow, corrupt segment, missing overlap, or an arrival older than the reorder window:
  - Freeze the last verified contiguous presentation.
  - Continue buffering within fixed capacity.
  - Enter `RECOVERING`.
  - Attempt bounded cold-path repair respecting venue rate limits and `Retry-After`.
  - Atomically publish the repaired range or remain `STALE/PARTIAL` with the exact gap shown.
- Never mutate already-published historical bars silently.
- Represent data status separately from camera state: `UNAVAILABLE`, `BACKFILLING`, `LIVE`, `RECOVERING`, `PARTIAL`, `STALE`, and `REPLAY`; camera state is `FOLLOWING` or `DETACHED`.
- Store raw public trades in versioned zstd-compressed segments, rolling every five minutes or at a fixed maximum uncompressed size. Include schema version, venue/symbol, time range, record count, ID/session bounds, and checksum.
- Write active data to a recoverable partial segment; finalize and update the manifest through atomic replacement. Quarantine corrupt segments and report them instead of accepting partial contents.
- Enforce seven-day retention, 2 GiB per venue/symbol, and 8 GiB globally. Evict least-recently-used finalized segments, never the active segment or current visible range.
- Keep decoded history in a bounded cold-path working-set cache; aggregate requested viewport plus prefetch rather than materializing seven days of footprint cells.
- Raw-frame replay will use the same parser and aggregator but perform no REST requests and will not write into the live cache. End-of-stream flushes deterministic final bars.

Aggregation rules:

- Use integer tick arithmetic throughout; no floating-point price bucketing.
- Time bars use UTC-aligned half-open intervals and preserve empty intervals as `EMPTY`, distinct from `GAP`.
- Volume bars accumulate quote notional and close at or above the saved threshold. A venue trade is never split between bars.
- Suggest the first quote-volume threshold from median recent one-minute quote volume divided by six, then save it until manually changed.
- Suggest the imbalance minimum from recent nonzero footprint-cell base volumes, rounded to valid quantity precision; save it per venue/symbol rather than adapting continuously.
- Store sell-aggressor volume on the left and buy-aggressor volume on the right. Unknown aggression contributes to total volume and OHLC but not delta, CVD, or imbalance.
- Compute diagonal imbalance against the adjacent grouped price level, with 300% ratio and the saved minimum-volume gate. Mark stacked imbalance after three consecutive qualifying levels.
- POC is the highest total-volume price in each bar; ties resolve nearest the close and then toward the lower price.
- CVD is cumulative buy-aggressor minus sell-aggressor volume and resets at the saved UTC session boundary.
- Reconfiguration rebuilds the requested range on the cold worker and swaps it atomically when ready.

### 2.3 Desk presentation and controls

- Make the Orderflow workspace footprint-dominant:
  - Approximately 12% watchlist rail.
  - Approximately 17% right rail.
  - Approximately 12% bottom activity blotter.
  - Remaining 60–65% for footprint and CVD.
  - DOM receives roughly 70% of the right rail; selected context receives 30%.
- Keep the footprint as the primary canvas. CVD occupies a collapsible 18% lower sub-chart by default.
- Replace the demo canvas with a retained camera supporting:
  - Drag to pan.
  - Wheel for horizontal/time zoom.
  - Ctrl+wheel for price zoom.
  - Double-click to fit.
  - Home/End and `-`/`+`.
  - Crosshair with time, price, buy, sell, delta, total, and POC details.
- Manual pan or zoom changes the camera to `DETACHED`. Continue counting unseen bars and show `N NEW · GO LIVE`; never snap back automatically.
- Add a compact toolbar for bar type/interval, volume threshold, tick grouping, base/quote units, imbalance settings, CVD boundary, fit, follow, and data-status details.
- Draw forming bars distinctly from completed bars. Show POC, last-price line, CVD, diagonal imbalance, stacked imbalance, empty buckets, and hatched gap regions.
- Use full numeric cells only when legible. At lower zoom levels, progressively switch to delta shading, POC, bar outline, and volume summaries. Enforce a 12,000-cell draw budget and publish at 20 Hz with p95 source-to-presentation latency no greater than 75 ms.
- Materialize up to 512 bars around the requested viewport with prefetch. The service, not the renderer, supplies new windows when the user pans.
- Extend the desk research controller with viewport/settings commands and immutable `FootprintPresentation` snapshots. Keep commands off the engine thread.
- Persist settings atomically per venue/symbol: bar type, interval/threshold, tick grouping, units, imbalance parameters, CVD boundary, camera, and follow state.
- Link the footprint to future heatmap camera group A by symbol, visible time range, and price range. In volume-bar mode, link the actual start/end timestamps of visible bars. DOM shares symbol and focused price only.
- Show `BACKFILLING`, `RECOVERING`, `HISTORY UNAVAILABLE`, cache corruption, queue overflow, and continuity gaps both inline and as timestamped Operations alerts. No sound, order action, retry dialog, or engine halt.

## 3. Test and Verification Plan

- Golden aggregation tests for time/volume boundaries, empty buckets, unknown aggression, integer tick grouping, diagonal and stacked imbalance, deterministic POC ties, forming bars, CVD resets, and quote/base conversion.
- Venue fixture tests for native IDs, exact decimals, aggressor mapping, metadata, batched frames, Bitget overlap limits, Bitunix session boundaries, and Binance raw history/live equivalence.
- Reconciliation tests for REST/live overlap, duplicates, out-of-order arrivals, two-second lateness, disconnects, queue overflow, missing overlap, partial history, reconnect buffering, and failed recovery.
- Cache tests for crash recovery, truncated/corrupt segments, checksum rejection, manifest replacement, schema mismatch, retention, per-key/global LRU caps, and concurrent reader publication.
- Deterministic raw-frame replay tests with networking disabled and cache writes suppressed.
- Prove identical engine event/order output for the same input with footprint enabled and disabled.
- Add hot-path allocation and SPSC ownership tests; run ASAN for ring and publication lifetimes.
- Benchmark ingress, aggregation, 20 Hz publication, worst-case visible-cell rendering, and UI frame time. Require zero hot-tap allocations, no producer blocking, p95 publication latency ≤75 ms, and a 4–6 ms footprint render target at 2560×1440.
- Perform headless visual QA at 2560×1440 and 1920×1080 for `LIVE`, `BACKFILLING`, `DETACHED`, `RECOVERING`, `PARTIAL`, empty, and unavailable states.
- Run all required core checks, focused hot-path tests, full test suite, and T3 independent verifier/fact-checker/performance reviews. Register new sources explicitly in `cmake/Sources.cmake`.
- Keep all live-safety frozen files untouched. If implementation proves that a frozen interface must change, stop and split that work into the separately approved CCB/T3 process.

## 4. Assumptions

- Footprint data is observational and can degrade independently without changing live-order safety.
- Public market cache data is not encrypted; credentials and account fills are never stored in it.
- Bitunix's live/cached presentation is exact only for uninterrupted observed sessions and is labeled accordingly.
- Existing engine event-log replay and normal backtest desks remain outside this integration; only live/shadow streams and raw provider-frame replay feed the footprint.

