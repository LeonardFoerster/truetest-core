# Bitunix Futures Provider — Implementation Guide

**Registry names:** `bitunix-futures` (canonical), `bitunix` (alias)  
**Code path:** `src/providers/bitunix/`  
**CMake:** `-DENABLE_BITUNIX=ON`, preset `linux-bitunix`  
**Status (2026-08-02):** **Phase 0–1 landed** — MD + paper/shadow; **live refused**

---

## 1. Goals

| Priority | Goal |
|----------|------|
| P0 | USDT-M linear perps (`BTCUSDT`) as `IProvider` with public WS trade MD |
| P1 | Paper (`LocalBookAdapter`) + shadow (`TradeTapeShadowAdapter`) |
| P2 (deferred) | Live orders, private WS, reconciler, kill, DMS, brackets |

**v1 success:** `--provider bitunix-futures --symbol BTCUSDT --stream trade` works in `engine_shadow` / backtest-with-stream paths. Live open prints a clear refuse message.

---

## 2. Venue map (API research)

| Item | Value |
|------|--------|
| REST | `https://fapi.bitunix.com` |
| Public WS | `wss://fapi.bitunix.com/public/` |
| Private WS | `wss://fapi.bitunix.com/private/` (deferred) |
| Auth | Double SHA-256 (see `bitunix_auth.h`) |
| Trade subscribe | `{"op":"subscribe","args":[{"symbol":"BTCUSDT","ch":"trade"}]}` |
| Ping | `{"op":"ping","ping":<unix_sec>}` |
| Symbol | `BTCUSDT` uppercase |

Docs: https://www.bitunix.com/api-docs/futures/

---

## 3. Non-negotiables

| ID | Rule |
|----|------|
| S1 | No `HAS_BITUNIX` outside `src/providers/bitunix/` + CMake |
| S2 | Live only in `engine_live` when Phase 2+ lands — Phase 0–1 refuses live |
| S3 | Hand parsers; no `nlohmann` on hot path |
| S4 | Halt terminal; no safety retry |
| S5 | Layer deps green |

---

## 4. Files

```
src/providers/bitunix/
  bitunix_auth.h
  bitunix_endpoints.h
  bitunix_parser.h
  bitunix_transport.h
  bitunix_futures_provider.h
  bitunix_futures_register.cpp
tests/providers/bitunix/
  test_bitunix_auth.cpp
  test_bitunix_endpoints.cpp
  test_bitunix_parser.cpp
  test_bitunix_futures_register.cpp
```

---

## 5. CLI / env

```bash
cmake --preset linux-bitunix && cmake --build --preset linux-bitunix
./out/build/linux-bitunix/engine_shadow \
  --provider bitunix-futures --symbol BTCUSDT --stream trade \
  --mode shadow --no-pin --status-format off --no-tui
```

Credentials reserved (unused until live):

- `TRUETEST_BITUNIX_API_KEY`
- `TRUETEST_BITUNIX_API_SECRET`

---

## 6. Phase checklist

### Phase 0 — done
- [x] Scaffold + register
- [x] Auth double-SHA256 unit tests
- [x] Endpoints + subscribe helpers

### Phase 1 — done
- [x] Public trade parser (multi-tick `parse_records`)
- [x] Public WS transport + subscribe
- [x] Provider open for paper/shadow; live refuse

### Phase 2–4 — not started
- [ ] REST order transport + encoder
- [ ] Private WS login / fills
- [ ] Reconciler / kill / DMS
- [ ] Freeze list promotion after live hardening

---

## 7. Gaps / residual risks

| Risk | Note |
|------|------|
| No confirmed public demo/testnet | Treat mainnet MD carefully; live deferred |
| Trade timestamp ISO parse | Falls back to frame `ts` / wall clock |
| Depth/kline | Channel map stubs only; trade is the production path |
| DMS | Unknown / not wired — document before live |

---

*Last updated: 2026-08-02*
