# Plan 02 — Data provenance and integrity

## Scope

`DataBridge`, `CsvBarParser`, `MarketSeries`, sorting, timestamp resolution, and
the engine's `bar_at(i)` boundary.

## Required work

1. Carry physical CSV row, accepted index, stored index, engine index, symbol,
   timestamp, and rejection reason through the complete pipeline.
2. Distinguish parser rejection from domain validation rejection.
3. Validate complete OHLC geometry (`low <= open/close <= high`) or explicitly
   model exceptions; never silently accept and later skip them.
4. Detect and report duplicate symbol/timestamp records and out-of-order input.
5. Make bar-open timestamp and decision timestamp explicit configuration fields.
6. Add tests for repeated headers, malformed values, missing fields, duplicates,
   equal timestamps, invalid OHLC, sorting, and symbol filtering.

## Required report

```text
physical row → parser result → validation → stored index → engine index
```

Include input digest, accepted/rejected counts by reason, time range, interval,
symbols, and whether sorting changed order.

## Acceptance criteria

- Every strategy-visible bar has deterministic provenance.
- Every rejected row has a physical row and reason.
- No silent bar removal after parsing.
- The strategy receives exactly the bars listed in the provenance report.

## Evidence status

Implemented first slice:

- `CsvBarParser` now records one-based physical rows, zero-based parser-accepted
  indexes, parser stage, typed rejection reason, and source field. These indexes
  are local to one parser/file session and reset when a new header is parsed.
- Required OHLC tokens are strict (no trailing garbage or manufactured zero for
  a missing value), malformed explicit timestamps are rejected, and a new
  header resets all schema and row state.
- `MarketSeries` now enforces `low <= open/close <= high` and exposes the last
  typed domain-validation reason.

These parser and domain-validation policies are `CONFIRMED` by focused tests.
Complete row-to-stored-index provenance remains `UNVERIFIED`: the frozen
`DataBridge` currently converts `bar_record` to `Bar` without forwarding source
identity. Duplicate/out-of-order reporting, input digest/report aggregation,
filter/sort provenance, and actual engine-dispatch indexes also remain
`UNVERIFIED`. Explicit bar-open/decision timestamp configuration and invalid
fallback-date handling also require later slices.

## References

- `src/providers/data_bridge.h`
- `src/providers/local/csv_parser.h`
- `src/data/market_series.cpp`
- `docs/internal/data-pipeline.md`
