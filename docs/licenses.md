# Third-party licenses

This document is the authoritative list of every third-party component the
TrueTest engine links into any of its three shipping binaries
(`engine_backtest`, `engine_shadow`, `engine_live`), the shared library
(`libtruetest.so`), the test binary, or the benchmark binary.

Acquisition for every entry is pinned — see `cmake/Dependencies.cmake` for
exact tag / version. This doc must be updated in the same PR that adds,
removes, upgrades, or re-scopes a dependency. CI does not enforce that
today (tracked as a Step 11 follow-up).

## Shipped in every binary (always linked)

| Component     | Version   | License            | Link mode | engine_live permissible? |
|---------------|-----------|--------------------|-----------|--------------------------|
| CLI11         | v2.4.2    | BSD-3-Clause       | static    | yes                      |
| zstd          | v1.5.6    | BSD-3-Clause / GPLv2 dual | static (BSD chosen) | yes |
| nlohmann/json | v3.11.3   | MIT                | header-only | yes — config-file parse only; never hot-path |

Justification: CLI11 is header-only, zstd is the binary event-log codec
(compression only, no network I/O), nlohmann/json is the CLI config / C-API
serialization surface. None of these three touch credentials or the hot path.

## Shipped when `ENABLE_SQLITE=ON` (default)

| Component | Version            | License                  | Link mode | engine_live permissible? |
|-----------|--------------------|--------------------------|-----------|--------------------------|
| SQLite3   | system (find_package) | Public domain (SQLite blessing) | dynamic | yes |

## Shipped only when an ENABLE_* flag is set

| Component        | Flag                 | Version / source        | License               | Link mode | engine_live permissible? |
|------------------|----------------------|-------------------------|-----------------------|-----------|--------------------------|
| Boost (headers)  | `ENABLE_WEB_UI` / `ENABLE_BINANCE` | find_package      | BSL-1.0               | header-only | yes               |
| Boost.System     | `ENABLE_LIVE_DATA`   | find_package            | BSL-1.0               | dynamic   | yes                      |
| OpenSSL          | `ENABLE_BINANCE`     | find_package            | Apache-2.0 (3.x)      | dynamic   | yes — required for HMAC signing |
| libpqxx          | `ENABLE_POSTGRESQL`  | 7.9.2 (FetchContent)    | BSD-3-Clause          | static    | yes                      |
| libpq / PostgreSQL client | `ENABLE_POSTGRESQL` | find_package / vcpkg | PostgreSQL license | dynamic | yes |
| Abseil           | `ENABLE_DEBUG`       | 20240722.0 (FetchContent) | Apache-2.0          | static    | no — debug-only; must not ship in live |

## Test / benchmark binaries only (not shipped)

| Component         | Version   | License       | Scope                         |
|-------------------|-----------|---------------|-------------------------------|
| GoogleTest        | v1.15.2   | BSD-3-Clause  | `BUILD_TESTS=ON` test binaries |
| Google Benchmark  | v1.8.5    | Apache-2.0    | `ENABLE_BENCHMARKS=ON` only    |

## Rules of the road

1. **New deps** require a line in this table plus a `cmake/Dependencies.cmake`
   pin. No vendored source trees under `third_party/` — every entry is either
   `FetchContent` with an exact tag or `find_package` against a system
   library.
2. **Copyleft (GPL, AGPL, LGPL, etc.)** must not enter the tree. If a
   candidate dep is dual-licensed, document which license we elect here
   (zstd is the existing precedent — we link it under the BSD arm).
3. **`engine_live` permissibility** is a separate column because a few
   components (notably Abseil under `ENABLE_DEBUG`) are authorized for
   developer builds but must not be in the live binary. The
   `target_allows_live_orders()` gate and the default-off ENABLE_* flags
   keep that invariant today; a future step will add a CI job that builds
   `engine_live` with `-DENABLE_DEBUG=OFF` explicitly asserted.
4. **Hot-path JSON is off-limits** for `nlohmann/json` — see
   `scripts/check-hotpath-json.sh` and the allow-list it enforces.
