# CLI Flags Reference

**All CLI flags live in one place:** `src/bin/main.inc` (function `register_cli_options`, using CLI11). The three binaries (`engine_backtest`, `engine_shadow`, `engine_live`) share the exact same registration via `#include "../main.inc"`. QuestDB and web flags are conditional on `HAS_QUESTDB` / `HAS_WEB` at build time.

**Run profiles and precedence:** `--preset` selects a named option bundle. Profile-owned values are applied after JSON configuration; explicit CLI flags take priority.
(There is **no** `TRUETEST_CONFIG` environment variable — only `--config <path>`.)  
**Introspection:** `--dry-run` (validate + print summary + exit), `--dump-config` (emit resolved snake_case JSON).

### Core / Replay / Logging
| Flag                          | What it does |
|-------------------------------|--------------|
| `--replay <path>`             | Authoritative ledger replay from a sealed, current-v3, non-rotated binary event log; recorded orders, fills and funding are applied once and strategies are not rerun. Use the same initial balance as the recorded run. |
| `--replay-from / --replay-to` | Both bounds are currently refused for ledger replay: prefix state is unavailable and append order need not be monotonic in exchange-event time. |
| `--log-events <path>`         | Write binary event log (market + order + fill events) for this run. |
| `--log-file <path>`           | Write operational text log (L1) instead of stderr. |
| `--log-max-size <MB>`         | Rotate logs after this size (L3). 0 = no rotation. |
| `--log-keep <N>`              | How many rotated log files to keep. |
| `--compress-log / --no-compress-log` | Toggle zstd compression of binary event logs (default on). |
| `--seed <uint64>`             | Master RNG seed (0 = non-deterministic). |

`--log-events` is intentionally refused together with `--replay`; replay reads
an authoritative ledger and does not generate a second event log.
Logs created with `--log-max-size > 0` are marked as independent rotation
segments and remain inspection-only until a manifest-based stitching format is
implemented. Use the default `--log-max-size 0` when authoritative replay is
required.

When `--log-events` is set, a dropped or failed logging-worker write is
**terminal** (triggers halt) regardless of `--drop-policy`/mode — a silently
incomplete durable ledger is never treated as authoritative. The logging
worker's error budget is also tightened to 1 consecutive error instead of the
usual default in this mode.

Only a cleanly sealed, current-v3, non-segmented event log is eligible for
authoritative ledger replay. Mainnet live atomically reserves a unique new
regular file beneath existing symlink-free trusted directory ancestry, verifies
and pins its identity, synchronizes its preamble before provider startup,
refuses rotation and non-dedicated logging presets, and claims the reservation
once before any logger truncation. Normal and generic `risk_unwind` order intents
wait up to two seconds for their exact post-flush/fsync ACK before adapter
submission; both ACK production and engine consumption must beat the deadline.
A lock-free terminal admission CAS then spans setup and the adapter call: a
command that wins before concurrent close is in flight, and none can enter after
close. The deadline does not bound an earlier blocking
ring publication, a blocked `fsync`, provider shutdown, or worker join. Other
economic records checkpoint when the logger consumes them; observational
records checkpoint after 256 consumed records or when a later record observes
at least 100 ms since completion of the prior sync. Compromise is sticky; the
engine attempts the two-phase index/trailer-prefix plus append-only v3 integrity
seal only after a quiesced/drained/healthy shutdown gate, and both phases must
return successfully for operational evidence.
Reserved logger destruction never seals implicitly. Worker backlog, an idle
final observational record, or abrupt termination can leave an unsealed
diagnostic prefix. Per-record and covered-prefix CRC32C detect accidental
corruption; they are not authentication. This is not a full command WAL,
exactly-once execution, or complete crash recovery. Backtest, shadow, and
sandbox logs do not use the mainnet reservation policy. See
`docs/todos/08-H-persistence-observability.md` H-03 for remaining
cancel/amend/native-bracket and kill-switch/DMS/native safety command
intent/outcome and reconstruction work.

### Threading / CPU
| Flag                    | What it does |
|-------------------------|--------------|
| `--thread-preset <name>` | One of: `inline`, `light`, `standard`, `full`, `extended` (auto-selected from core count if omitted). |
| `--no-pin`              | Disable CPU affinity pinning. |
| `--spin-policy <name>`  | Worker spin policy: `spin`, `yield`, or `adaptive` (default). |

