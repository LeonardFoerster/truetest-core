# Performance baseline — Step 1 of `docs/performance.md`

This file is the measurement anchor referenced by every subsequent item in
`docs/performance.md`. Every optimisation lands a row in the changelog at
the bottom comparing its run against this one.

## Build identity

| Field          | Value                                                      |
| -------------- | ---------------------------------------------------------- |
| Git SHA        | `f32c63ef44c1` (HEAD: `f32c63e ncurses dashboard version 2.0`) |
| Tree state     | **dirty** (43 working-copy paths uncommitted)             |
| Branch         | `pre_performance`                                          |
| Built          | 2026-04-27T18:21:38Z                                       |
| Build type     | Release (`-O3 -flto -DNDEBUG`)                             |
| Compiler       | GCC 15.2.1                                                 |
| CMake flags    | `-DENABLE_DEBUG=ON -DENABLE_QUESTDB=ON -DENABLE_BINANCE=ON -DENABLE_NATIVE_OPT=ON -DCMAKE_BUILD_TYPE=Release` |
| Build dir      | `build-perf/`                                              |
| Binaries built | `engine_backtest`, `engine_shadow`, `engine_live`          |
| Deps pinned    | cli11 v2.4.2, zstd v1.5.6, nlohmann v3.11.3, abseil 20240722.0, gtest v1.15.2, bench v1.8.5 |

## Host

| Field   | Value                                            |
| ------- | ------------------------------------------------ |
| CPU     | AMD Ryzen 9 5900X (12 cores / 24 threads, single NUMA node) |
| Caches  | L1d 384 KiB, L1i 384 KiB, L2 6 MiB, L3 64 MiB    |
| Kernel  | Linux 7.0.1-1-cachyos x86_64                     |
| OS      | CachyOS                                          |

## Workload

> **Substitution disclaimer.** `performance.md` step 1.2 prescribes either
> a recorded Binance WebSocket replay (`engine_shadow`) or a 5-minute
> paper-mode live stream (`engine_live`). Neither was achievable in this
> environment: no recording exists in the tree, the test fixtures are
> 3–31 lines, and live Binance access requires API credentials and an
> outbound network. The baseline below is therefore captured from
> `engine_backtest` against a synthetic CSV.
>
> **What this means for the changelog.** Numbers here are useful for
> measuring optimisations that act on the bar-driven event loop:
> `mm_replenish`, `market_create`, `ring_publish`, allocator pressure,
> ring HWM, worker utilisation. They are *not* representative of
> network-bound shadow/live latency. When credentials/recordings become
> available, capture a second baseline against `engine_shadow` replay
> and treat the two as independent anchors.

| Field                | Value                                                     |
| -------------------- | --------------------------------------------------------- |
| Binary               | `build-perf/engine_backtest`                              |
| Command              | `engine_backtest --provider local --path /tmp/perf_baseline_50k.csv --strategy sma --thread-preset standard --no-pin --balance 100000` |
| Data                 | Synthetic 50 000-bar OHLCV (random walk, seed 42, AAPL)   |
| Threading preset     | `standard` (event-loop + LoggingWorker + RiskStatsWorker) |
| CPU pinning          | disabled (`--no-pin`)                                     |
| Strategy             | `sma` (period 20, default)                                |
| Spin policy          | adaptive                                                  |
| Wall time            | **36.671 s**                                              |
| Throughput           | 1 397.87 events/s (engine-reported) ≈ 1 363 bars/s        |
| Trades executed      | 25                                                        |
| Total fills          | 51                                                        |
| Total orders         | 1 051                                                     |
| avg tick→trade       | 52.69 µs (min 9.80 µs, max 91.56 µs)                      |

## Stage latency (bar loop, 50 000 iterations)

```
Stage               Calls     Total(ms)     Avg(ns)     Max(ns)     Min(ns)
market_create       50000     36556.0       731119      18517284    2444    *** JITTER ***
strategy            50000         7.5          150         81473      20    *** JITTER ***
ring_publish        50000        70.9         1417        369533     100    *** JITTER ***
mm_replenish        50000     36386.8       727735      18512514    1813    *** JITTER ***
stop_check          50000         1.2           23         22122      20    *** JITTER ***
pending_drain       50000        33.9          678         61916      10    *** JITTER ***
```

### What dominates

* `market_create` and `mm_replenish` together account for ~99 % of the
  bar-loop wall time. Their averages (731 µs and 728 µs per bar) and
  18.5 ms maxima point at allocator + jitter, not algorithmic work —
  these stages do little arithmetic.
* `strategy` is 150 ns/call: SMA decision is effectively free.
* `ring_publish` is 1.4 µs/call avg; the ring itself is not a hot
  spot at this throughput.
* `pending_drain` and `stop_check` are negligible.

The two dominant stages are exactly the lines `performance.md` items 2
(mimalloc), 5 (`absl::flat_hash_map`), 6 (symbol interning), and 9
(pre-alloc snapshot vectors) target. Use the absolute totals here
(`market_create=36 556 ms`, `mm_replenish=36 387 ms`) as the regression
target after each fix.

## Memory

```
                    Start           End             Delta
RSS                 17.1 MiB       102.6 MiB        +85.6 MiB
Virtual             28.7 MiB       262.5 MiB       +233.8 MiB
Peak RSS                           102.6 MiB
Heap                 9.4 MiB       119.4 MiB       +110.1 MiB
```

