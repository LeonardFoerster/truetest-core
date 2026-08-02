# Upcoming Platforms — Multi-Venue Provider Integration

**Status (2026-08-02):** Product venues on `master`: **Binance** (golden), **Bitget** (landing), **Bitunix** (Phase 0–1 planned).  
**Bybit / Gate.io** full trees are **not** on `master` — archived at git tag `archive/provider-2026-07-30`.

**Audience:** Agents implementing new CEX providers for TrueTest `core/`.

---

## Active product venues

| Venue | Registry names | Focus | Tree |
|-------|----------------|-------|------|
| **Binance** | `binance`, `binance-futures` | Spot + USDT-M futures (golden path) | `src/providers/binance/` |
| **Bitget** | `bitget`, `bitget-futures` | UTA USDT-M | `src/providers/bitget/` (port in progress) |
| **Bitunix** | `bitunix`, `bitunix-futures` | USDT perps | `src/providers/bitunix/` (Phase 0–1) |

## Archived / not in tree

| Venue | Recovery | Notes |
|-------|----------|-------|
| **Bybit** (full Phase 0–4) | `git show archive/provider-2026-07-30:src/providers/bybit/` | Parked off master by design |
| **Gate.io** (MD + safety partial) | `git show archive/provider-2026-07-30:src/providers/gate/` | Parked; live orders were never wired |
| Guides for OKX / Kraken / etc. | May exist only on archive tag under `docs/upcoming_platform/` | Not started on master |

Thin Phase-0 stubs that previously sat on `master` under `src/providers/bybit/` and `src/providers/gate/` were **removed** so the tree only claims venues that ship.

---

## Documents (master)

| File | Venue | Status |
|------|-------|--------|
| [bitget.md](./bitget.md) | Bitget | Design/DoD (copied with provider port) |
| [bitunix.md](./bitunix.md) | Bitunix | Phase 0–1 guide (when added) |
| Ops: [../operations/03-bitget-demo.md](../operations/03-bitget-demo.md) | Bitget demo SOP | When present |

---

## Non-negotiables (every venue)

| Rule | Meaning |
|------|---------|
| Sole extension | `IProvider` + registry only; no `HAS_*` in `engine/`, `core/`, `threading/` |
| Live gate | Live orders only in `engine_live` (`TT_TARGET`) |
| Halt | Terminal write-once; no retry on kill/DMS/reconciler |
| Hot path | No `nlohmann::json`; hand parsers; zero heap on hot path |
| Layers | `./scripts/check-layer-deps.sh` green |

Golden reference: `src/providers/binance/*` (especially `binance_futures_*`).

---

## CMake pattern

```cmake
option(ENABLE_<VENUE> "Build with <Venue> provider" OFF)
# Dependencies.cmake: target_sources(..._register.cpp) + HAS_<VENUE>
```

Presets: `linux-bitget`, `linux-bitunix` (as added). Default `linux-tests` stays Binance-optional.

---

## Recovery of archived Bybit/Gate

```bash
git fetch --tags
git checkout archive/provider-2026-07-30 -- src/providers/bybit   # only if resurrecting
# Prefer a new branch from current master + selective checkout, then re-wire DataBridge.
```

Do **not** merge the whole `provider` branch into master — it predates MarketSeries and diverges on engine/data paths.

*Last updated: 2026-08-02 — venue cleanup (binance + bitget + bitunix).*