### Provider / Data / Monte Carlo
| Flag                    | What it does |
|-------------------------|--------------|
| `--provider <name>`     | Provider: `local`, `binance`, `binance-futures`, `bitget`, `bitget-futures`, `bitunix`, `bitunix-futures`, `synthetic` (registry also accepts `montecarlo` as alias of synthetic). Venue providers only resolve when built with the matching `ENABLE_*` / `HAS_*`. |
| `--path <path>`         | Data file path (for `local` provider; supports comma-separated for multi-path). |
| `--mc-params "..."`     | Synthetic/Monte-Carlo generator params (`mu=...,sigma=...,n_steps=...,initial_price=...`). |
| `--monte-carlo`         | Run a Monte Carlo campaign (requires `--mc-trials`). |
| `--mc-trials <N>`       | Number of independent trials. |
| `--mc-model <name>`     | Generator: `gbm` (only gbm implemented today). |
| `--mc-parallel`         | **Experimental**: run trials concurrently (use only with `--thread-preset inline`). |
| `--mc-reuse-objects`    | Reuse `data_handler`/strategy/etc. between MC trials for speed (results not guaranteed bit-identical on caches). |

### Strategy
| Flag                  | What it does |
|-----------------------|--------------|
| `--strategy <names>`  | Comma-separated list (e.g. `sma,mean-reversion,structure-continuation`). First is primary. Registered set: `mean-reversion`, `sma`, `ma-crossover`, `breakout`, `coiled-spring`, `structure-continuation`, `larry_connor`, `hedge-demo`. Default if empty: `mean-reversion`. `adaptive-hybrid` is retired and explicitly refused. |
| `--format <tick\|bar>` | Input data format for local provider. |
| `--sma-period <N>`    | SMA period (default 20). |
| `--param key=val`     | Strategy parameter (repeatable). |

### Mode / Execution / Fees
| Flag              | What it does |
|-------------------|--------------|
| `--mode <name>`   | `backtest`, `shadow`, or `live` (default from `TT_TARGET`). |
| `--fee <model>`   | `fixed` or `tiered`. |
| `--fee-value`     | Fixed fee amount. |
| `--maker-rate / --taker-rate` | Tiered maker/taker rates. |

### Symbol / Streaming / Live Safety
| Flag                    | What it does |
|-------------------------|--------------|
| `--symbol <SYM>`        | Trading symbol (e.g. `BTCUSDT`). |
| `--stream <type>`       | `trade`, `kline`, `kline_1m` / venue kline ids, etc. On Binance, kline streams only emit closed candles (`"x": true`); forming/in-progress candles are discarded, not forwarded early. |
| `--depth-stream <spec>` | L2 depth on same WS (Binance e.g. `depth20@100ms`; Bitget e.g. `books5`). Enables queue/impact realism and real-book seeding. |
| `--live`                | Required safety flag for real-money orders (mainnet triggers math captcha; sandbox skips it). Only works on `engine_live` binary. |
| `--testnet`             | Sandbox routing: Binance → spot/futures testnet hosts; Bitget → demo/paptrading (same as `--demo`). |
| `--demo`                | Bitget demo/paptrading endpoints (also set by `--testnet` when provider is `bitget*`). |

For mainnet live, spot and futures both require a positive daily-loss limit, a
unique durable event-ledger path, rotation disabled, and a dedicated logging
worker (`standard`, `full`, or `extended`). Futures additionally require all
three positive venue caps, an armed DMS, `--risk-unwind`, and
`--reconcile-tolerance-bps` in `(0,3]`. Sandbox recognition is provider-aware:
Binance uses `--testnet`, Bitget uses `--demo` or `--testnet`, and Bitunix
refuses both in every engine mode because it has no sandbox endpoint.

### Futures risk / DMS (venue futures providers)
| Flag                              | What it does |
|-----------------------------------|--------------|
| `--margin-type <isolated\|crossed>` | Advisory margin-mode check at startup. |
| `--margin-type-strict`            | Turn margin-mode mismatch into a hard refusal. |
| `--liquidation-warn-pct <f>`      | Warn if any position is within this % of liquidation (default 5%). |
| `--max-notional / --max-leverage / --min-liq-distance-pct` | Pre-trade venue risk caps (`FuturesRiskCheck`). |
| `--dead-man-countdown-ms / --dead-man-heartbeat-ms` | DMS countdown settings (defaults ~30s / auto). |
| `--disarm-deadman`                | Explicitly do not arm DMS for this run. |