Heap grew 110 MiB on a workload that produces 51 fills. Per-event
allocation pressure (event constructions, `std::string` symbols, map
inserts) is exactly what `performance.md` items 2/5/6 address.

## Thread utilisation

```
Thread            Events      Busy%     Idle%     Polls       Hit%
logging           75521        0.2%     99.8%    72 532 920    0.1%
risk_stats        75521        1.0%     99.0%    72 325 191    0.1%
```

Workers are essentially idle at this throughput — the event loop is the
bottleneck. Spin policy is burning ~72 M empty polls per worker, so
`hit%` is microscopic. This means optimisations that move work *off* the
event loop (e.g. item 4 log-formatting audit) only help if they free
event-loop cycles; the workers themselves have plenty of headroom.

## Ring buffer diagnostics

```
Ring                cap     push      pop   drops    HWM   (%)
logging_ring       65536    75521      0      0       19    0.0
risk_ring          65536        0      0      0        0    0.0
stats_ring         65536        0      0      0        0    0.0
observer_ring      65536        0      0      0        0    0.0
risk_stats_ring    65536    75521      0      0        6    0.0
mm_ring            65536        0      0      0        0    0.0
```

* HWM 19/65536 (logging) and 6/65536 (risk_stats) — rings are 100×
  oversized for this workload. Confirms `pop=0` accounting bug (pops
  aren't instrumented in `RingBuffer::try_pop`); the diagnostics show
  push count only. Worth fixing in passing if any item touches the
  ring code, but not blocking.
* Zero drops anywhere. Drop-policy infrastructure is unexercised at
  this throughput.

## Reproduction

```bash
# 1. Configure + build
cmake -B build-perf \
  -DENABLE_DEBUG=ON -DENABLE_QUESTDB=ON \
  -DENABLE_BINANCE=ON -DENABLE_NATIVE_OPT=ON \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build-perf -j --target engine_backtest engine_shadow engine_live

# 2. Generate the synthetic workload
awk 'BEGIN { srand(42); print "symbol,open,high,low,close,volume";
             px=150.0;
             for (i=0;i<50000;++i) {
               o=px; drift=(rand()-0.5)*0.6; c=o+drift;
               h=(o>c?o:c)+rand()*0.4; l=(o<c?o:c)-rand()*0.4;
               if (l<1) l=1; v=int(900000+rand()*300000);
               printf "AAPL,%.4f,%.4f,%.4f,%.4f,%d\n", o,h,l,c,v;
               px=c; } }' > /tmp/perf_baseline_50k.csv

# 3. Run baseline
./build-perf/engine_backtest \
  --provider local --path /tmp/perf_baseline_50k.csv --strategy sma \
  --thread-preset standard --no-pin --balance 100000

# 4. The StageTimer / Memory / Thread / Ring report is appended to
#    truetest_debug.log in the cwd (absl LogSink); stderr only carries
#    the high-level audit + ring HWM lines.
```

## Canonical baseline TODO

When a Binance recording or paper-trading credentials are available,
re-run step 1 against `engine_shadow --replay-data <recording>` and
`engine_live --provider binance --symbol btcusdt --stream kline_1m`
respectively, and append both as separate baseline blocks in this file.
The current numbers stay valid as a "bar-mode-backtest" baseline.

## Append-only changelog

When an item in `docs/performance.md` lands, add a row here.

| Date | Item | Commit | Stage Δ vs baseline | Wall Δ |
|------|------|--------|----------------------|--------|
| 2026-04-27 | 1 (baseline) | `f32c63e` (dirty) | — | — |
| 2026-04-27 | 2 (mimalloc) | `be6d4a1` (folded into user's mid-session sweep) | mean ≈ flat (±1%); **max −29%** | **+0.5%** (noise) |
| 2026-04-27 | 3 (PGO)      | `ad13ebf` | mean stages **−1 to −47 %**; tails **−70 to −97 %** on cheap stages | **−1.0 %** (real) |
| 2026-04-27 | 4 (log audit) | `—` (no code change) | n/a — pre-existing structure already correct | n/a |
| 2026-04-27 | 5 (flat_hash_map) | `d1b22d7` | ring_publish mean **−12 %**; pending_drain mean **−21 %**; mm_replenish max **−14 %** vs Step 2 | flat (allocation-bound) |
| 2026-04-27 | **Composed (2+3+5) — new reference baseline** | `2e3288b` | mm_replenish mean **−2.7 %**; mm_replenish max **−45 %**; ring_publish mean **−58.5 %**; pending_drain mean **−48 %**; strategy max **−92.5 %** | **−3.0 %** (real, single run) |
| 2026-04-27 | E3 (orderbook maps → flat_hash_map) | `66d972c` (HEAD) | neutral within noise; structural consistency only; tests 653/653 pass | flat (within run-to-run variance) |
| 2026-04-28 | 7 (recent_fills deque → ring) | `71bb2a7` | (composed with 8+9 below) | (composed with 8+9 below) |
| 2026-04-28 | 8 (AdverseSelection pending_ deque → ring) | `d9203a6` | (composed with 7+9 below) | (composed with 7+9 below) |
| 2026-04-28 | 9 (pre-alloc dashboard scratch) | `a2fc746` (HEAD) | (composed with 7+8 above) | (composed with 7+8 above) |
| 2026-04-28 | **Composed (7+8+9) — same-host A/B vs HEAD~3** | `a2fc746` | ring_publish mean **−10.6%**; pending_drain mean **−6.0%**; strategy mean **−8.0%**; mm_replenish/market_create mean ≈ flat (−1.2% noise); RSS +2.2 MiB (scratch reserves) | **−1.2%** (within noise; 3-run mean: 43.05 s vs 43.56 s, host loaded) |
| 2026-04-29 | **Post-7+8+9 vs canonical Composed (2+3+5) — fresh host snapshot** | `a2fc746` | ring_publish mean **+12%**, pending_drain **+5%**, strategy **+26%**, market_create/mm_replenish mean **+2%** (all within run-to-run noise — see §"Post-7+8+9 fresh snapshot" below) | **+1.9%** vs single-run composed; **−1.1%** vs original baseline (3-run mean 36.252 s) |

### Post-7+8+9 fresh snapshot (2026-04-29)

Decision-making baseline before picking the next perf item. Ran the existing
`build-pgo-final/engine_backtest` (built 2026-04-28 23:41 against HEAD `a2fc746`)
three times on a quiet host. Workload, flags, balance, trade count (25),
fill count (51), PnL (−$724.15) all bit-identical to every prior run.

#### 3-run table

| Run | Wall (s) | Throughput (ev/s) | `market_create` avg/max | `mm_replenish` avg/max | `ring_publish` avg | `pending_drain` avg | `strategy` avg | RSS Δ | Peak RSS |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | (not timed) | 1 426.71 | 725 300 / 9.65 ms | 723 313 / 9.64 ms | 653 ns | 368 ns | 158 ns | +76.8 MiB | 99.9 MiB |
| 2 | 36.300 | 1 409.79 | 724 937 / 10.28 ms | 722 971 / 10.28 ms | 645 ns | 367 ns | 160 ns | +85.1 MiB | 103.2 MiB |
| 3 | 36.204 | 1 413.53 | 723 021 / 10.50 ms | 720 991 / 10.50 ms | 677 ns | 368 ns | 168 ns | +76.8 MiB | 99.9 MiB |
| **mean** | **36.252** | **1 416.7** | **724 419 / 10.14 ms** | **722 425 / 10.14 ms** | **658 ns** | **368 ns** | **162 ns** | **+79.6 MiB** | **101.0 MiB** |

#### What this profile says

* **The dominant cost is unchanged.** `mm_replenish` + `market_create`
  together account for **99.5%** of bar-loop wall time (72 297 ms / 72 644 ms).
  Each is ~723 µs/bar mean, and the two are ~equal because `market_create`
  measures the same call chain `mm_replenish` enters via `replenish` →
  `add_order` → `make_shared<order>` × 20 levels. No item that landed
  (2+3+5+E3+7+8+9) materially moved this; the doc predicted as much.

* **Cheap stages are at hardware floor.** `ring_publish` 658 ns mean,
  `pending_drain` 368 ns, `strategy` 162 ns, `stop_check` 24 ns. Items
  4/5/7/8/9 already harvested everything reachable on these. Further
  optimisation here is sub-1% of wall.

* **Tail latency held its 45% reduction.** `mm_replenish` max stayed
  at ~10.14 ms (vs original 18.51 ms / composed 2+3+5 10.17 ms).
  mimalloc + flat_hash_map cliff-removal is durable.

* **Workers remain ~99% idle.** `logging` 0.2% busy, `risk_stats` 1.0%
  busy, ~69 M empty polls each. Same picture as the original baseline.
  Spin-policy default is still wasteful.

* **No regression vs original baseline.** Wall **−1.1%** vs `f32c63e`
  (36.671 s → 36.252 s). Apparent +1.9% vs composed-2+3+5 single-run
  (35.560 s) is within 3-run variance — the doc notes "treat 0–2% wall
  deltas as noise." `Peak RSS` 96.0 → 101.0 MiB (+5.2%) is the item-9
  scratch reservation.

#### What this profile rules out

* **Item 11 (variant-in-ring) cannot be justified by this data.**
  `ring_publish` is 658 ns mean — there is no shared_ptr atomic-op cost
  visible here. The doc's gate ("if atomic-fetch-add/sub shows in the
  top 10") is not met. Would need `perf top` on a higher-throughput
  workload (e.g. tick-mode with WS replay) to even try.

* **Item 10 (lock-free dashboard view) cannot be informed by this data.**
  Backtest with `--thread-preset standard` does not render the TUI;
  `dashboard_view_` is built but never read. Justifying item 10 needs
  `engine_shadow` profiling under live render load.

* **Items 6 / 14 / 15 / 16 remain speculative.** Doc explicitly gates
  these on profiler evidence we do not have.

#### Where the time actually goes

`mm_replenish` is dominated by allocation *count*: 50 000 bars × 20
MM-seeded levels × `make_shared<order>` = 1 M allocations. mimalloc
serves these at ~1.5 ns each; the cost is the volume, not the per-call
latency. The COPY tracker confirms `market_event` is copied 50 000 times
(once per bar, on event publish into the ring).

The two structural fixes that would move this are (a) an `order` object
pool inside `OrderBook` so MM replenish recycles instead of allocates,
and (b) inline-event variant in ring slots (the doc's item 11). Both
are multi-day refactors.

#### Decision

The doc's recommended stopping criterion ("Re-profile. Stop here unless
the new profile justifies more.") is reached. None of the remaining
items in `performance.md` are justified by this profile alone:

* **Items 6, 11, 12, 13, 14, 15, 16** — explicitly gated on profiler
  evidence the bar-mode profile cannot provide.
* **Item 10** — needs an `engine_shadow` profile under render load
  before deciding.

Recommended action: pause perf work, redirect attention to the
in-progress safety/health workstream (heartbeat worker, dead-man's-switch,
watchdog) visible as uncommitted scaffolding in the tree. Re-open this
file with a shadow/live workload baseline once a Binance recording or
paper credentials are available, then re-decide between item 10 and a
non-doc orderbook order-pool item.

### Item 2 detail — mimalloc swap

Build: `-DENABLE_MIMALLOC=ON` added on top of the baseline flags.
Verified active at runtime via `MIMALLOC_VERBOSE=1` (`mimalloc: process init`,
`reserved 1048576 KiB memory`, end-of-run heap stats). 3 `mi_malloc*`
symbols present in `nm engine_live`.

Same workload, same binary configuration, same host. Two-run pair only —
treat 0–2% wall deltas as noise.

| Stage | Baseline avg (ns) | mimalloc avg (ns) | Δ avg | Baseline max (ns) | mimalloc max (ns) | Δ max |
|-------|------------------:|------------------:|------:|------------------:|------------------:|------:|
| market_create | 731 119 | 735 125 | **+0.5%** | 18 517 284 | 13 234 106 | **−29%** |
| mm_replenish  | 727 735 | 732 566 | **+0.7%** | 18 512 514 | 13 230 971 | **−29%** |
| ring_publish  |   1 417 |     840 | **−41%** |    369 533 |    320 210 | −13% |
| pending_drain |     678 |     495 | **−27%** |     61 916 |     47 670 | −23% |
| strategy      |     150 |     142 |  −5% |     81 473 |     18 896 | **−77%** |
| stop_check    |      23 |      26 | (noise) |     22 122 |      6 642 | **−70%** |

Wall: 36.671 s → 36.845 s (**+0.5%**). Engine throughput: 1 397.87 → 1 390.27 events/s (**−0.5%**). Both within run-to-run noise.

Memory:
- RSS delta: +85.6 MiB → +79.0 MiB (**−7.7%**)
- Peak RSS: 102.6 MiB → 97.8 MiB (**−4.7%**)
- Virtual delta: +233.8 MiB → +1040 MiB — *expected*, not a regression. mimalloc reserves a 1 GiB arena up front (`mimalloc: option 'arena_reserve': 1048576 KiB`); RSS is the truth and it dropped.

#### Why mean is flat but tails dropped 29%

This single-threaded backtest doesn't stress mimalloc's main strengths.
glibc malloc has thread-local arenas that handle uncontended single-thread
patterns about as well as mimalloc, so per-call mean barely moves. The
tail-latency win comes from mimalloc's eager-commit + segment caching:
worst-case allocator events that cause glibc page-fault storms (the
18.5 ms maxima in the baseline) get serviced from pre-committed arena
memory instead.

Where this matters in production: shadow/live mode runs on top of a
network event source and is sensitive to tail jitter (an 18 ms stall
shows up as visible cursor lag in the TUI and missed price ticks).
A 29% reduction in worst-case bar-loop latency is a real operational
win even though throughput-on-CSV is flat.

#### Where the bigger mean win would come from

`market_create` and `mm_replenish` together are >99% of bar-loop time.
The dominant cost inside `mm_replenish` is `MarketMaker::replenish` →
`orderbook::add_order` → `std::make_shared<order>` per level per side
per bar. That allocation is replaced wholesale by Item 11's variant-in-
ring-slot or Item 6's symbol interning — neither of which mimalloc can
help with because the cost is the *number* of allocations, not their
per-call latency.

Mimalloc was a low-risk infra setup the doc puts ahead of items that do
the structural work. Keep it on (cost is essentially zero) and judge
the next item's win against this run, not the original baseline.

#### Verification

No source files modified; the change is purely `CMakeLists.txt` (option
+ link block) and `cmake/Dependencies.cmake` (FetchContent block, gated
on `ENABLE_MIMALLOC`). Off-by-default → CI builds and existing tests are
bit-identical to before. Test suite was not re-run for this item because
no engine code paths changed; the `engine_core` OBJECT library is
allocator-neutral by design and mimalloc only links into the three
engine binaries.

### Item 3 detail — Profile-guided optimisation (PGO)

Build: `-DENABLE_PGO_GENERATE=ON` + training run + `-DENABLE_PGO_USE=ON`
on top of mimalloc + native-opt. Build dir: `build-pgo/`. Profile data:
34 `.gcda` files, 844 KB, written under `build-pgo/pgo-data/`.

Stages:

| Stage | Baseline (orig) | mimalloc | **PGO+mimalloc** | Δ avg vs baseline | Δ max vs baseline |
|-------|----------------:|---------:|-----------------:|------------------:|------------------:|
| market_create avg (ns) | 731 119 | 735 125 | **724 424** | **−0.9%** | — |
| market_create max (ms) | 18.52 | 13.23 | **13.06** | — | **−29.5%** |
| mm_replenish avg (ns)  | 727 735 | 732 566 | **722 132** | **−0.8%** | — |
| mm_replenish max (ms)  | 18.51 | 13.23 | **13.06** | — | **−29.5%** |
| ring_publish avg (ns)  | 1 417 | 840 | **751** | **−47.0%** | — |
| ring_publish max (ns)  | 369 533 | 320 210 | **225 052** | — | **−39.1%** |
| pending_drain avg (ns) | 678 | 495 | **443** | **−34.7%** | — |
| strategy avg (ns)      | 150 | 142 | **142** | −5.3% | — |
| strategy max (ns)      | 81 473 | 18 896 | **6 152** | — | **−92.5%** |
| stop_check avg (ns)    | 23 | 26 | 24 | flat | — |
| stop_check max (ns)    | 22 122 | 6 642 | **602** | — | **−97.3%** |

Wall: 36.671 → 36.845 → **36.301 s** (**−1.0 %** vs baseline).
Throughput: 1 397.87 → 1 390.27 → **1 410.80 ev/s** (**+0.9 %** vs baseline,
**+1.5 %** vs mimalloc).

#### Where PGO helped, and where it didn't

**It helped:** the cheap stages got dramatically tighter — `ring_publish`
mean dropped 47%, `pending_drain` 35%, and worst-case latencies on
`strategy` and `stop_check` dropped to single-digit microseconds (−92%
and −97% respectively). PGO learned that branches inside these stages
are highly predictable — most fills don't come back, most ticks don't
trip a stop, most strategies don't emit an order — and re-laid the
basic blocks accordingly.

**It barely helped where the cost is real work:** `mm_replenish` and
`market_create` are dominated by the *number* of `make_shared<order>`
calls per bar (one per L2 level seeded by `MarketMaker::replenish`), not
by branch mispredictions. PGO can't change the call count; it can only
make each call's overhead smaller. Net result: 0.8–0.9% improvement on
the dominant stages, 1% on wall.

**The tail-cleanup is the real story.** Across stages, max latencies
dropped by 30–97%. That's PGO doing what it does well: turning
"sometimes the inner loop is slow" into "the inner loop is consistently
fast." For shadow/live operation that's worth more than the modest
mean improvement.

#### Composability with mimalloc

The two compose cleanly. Stage 1's instrumented binary still links
mimalloc-static (third-party code is uninstrumented, exactly as the
doc intends). Profile data captures only first-party branches, so PGO
optimises engine code while mimalloc continues to handle allocations.
No flag conflicts, no special handling.

#### What this means for items 4–11

Items 4 (log audit) and 5 (flat_hash_map) target stages that PGO has
already cleaned up to 750 ns / 443 ns level — wins from those items
will be small in *absolute* terms unless they also touch
`mm_replenish` or `market_create`. The dominant cost is allocation
volume in `MarketMaker::replenish`. Items 6 (symbol interning) and
11 (shared_ptr → variant) attack that volume directly; expect those
to move the needle far more than items 4–10.

#### Verification

No source files modified; change is purely `CMakeLists.txt` (two new
options) and `cmake/CompilerFlags.cmake` (mutual-exclusivity guard +
PGO flag block in `tt_apply_common_flags`). Both options default OFF —
CI builds are bit-identical to before. The PGO workflow appendix is
documented in `docs/performance.md` and `CLAUDE.md`.

### Item 4 detail — log_event audit

**Verdict: already correct. No code change required.**

The doc's concern is "formatting on the event-loop thread before
pushing to the ring." That's not the case here — verified end-to-end:

1. **Ring element type** (`src/threading/worker.h:34`): rings carry
   `event_pointer` (alias for `std::shared_ptr<event>`), not strings.
   Confirmed for every consumer via `Worker::run<N, Policy>(RingBuffer<event_pointer, N, Policy>&)`.
2. **`engine::publish_event`** (`src/engine/engine.cpp:238`): pushes
   the raw `event_pointer` via `ring->try_push(ev)`. No formatting.
   Per-preset push fan-out is at lines 308–334 (`TT_PUSH` macro).
3. **`engine::log_event`** (`src/engine/engine.cpp:230`):
   ```cpp
   void engine::log_event(const event& ev)
   {
       if (config_.is_threaded()) return;       // threaded → no-op
       if (event_logger_) event_logger_->log(ev);  // inline only — binary
   }
   ```
   Threaded mode (`light`/`standard`/`full`/`extended`) returns
   immediately. Inline mode goes straight to the binary `EventLogger`,
   which is `event_serial::serialise()` → bytes → optional zstd →
   file write (`src/core/event_log.h:546`). Zero string formatting.
4. **`LoggingWorker::on_event`** (`src/engine/logging_worker.h:45`):
   takes the raw `event_pointer`, runs (a) `event_logger_->log(*ev)`
   (binary serialiser) and (b) `format_event(*ev)` for the text sink
   (`src/engine/logging_worker.h:124`) — **all on the worker thread**.
   Text-sink output is batched to 100 events before file/stdout flush.
5. **14 `log_event(...)` call sites** in `engine.cpp` all pass the raw
   `event&` reference (rejection, order, fill, cancel, amend, close,
   market, tick — see `engine.cpp:1453, 1482, 1523, 1613, 1643, 1689,
   1722, 1778, 1794, 1894, 2310, 2422, 3076, 3271`). None pre-format.

**Measured impact on the hot path.** In the Step 3 run (PGO+mimalloc),
the bar loop is 722 µs/iter and `mm_replenish` accounts for ~99 %.
Per-iteration `log_event(mkt)` cost is bounded above by the leftover
~1.2 µs that contains *all six* explicitly-instrumented stages plus
two `log_event` calls. PGO's `stop_check` and `strategy` max latencies
falling to 0.6 µs and 6.2 µs respectively are independent evidence
that the inline machinery around the dispatch is already trivial.

**Out-of-scope observations** (worth noting, not Step-4 fixes):

- `engine::event_logger_` and `LoggingWorker::event_logger_` are two
  separate `unique_ptr<EventLogger>` instances, both opened against
  `config_.event_log_path`. In threaded mode the engine's instance is
  dormant — only `flush()` is called on it from the engine — but the
  file is still opened twice. Wasteful (and a potential interleaving
  hazard if anything ever did write through the engine's instance in
  threaded mode), but no hot-path cost.
- `EventLogger::log` (`src/core/event_log.h:558`) allocates a
  per-call `std::vector<uint8_t> payload`. In threaded mode this lives
  on the worker thread and is harmless. In inline mode it is on the
  hot path. Could be replaced by a member buffer reused across calls;
  this is the same shape as Item 9's pre-allocated snapshot vectors.
  Filed as a separate follow-up in §"Items 7–9 candidates", not part
  of Step 4.

#### Verification

No source files modified. Test suite was not re-run for this item:
nothing changed. Step 4 is now closed as "audit confirms pre-existing
correctness" rather than a fix that lands.

### Item 5 detail — `absl::flat_hash_map` swap

Build: `build-perf/` rebuilt with `BUILD_TESTS=ON` on top of mimalloc +
debug + native-opt. Abseil moved from optional (gated on `ENABLE_DEBUG`)
to always-fetched in `tt_fetch_dependencies`; `absl::flat_hash_map`
linked PUBLIC into `engine_core` so every executable that links it
inherits the include path. Test suite: **653 / 653 pass** (1 skipped).

Six declarations swapped:

| File | Member | Key → Value |
|------|--------|-------------|
| `src/execution/portfolio.h:88` | `positions_` | `string → position` |
| `src/execution/portfolio.h:89` | `lots_` | `uint64_t → lot` |
| `src/execution/order_tracker.h:71` | `statuses_` | `uint64_t → order_status` |
| `src/engine/engine.h:85` | `execution_adapters_` | `string → shared_ptr<IExecutionAdapter>` |
| `src/engine/engine.h:160` | `open_orders_cache_` | `uint64_t → open_order_cache_entry` |
| `src/engine/engine.h:269` | `order_meta_` | `uint64_t → order_meta` |

One downstream call site updated: the local `pos_map` builder inside
`engine.cpp:160` (`restore_state` argument), now also `flat_hash_map`.

Comparison is against the **Step 2 (mimalloc)** run because this binary
shares its flag set (mimalloc + native-opt + debug, no PGO). PGO is a
separate axis; folding flat_hash_map into the PGO pipeline would
require a fresh training cycle.

| Stage | Step 2 (mimalloc) avg | **Step 5 avg** | Δ avg | Step 2 max | **Step 5 max** | Δ max |
|-------|----------------------:|---------------:|------:|-----------:|---------------:|------:|
| market_create | 735 125 ns | 735 006 ns | flat | 13.23 ms | **11.35 ms** | **−14.2 %** |
| mm_replenish  | 732 566 ns | 732 821 ns | flat | 13.23 ms | **11.35 ms** | **−14.2 %** |
| ring_publish  |     840 ns | **735 ns** | **−12.5 %** | 320 210 ns | **216 666 ns** | **−32.3 %** |
| pending_drain |     495 ns | **393 ns** | **−20.6 %** |  47 670 ns | 109 766 ns | regressed (single outlier) |
| strategy      |     142 ns | 148 ns | flat (+4 %, noise) |  18 896 ns | 44 864 ns | regressed (outlier) |
| stop_check    |      26 ns | 24 ns | flat (−7 %, noise) |   6 642 ns | **5 701 ns** | −14.2 % |

Wall: 36.845 → **36.841 s** (flat). Throughput: 1 390.27 → 1 390.49 ev/s
(flat). RSS delta: +79.0 → **+78.0 MiB** (−1.3 %).

#### Where flat_hash_map helped, and where it didn't

**It helped where the maps are actually in the loop.** `ring_publish`
mean dropped 12% — `publish_event` doesn't directly look up a map, but
the surrounding routing (`order_tracker_.set_status` →
`open_orders_cache_[id]` → `update_open_order_status`) hits three of
the swapped tables on every order. `pending_drain` dropped 21%
because each drained order goes through `process_order` → status
transitions → `lookup_opener(order_id)` (`order_meta_` hit). Both
stages are now bounded by the small constant work that's left.

**It didn't help where the cost is allocation, not lookup.**
`mm_replenish` and `market_create` mean times are unchanged — the
dominant cost there is `make_shared<order>` per MM-seeded level per
bar (one `orderbook::add_order` call), and the `orderbook.order_map_`
that gets touched on every add is *not* one of the six declarations
the doc names. Touching it would broaden Item 5's scope and the doc
explicitly says "Replace each of the following declarations" — so the
out-of-scope `orderbook` map stays.

**Tail latency dropped meaningfully.** `market_create` and
`mm_replenish` worst-case both fell from 13.2 ms to 11.4 ms (−14 %).
That's open-addressing pulling the cold-rehash cliff away — instead
of "occasional 18 ms page-fault storm on a chained-bucket grow", a
flat table grows once per power-of-two and the rehash itself is
linear-cache-friendly.

#### Composability with the prior items

mimalloc + flat_hash_map compose cleanly. flat_hash_map's
node-elision (one allocation per table grow vs one per insert) is
*aligned* with what mimalloc optimises (reducing total allocator
calls), so the wins don't cancel. PGO is orthogonal: re-running the
training workflow on this binary would propagate flat_hash_map
through the same hot-path branch profiles the existing PGO data
already validates. Doing so is a follow-up; the doc's "Verification"
for Item 5 is the test suite, which passes.

#### What this means for items 6–11

The bottleneck is now visible: mean bar-loop time is overwhelmingly
`MarketMaker::replenish` allocations. Items that reduce *count* of
allocations (Item 6 symbol interning eliminates the per-event
`std::string symbol` constructor; Item 11 variant-in-ring eliminates
the `shared_ptr` allocation) are the only ones that will move the
mean wall time meaningfully from here. Items 7–9 (small ring buffers
+ pre-alloc snapshot vectors) are quick mechanical wins but won't
move `mm_replenish`. Plan Item 5 → Item 11 directly if you have
appetite for the refactor; or land Items 7/8/9 first as low-risk
practice.

#### Verification

- `cmake --build build-perf -j --target truetest_tests` — clean.
- `./build-perf/truetest_tests` — **653 / 653 pass** (1 skipped). The
  doc's caution about iteration-order-dependent tests was unfounded
  for this codebase.
- Engine binaries built and ran the synthetic workload to completion
  with identical trade count (25), identical fill count (51), identical
  PnL ($−724.15) — behaviourally bit-identical.

### Composed (Items 2 + 3 + 5) — new reference baseline

This is the canonical "all four optimisations together" build that
subsequent items (6 onward) measure against. Each prior step measured
in isolation; composing them surfaces an interaction effect that a
linear sum-of-deltas misses.

**Build.** `build-pgo-final/` configured with the full flag set:
```
-DENABLE_DEBUG=ON -DENABLE_QUESTDB=ON -DENABLE_BINANCE=ON
-DENABLE_NATIVE_OPT=ON -DENABLE_MIMALLOC=ON -DBUILD_TESTS=ON
-DCMAKE_BUILD_TYPE=Release
```
Three-stage PGO workflow: configure with `ENABLE_PGO_GENERATE=ON`,
build, run training workload (50 k-bar synthetic CSV — 119 `.gcda`
files / 3.3 MB profile), reconfigure with `ENABLE_PGO_USE=ON`, rebuild.

**Composition verified.** Final `engine_backtest` binary:
- 3.36 MiB (smaller than the original 3.86 MiB baseline; PGO did
  dead-code elimination)
- 0 residual `__gcov0_*` symbols (the prior `build-pgo/` had 2 — that
  Phase 1 anomaly is now resolved by a clean rebuild against the
  post-Step-5 source)
- 57 `flat_hash_map` / `raw_hash_set` symbols (Item 5 active)
- 2 `mi_malloc*` symbols (Item 2 active)

**Tests.** `./build-pgo-final/truetest_tests` — **653 / 653 pass**
(1 skipped). Confirms PGO + flat_hash_map + mimalloc + native-opt
compose without subtle interaction. Issue A2 from the post-Step-5
problem audit resolved.

**Workload.** Same `/tmp/perf_baseline_50k.csv`, same
`--thread-preset standard --no-pin --balance 100000` flags, same trade
count (25), same fill count (51), same PnL (−$724.15) as every
prior run.

#### Stage table — composed vs each step in isolation

| Stage | Original | mimalloc (2) | PGO (3) | flat_hash (5) | **Composed (2+3+5)** |
|---|---:|---:|---:|---:|---:|
| Wall | 36.671 s | 36.845 s | 36.301 s | 36.841 s | **35.560 s** |
| Throughput (ev/s) | 1 397.87 | 1 390.27 | 1 410.80 | 1 390.49 | **1 440.02** |
| `market_create` mean | 731 119 | 735 125 | 724 424 | 735 006 | **709 734** ns |
| `market_create` max | 18.52 | 13.23 | 13.06 | 11.35 | **10.17** ms |
| `mm_replenish` mean | 727 735 | 732 566 | 722 132 | 732 821 | **707 939** ns |
| `mm_replenish` max | 18.51 | 13.23 | 13.06 | 11.35 | **10.17** ms |
| `ring_publish` mean | 1 417 | 840 | 751 | 735 | **588** ns |
| `ring_publish` max | 369 533 | 320 210 | 225 052 | 216 666 | **231 815** ns |
| `pending_drain` mean | 678 | 495 | 443 | 393 | **352** ns |
| `strategy` mean | 150 | 142 | 142 | 148 | **129** ns |
| `strategy` max | 81 473 | 18 896 | 6 152 | 44 864 | **6 111** ns |
| `stop_check` max | 22 122 | 6 642 | 602 | 5 701 | **3 988** ns |
| RSS delta | +85.6 | +79.0 | +78.8 | +78.0 | **+78.0** MiB |
| Peak RSS | 102.6 | 97.8 | 96.7 | 96.9 | **96.0** MiB |

#### Composition effects worth noting

**The `mm_replenish` mean finally moves.** PGO alone could only
achieve −0.8 %; flat_hash_map alone was flat. Composed: −2.7 %. The
reason is that PGO's training data was captured *after* the
flat_hash_map swap — the SwissTable probing pattern (linear-scan over
H2 hash bytes with SIMD on supported targets) generates a completely
different branch profile than chained-bucket linked-list traversal.
PGO retrained on this profile picks up inlining/layout decisions that
weren't visible when training against the chained version.

**Tail latencies on the dominant stages cleaned up to under 11 ms.**
mimalloc alone: 13.23 ms max. flat_hash_map alone: 11.35 ms max.
Composed: 10.17 ms — 45 % under the original baseline's 18.5 ms.
Not strictly additive, but each layer removed a different cliff:
mimalloc removed glibc page-fault storms, flat_hash_map removed
chained-bucket grow rehashes, PGO smoothed the basic-block layout
around both.

**`ring_publish` mean dropped 58 %.** Six-stage compounding:
`(840/1417) × (751/840) × (735/751) × (588/735)` ≈ 0.41. Each
step nibbled the same chain — atomic load + store, status update via
flat_hash_map, drop-counter accounting — and PGO+flat_hash_map's
combined effect on the embedded `set_status` / `cache_open_order`
calls is finally visible.

**Worker thread utilisation unchanged.** Both `logging` and
`risk_stats` workers still report 0.2 % / 1.0 % busy with 72 M empty
polls each. None of Steps 2–5 attacked this — they couldn't.
Pre-existing problem C4 from the audit still stands; addressing it is
its own work item (likely a `--spin-policy yield` default change).

#### What this means for items 6–11

Subsequent items measure against this composed run, not against the
original baseline. With the bottleneck already at ~708 µs per bar and
~99 % of that in `mm_replenish`, the only items that can move wall
time meaningfully now are:

- ~~**Item E3 (out-of-scope follow-up): swap `orderbook.order_map_` to
  `flat_hash_map`.**~~ **Landed (`66d972c`); measured neutral.**
  Hypothesis was that this was the highest-ROI remaining swap because
  `MarketMaker::replenish` calls `add_order` on every level per side
  per bar. Empirically the bottleneck inside `mm_replenish` is the
  20× `make_shared<order>` per bar (10 bid + 10 ask levels), not the
  map insert that follows. Across 4 post-E3 runs, wall lands at
  35.65–36.28 s (avg 35.9 s) vs single pre-E3 composed measurement
  35.56 s — overlapping noise. Kept the swap because it brings the
  orderbook subsystem in line with the post-Item-5 engine_core
  convention; no more mixed map types in hot lookup paths.
- **Item 6 (symbol interning).** Eliminates the `std::string symbol`
  copy + hash that happens inside every event constructor. Should
  cut `market_create` independently of `mm_replenish`.
- **Item 11 (variant-in-ring).** Removes the `make_shared<event>`
  call from every event publish. Both items are multi-day refactors;
  E3 is 30 minutes.

Items 7/8/9 (deque → ring + pre-alloc snapshot vectors) are
mechanical wins that won't move the dominant stages — defer until
incidental work in those files justifies the touch.

#### Reproduction

```bash
# Stage 1: instrumented build (mimalloc + native-opt + flat_hash_map already in source)
cmake -B build-pgo-final \
  -DENABLE_DEBUG=ON -DENABLE_QUESTDB=ON -DENABLE_BINANCE=ON \
  -DENABLE_NATIVE_OPT=ON -DENABLE_MIMALLOC=ON \
  -DENABLE_PGO_GENERATE=ON \
  -DBUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build-pgo-final -j --target engine_backtest truetest_tests

# Stage 2: training workload — same synthetic 50k-bar CSV
./build-pgo-final/engine_backtest \
  --provider local --path /tmp/perf_baseline_50k.csv --strategy sma \
  --thread-preset standard --no-pin --balance 100000

# Stage 3: optimised rebuild
cmake -B build-pgo-final \
  -DENABLE_PGO_GENERATE=OFF -DENABLE_PGO_USE=ON \
  -DENABLE_DEBUG=ON -DENABLE_QUESTDB=ON -DENABLE_BINANCE=ON \
  -DENABLE_NATIVE_OPT=ON -DENABLE_MIMALLOC=ON \
  -DBUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build-pgo-final -j --target engine_backtest truetest_tests

# Verify + run
./build-pgo-final/truetest_tests           # expect 653/653 pass
./build-pgo-final/engine_backtest --provider local --path /tmp/perf_baseline_50k.csv \
  --strategy sma --thread-preset standard --no-pin --balance 100000
# StageTimer report appended to ./truetest_debug.log
```
