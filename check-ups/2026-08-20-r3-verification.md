# R3 verification record — authoritative risk accounting

Date: 2026-08-20
Baseline commit: `23be54e9eaeef5e242b318252667d69afb0aea42`
(`refactor(engine): routerpipeline auftragsattribution entkoppeln`)
Design/spec: [`docs/internal/r3-authoritative-risk-accounting.md`](../docs/internal/r3-authoritative-risk-accounting.md)

Commits under review:

| Commit | Subject |
|--------|---------|
| `7a5e427` | `feat(risk): autoritatives orderledger und mark-to-market risikobuchhaltung` |
| `8f4af4f` | `bench: r3 orderledger und risikoschnappschuss messen` |
| *(this commit)* | `docs: r3 risikobuchhaltung dokumentieren` |

The three commits are surgically staged: the tree also carries unrelated
in-flight work (L2 sequencing / execution-router / market-making strategy) in
some of the same files, and only the R3 hunks were committed. `7a5e427..fe03c34`
was checked out into a detached worktree and built + tested on its own to prove
the split leaves no dangling dependency — see §7.1.

---

## 1. Builds exercised

| Preset | Configuration | Purpose |
|--------|---------------|---------|
| `linux-tests` | Debug, `BUILD_TESTS=ON` | unit / property / integration / regression suites, `engine_backtest` |
| `linux-asan` | Debug, ASAN **and** UBSAN, `ENABLE_BINANCE=ON` | sanitizer run |
| `linux-benchmarks` | Debug + `ENABLE_DEBUG=ON` + `ENABLE_BENCHMARKS=ON` | order-path benchmarks and the A/B below |
| detached worktree at `591473f` (= this commit's parent content) | Debug, `BUILD_TESTS=ON` | proof the three commits build and pass without the unrelated uncommitted work in the tree |

## 2. Test results

`linux-tests` on the full working tree (R3 plus the unrelated in-flight work),
run as two processes (see §2.1):

```
--gtest_filter='-EngineStreaming*'   1442 tests, 0 failures   11.7 s
--gtest_filter='EngineStreaming*'      16 tests, 0 failures    1.0 s
                                     ----
                                     1458 tests, 0 failures
```

Same two runs in the detached worktree at the R3-only tip (R3 only — the in-flight
market-making suites are not part of these commits):

```
1367 tests, 0 failures
```

R3-specific suites (71 new tests):

| Suite | Tests | Covers |
|-------|-------|--------|
| `OrderLedger` | 19 | lifecycle matrix, partial fills, terminal states, duplicate fills, cancel/fill races, multi-order and multi-instrument aggregation, amend, reset |
| `RiskAccounting` | 18 | mark-to-market exposure, cost-basis independence, worst case for every position/pending combination, mark classification, equity completeness |
| `RiskEnforcement` | 22 | candidate vs. current+pending, hard inventory limit and its boundaries, risk-reducing exemption, portfolio aggregation, stale/missing marks, funding below/at/above, rule codes, analytics-counter regression, VaR removal |
| `RiskProperty` | 6 | ledger invariants over 300 generated lifecycles, terminal orders carry no pending exposure, cancel never grows worst case, reducing orders never grow inventory, exposure independent of cost basis, worst case brackets the position |
| `EngineRiskLedger` | 6 | the same properties through the real engine pipeline (routing, bar-delay, EOS expiry, mark-driven rejection) |
| `OrderTracker` (pre-existing) | 3 | unchanged; still green against the extended ledger |
| `RiskManager` (pre-existing) | 21 | unchanged except three funding tests that now set `funding_rate_known` |

### 2.1 Pre-existing full-suite hang (not R3)

Running the binary unfiltered stalls in the `EngineStreaming` prequeued-funding
tests (`PrequeuedFundingIsPersistedInInlineEventLog` /
`PrequeuedFundingReachesEveryThreadedAnalyticsAndDurableLog`): one worker thread
spins at 100 % CPU, the main thread parks in `futex_wait`. Both pass in under a
second when the process starts with `--gtest_filter='EngineStreaming*'`.

R3 touches the funding path (`Analytics::on_funding` derives the 8h rate), so
attribution was checked rather than assumed: with that derivation commented out
and the binary rebuilt, the unfiltered run stalls at the identical test. The
defect is an ordering/state-pollution issue that predates this work.

## 3. Sanitizers

`linux-asan` (ASAN + UBSAN, `detect_leaks=1`, `print_stacktrace=1`):

```
R3 suites only            147 tests, 0 failures, 0 sanitizer diagnostics
--gtest_filter='-EngineStreaming*'
                         1675 tests, 2 failures, 0 sanitizer diagnostics   40.6 s
```

Zero ASAN / UBSAN / LeakSanitizer diagnostics. The two failures are outside R3
and outside this change's file set:

- `BinanceDepthParser.RejectsWholeFrameOnUnsafeLevel` — belongs to the
  uncommitted depth-parser work already present in the tree
  (`src/providers/binance/binance_depth_parser.h`); the `linux-tests` preset
  does not build Binance, which is why it only shows here.
- `BinanceFuturesDeadMansSwitch.LivenessTsAdvancesOnHeartbeat` — clock-granularity
  flake under ASAN (`5236741 vs 5236741`).

## 4. Gate scripts

```
./scripts/check-hotpath-json.sh   OK (nlohmann/json confined to the allow-list)
./scripts/check-layer-deps.sh     OK
./scripts/check-mm-reference.sh   OK (unrelated, run because Sources.cmake changed)
./scripts/check-live-safety-freeze.sh
```

The freeze script reports one remaining dirty protected file,
`src/engine/engine_lifecycle.cpp` (`l2_sequence_states_.clear()` in
`reset_for_next_trial`) — pre-existing uncommitted work, not part of R3. Every
frozen file R3 modified is committed in `7a5e427`, which carries the
`LIVE_SAFETY_CCB_APPROVED` token. R3 additionally puts
`src/execution/order_tracker.h`, `src/execution/mark_point.h` and
`src/risk/risk_accounting.h` under the freeze, in both the script and
`AGENTS.md` §3.

## 5. Benchmarks

`linux-benchmarks` (Debug + instrumentation — absolute numbers are
Debug-relative and only meaningful as before/after on this preset).

| Benchmark | Time | Notes |
|-----------|------|-------|
| `BM_RiskLedger_Lifecycle` | 1343 ns | register → pending → open → partial fill → fill, incl. symbol aggregates |
| `BM_RiskView_Build/1` | 1133 ns | one instrument |
| `BM_RiskView_Build/4` | 4384 ns | four instruments, four resting orders each |
| `BM_RiskView_Build/16` | 22868 ns | sixteen instruments — confirms O(#symbols), not O(#orders) |

### 5.1 Engine-throughput A/B

`BM_Engine_Throughput_100k`, three runs each, same binary, the only difference
being whether the R3 order-path work runs (ledger registration +
`build_risk_view`) or the pre-R3 `marked_account_equity` pass does:

| Variant | bars/sec (3 runs) | mean |
|---------|-------------------|------|
| with R3 | 8.18 k / 8.48 k / 8.63 k | 8.43 k |
| R3 order-path work disabled | 8.23 k / 8.32 k / 8.25 k | 8.27 k |

Run-to-run spread is ≈ 6 %, larger than the ≈ 2 % difference between the two
variants, and the R3 variant is nominally the faster of the two. The honest
reading is **no measurable throughput regression**, not a speed-up. Structurally
that is expected: the order path gained one O(#symbols) pass and lost another
one plus a mutex acquisition, because the account equity is now taken from the
same snapshot instead of a second walk over the position map.

## 6. End-to-end CLI exercise

```
engine_backtest --provider synthetic --strategy sma --seed 424242 \
  --no-pin --status-format off --no-tui --output run.json
    -> 84 trades, final equity 6994.02

  ... --max-inventory-qty 1.0 --max-mark-age-ms 60000
    -> 84 trades, final equity 6994.02   (cap above the strategy's sizes: non-binding)

  ... --max-inventory-qty 0.0001
    -> 0 trades, final equity 10000.00   (cap binds; every entry refused)
```

The third run is the proof that the new flag reaches the engine and that the
hard inventory limit blocks inventory-increasing orders.

## 7. Commit hygiene

### 7.1 Split verification

Four files R3 had to touch (`src/engine/engine.h`, `engine.cpp`,
`engine_market.cpp`, `engine_pending.cpp`) already carried uncommitted L2
sequencing / execution-router work, and three more (`cmake/Sources.cmake`,
`benchmarks/bench_main.cpp`, `docs/00-INDEX.md`) carried uncommitted
market-making entries. Only the R3 hunks were staged; the rest stayed in the
working tree.

A first attempt committed those files wholesale, which both bundled other
people's in-flight work into the R3 commit and left `HEAD` unbuildable (the
committed `engine.h` referenced `provider::l2_delta_batch`, which lives in a
still-uncommitted header). That was caught by building the commits in a
detached worktree, and the commits were redone surgically. The worktree build
+ test run above is the standing proof that the split leaves no dangling
dependency.

One R3-adjacent line is deliberately **not** committed: the mark-timestamp
adaptation inside `engine::apply_l2_delta_batch`. That function does not exist
at the baseline commit — it belongs to the in-flight L2 work — so its one-line
adaptation to the new `mark_point` value type stays in the working tree and
belongs to whoever commits that function.

## 8. Residual risks

1. **No dedicated funding-rate feed.** The 8h rate is derived from settlements,
   so it is a *realized* rate that only refreshes when funding settles (every
   8 h on Binance USDT-M) and only for symbols that already hold a position. A
   venue funding-rate stream would give a forward-looking rate; the setter
   (`Analytics::set_current_funding_rate_8h`) is already the seam for it.
2. **No account-level cross-margin ratio.** Deliberate — see the design note §8
   and R-04.
3. **Late fills after cancel** remain a documented venue-level residual risk
   (unchanged by R3). The ledger books such a fill's quantity, which is the
   correct accounting, and re-opens the order if quantity remains outstanding.
4. **Ledger memory is O(orders ever seen)** — one ~64-byte record per order id
   for the life of a run, as before R3 (which kept an `id -> status` entry with
   the same lifetime). `reset()` clears it at Monte-Carlo trial boundaries.
5. **`SymbolTable::kMaxSymbols` = 256.** Beyond that the ledger stops producing
   per-symbol aggregates; the risk path fails closed on the affected symbol
   (`exposure_tracked == false` → reject inventory-increasing orders) rather
   than reading it as flat.