### Credentials / Network / Recording
| Flag                    | What it does |
|-------------------------|--------------|
| `--api-key / --api-secret` | Exchange credentials. Prefer env (env wins over CLI): `TRUETEST_BINANCE_*` (default), `TRUETEST_BITGET_*` (+ passphrase), `TRUETEST_BITUNIX_*`. |
| `--api-passphrase`      | API passphrase (Bitget and similar venues). Env: `TRUETEST_BITGET_API_PASSPHRASE`. |
| `--host / --port`       | Override WS host/port. |
| `--record <path>`       | Record live WS feed to file. |
| `--replay-data <path>`  | Replay a previously recorded WS file. |

### Portfolio / Risk Basics / Checkpoints
| Flag                        | What it does |
|-----------------------------|--------------|
| `--balance <float>`         | Initial cash (default 10000). |
| `--risk-fraction <float>`   | Fraction of equity per trade (default 0.02). |
| `--sl / --tp <float>`       | Platform stop-loss / take-profit as fraction of entry (also used by DefaultExitPolicy; see Platform exits). |
| `--checkpoint <path>`       | Write periodic binary portfolio snapshots. |
| `--checkpoint-interval <N>` | Events between checkpoints (default 10000). |
| `--resume <path>`           | **Unavailable.** Checkpoint v1 lacks enough state for a safe restore; CLI and direct engine configuration refuse before mutation. |

### Backfill / Order Realism / Debug
| Flag                        | What it does |
|-----------------------------|--------------|
| `--backfill <N>`            | Historical bars to fetch before streaming (default 500). |
| `--backfill-interval`       | Kline interval for backfill. |
| `--aggression / --qty-scale / --fill-rng-seed / --spread-step` | LocalBookAdapter / fill-model tuning knobs. |
| `--debug-fills / --debug-fills-budget` | Log first N fills with book state (intended vs. fill). |

### Config / Dry-Run / Analytics
| Flag                    | What it does |
|-------------------------|--------------|
| `--config <path>`       | Load JSON config file. |
| `--preset <name>`       | Apply a named run profile: `futures-phase0`, `mc-robustness`, `backtest-local-l2`, or `shadow-tape` (aliases accepted). Use `--dump-config` to inspect resolved values. |
| `--dump-config`         | Print resolved config as JSON and exit. |
| `--dry-run`             | Validate everything, print summary, and exit (no engine run). |
| `--rolling-window / --risk-free-rate / --periods-per-year / --max-equity-points` | Analytics (Sharpe/Sortino, equity curve decimation, etc.). |

### Realism Models (backtest/shadow only)
| Flag                        | What it does |
|-----------------------------|--------------|
| `--exec-bar-delay <N>`      | Future same-symbol price events before submission (1 = next observation; prevents same-bar execution; no EOS force-fill). |
| `--wire-latency-us`         | Extra wire + ingest latency on top of any engine latency model. |
| `--order-latency-us / --order-latency-stddev-us` | Strategy→eligible delay (fixed or stochastic). |
| `--impact-k-bps / --impact-adv` | Square-root market impact model. |
| `--realistic-fills`         | **Deprecated warn-noop** (passive-side fill pricing is always on). |
| `--bar-spread-bps`          | **Deprecated warn-noop** (calibrate `--mm-spread-pct` instead). |
| `--walked-book-impact`      | On L2 symbols, use actual VWAP of walked levels instead of mid + impact model. |
| `--fill-prob / --fill-fade / --fill-decay` | Probabilistic limit-fill model (default off). |
| `--mm-levels / --mm-base-depth / --mm-spread-pct / --mm-vol-mult / --mm-max-spread-pct` | Synthetic-book calibration. |
| `--queue-model <none\|l2-snapshot>` | Shadow queue-position model (requires `--depth-stream`). |
| `--maker-queue-model <none\|uniform\|front\|back>` | Maker queue model for paper limit orders. `--depth-stream` optional, but without it QueueAware joins **conservatively** (`size_ahead=+inf`, no `on_trade` fills — no front-of-queue assumption without real L2); bar/tick range sweep still fills passive limits on `[low,high]`. |
| `--instrument <spec>`       | Per-symbol rules: `SYM:tick=...,lot=...,minq=...,minn=...,maker=...,taker=...` (repeatable). |

### Platform exits (all strategies)
| Flag | What it does |
|------|----------------|
| `--exit-policy <mode>` | **Platform** protective exits applied after each accepted strategy order. Modes: `floor` (default — ensure SL/TP when strategy omitted them), `strategy_only` (legacy research: only strategy `exit_intent`s), `engine_only` (ignore strategy intents), `union` (keep strategy intents; append at most one Platform intent containing only globally missing configured SL/TP/trailing legs; an empty Strategy plan receives the full Platform intent). Position-reducing signal closes do **not** get inverted short/long brackets. |
| `--sl <frac>` | Stop-loss fraction of entry for platform defaults (`0` = off). Default `0.003` (0.3%). |
| `--tp <frac>` | Take-profit fraction of entry for platform defaults (`0` = off). Default `0.01`. |

Strategies do **not** need to implement SL/TP. Rich strategy intents (ATR/fib/scale-out) still win under `floor` when they already set `stop_loss`.

### Risk Limits
| Flag                              | What it does |
|-----------------------------------|--------------|
| `--max-daily-loss / --daily-reset-hour` | Daily loss halt + reset hour (UTC). |
| `--max-trades-per-hour / --max-orders-per-minute` | Throughput limits. |
| `--risk-unwind`                   | **Flag** (no value): on risk halt, flatten all positions before stopping. |

### Live Safety / Reconciliation
| Flag                          | What it does |
|-------------------------------|--------------|
| `--reconcile-tolerance-bps <f>` | Max allowed local vs. exchange drift at startup (live only; default 10 bps). Refusal above this. |
| `--kill-switch-deadline-ms <ms>` | Hard deadline for cancel-all + flatten on shutdown (default 5s). Warning on miss. |

### Output / Status
| Flag                    | What it does |
|-------------------------|--------------|
| `--output <path>`       | Write results (JSON or CSV). |
| `--output-format <json\|csv>` | Results format. |
| `--status-format <auto\|tui\|plain\|ndjson\|off>` | Live dashboard mode (default `auto` → rich TUI on tty for shadow/live). |
| `--no-tui`              | Shortcut for `--status-format=plain`. |
| `--simple-tui`          | On shadow/live, use the ANSI-box console dashboard instead of the rich ncurses TUI; has no effect on backtest or non-TUI status formats. |

### ImGui trading command center (only when built with `ENABLE_IMGUI=ON` / `HAS_IMGUI_DESK`)
| Flag | What it does |
|------|----------------|
| `--desk` | Open the fixed ImGui trading command center with attended pause/flatten/kill controls. GLFW/ImGui/OpenGL startup and rendering stay on the application-main thread while engine work runs in a `std::jthread`. Batch runs keep the final snapshot visible until the operator closes the window. See `docs/internal/imgui-desk-design.md`. |

### Web UI (only when built with `ENABLE_WEB=ON` / `HAS_WEB`)
| Flag | What it does |
|------|----------------|
| `--web` | Serve the read-only web UI for this session. |
| `--web-port` | Port (default 8080). |
| `--web-bind` | Bind address (default `127.0.0.1`). |
| `--web-token` | Bearer token. **Required** for shadow/live when `--web` is set; optional for backtest. |
| `--web-assets` | Directory of built SPA assets (default: API/WS only). |

### QuestDB Persistence (only when built with `ENABLE_QUESTDB=ON` / `HAS_QUESTDB`)
| Flag                        | What it does |
|-----------------------------|--------------|
| `--persist`                 | Capture order lifecycle / run data to QuestDB. |
| `--run-tag <string>`        | Table prefix (auto-generated if omitted). |
| `--run-notes <string>`      | Free-form note stored in `runs_meta`. |
| `--questdb-host / --questdb-ilp-port / --questdb-http-port` | Connection details (defaults: 127.0.0.1, 9009, 9000). |
| `--questdb-flush-ms`        | Time-based ILP flush interval (default 150). |
| `--persist-strict`          | Hard-fail on QuestDB startup/runtime problems. Backtest returns non-zero; shadow/live also halt. Rejected with Monte Carlo. A local ILP fallback remains diagnostic, not success. |

**Implicit (CLI11):** `-h/--help`, `--help-all`.

This surface is intentionally large because the engine is used for backtest, deterministic replay, shadow divergence measurement, Monte Carlo campaigns, and gated live execution from a single binary tree. Most realism/queue/impact flags are ignored (or hard-rejected) in `--mode live`.
