# CLI Flags Reference

**All CLI flags live in one place:** `src/bin/main.inc:654` (function `register_cli_options`, using CLI11). The three binaries (`engine_backtest`, `engine_shadow`, `engine_live`) share the exact same registration via `#include "../main.inc"`. QuestDB flags are conditional on `HAS_QUESTDB` at build time.

**Precedence:** CLI > `--config` JSON (or `TRUETEST_CONFIG`) > defaults.  
**Introspection:** `--dry-run` (validate + print summary + exit), `--dump-config` (emit resolved snake_case JSON).

### Core / Replay / Logging
| Flag                          | What it does |
|-------------------------------|--------------|
| `--replay <path>`             | Replay from a binary event log (zstd-compressed). |
| `--replay-from / --replay-to` | Time window (µs since epoch) inside the replay file. |
| `--log-events <path>`         | Write binary event log (market + order + fill events) for this run. |
| `--log-file <path>`           | Write operational text log (L1) instead of stderr. |
| `--log-max-size <MB>`         | Rotate logs after this size (L3). 0 = no rotation. |
| `--log-keep <N>`              | How many rotated log files to keep. |
| `--compress-log / --no-compress-log` | Toggle zstd compression of binary event logs (default on). |
| `--seed <uint64>`             | Master RNG seed (0 = non-deterministic). |

### Threading / CPU
| Flag                    | What it does |
|-------------------------|--------------|
| `--thread-preset <name>` | One of: `inline`, `light`, `standard`, `full`, `extended` (auto-selected from core count if omitted). |
| `--no-pin`              | Disable CPU affinity pinning. |
| `--spin-policy <name>`  | Worker spin policy: `spin`, `yield`, or `adaptive` (default). |

### Provider / Data / Monte Carlo (current branch)
| Flag                    | What it does |
|-------------------------|--------------|
| `--provider <name>`     | Provider: `local`, `binance`, `binance-futures`, `synthetic` (or `montecarlo`). |
| `--path <path>`         | Data file path (for `local` provider; supports comma-separated for multi-path). |
| `--mc-params "..."`     | Synthetic/Monte-Carlo generator params (`mu=...,sigma=...,n_steps=...,initial_price=...`). |
| `--monte-carlo`         | Run a Monte Carlo campaign (requires `--mc-trials`). |
| `--mc-trials <N>`       | Number of independent trials. |
| `--mc-model <name>`     | Generator: `gbm` (default) or `ou`. |
| `--mc-parallel`         | **Experimental** (Phase 5): run trials concurrently (strong caveats — use only with `--thread-preset inline`). |
| `--mc-reuse-objects`    | Phase A: reuse `data_handler`/strategy/etc. between MC trials for speed (results not guaranteed bit-identical on caches). |

### Strategy
| Flag                  | What it does |
|-----------------------|--------------|
| `--strategy <names>`  | Comma-separated list (e.g. `sma,mean-reversion`). First is primary. |
| `--format <tick|bar>` | Input data format for local provider. |
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
| `--stream <type>`       | `trade`, `kline`, `kline_1m`, etc. |
| `--depth-stream <spec>` | L2 depth stream (e.g. `depth20@100ms`) on same WS connection. Enables queue/impact realism and real-book seeding. |
| `--live`                | Required safety flag for real-money orders (mainnet triggers math captcha; testnet skips it). Only works on `engine_live` binary. |
| `--testnet`             | Route Binance provider to testnet endpoints. |

### Futures-only (binance-futures)
| Flag                              | What it does |
|-----------------------------------|--------------|
| `--margin-type <isolated|crossed>` | Advisory margin-mode check at startup. |
| `--margin-type-strict`            | Turn margin-mode mismatch into a hard refusal. |
| `--liquidation-warn-pct <f>`      | Warn if any position is within this % of liquidation (default 5%). |
| `--max-notional / --max-leverage / --min-liq-distance-pct` | Pre-trade venue risk caps (FuturesRiskCheck). |
| `--dead-man-countdown-ms / --dead-man-heartbeat-ms` | DMS `/fapi/v1/countdownCancelAll` settings (default 30s / auto). |
| `--disarm-deadman`                | Explicitly do not arm DMS for this run. |
| `--dms-attempt-position-close`    | Phase 3: on persistent DMS heartbeat failure, also send a reduceOnly MARKET flatten (pairs with external watchdog). |

### Credentials / Network / Recording
| Flag                    | What it does |
|-------------------------|--------------|
| `--api-key / --api-secret` | Exchange credentials (env vars `TRUETEST_BINANCE_*` preferred). |
| `--host / --port`       | Override WS host/port. |
| `--record <path>`       | Record live WS feed to file (via `BinanceRecorder`). |
| `--replay-data <path>`  | Replay a previously recorded WS file. |

### Portfolio / Risk Basics / Checkpoints
| Flag                        | What it does |
|-----------------------------|--------------|
| `--balance <float>`         | Initial cash (default 10000). |
| `--risk-fraction <float>`   | Fraction of equity per trade (default 0.02). |
| `--sl / --tp <float>`       | Stop-loss / take-profit as fraction of entry price. |
| `--checkpoint <path>`       | Write periodic binary portfolio snapshots. |
| `--checkpoint-interval <N>` | Events between checkpoints (default 10000). |
| `--resume <path>`           | Load portfolio state from checkpoint before start. |

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
| `--dump-config`         | Print resolved config as JSON and exit. |
| `--dry-run`             | Validate everything, print summary, and exit (no engine run). |
| `--rolling-window / --risk-free-rate / --periods-per-year / --max-equity-points` | Analytics (Sharpe/Sortino, equity curve decimation, etc.). |

### Realism Models (backtest/shadow only)
| Flag                        | What it does |
|-----------------------------|--------------|
| `--exec-bar-delay <N>`      | Bars of execution delay (1 = parks until next bar open; kills same-bar lookahead). |
| `--wire-latency-us`         | Extra wire + ingest latency on top of any engine latency model. |
| `--order-latency-us / --order-latency-stddev-us` | Strategy→eligible delay (fixed or stochastic). |
| `--impact-k-bps / --impact-adv` | Square-root market impact model. |
| `--realistic-fills`         | Fill at resting counterparty price (one event per walked level); suppresses `--bar-spread-bps`. |
| `--bar-spread-bps`          | Full bid-ask spread charged to bar-mode market orders. |
| `--walked-book-impact`      | On L2 symbols, use actual VWAP of walked levels instead of mid + impact model. |
| `--queue-model <none|l2-snapshot>` | Shadow queue-position model (requires `--depth-stream`). |
| `--maker-queue-model <none|uniform|front|back>` | Maker queue model for paper limit orders (requires `--depth-stream`). |
| `--instrument <spec>`       | Per-symbol rules: `SYM:tick=...,lot=...,minq=...,minn=...,maker=...,taker=...` (repeatable). |

### Risk Limits
| Flag                              | What it does |
|-----------------------------------|--------------|
| `--max-daily-loss / --daily-reset-hour` | Daily loss halt + reset hour (UTC). |
| `--max-trades-per-hour / --max-orders-per-minute` | Throughput limits. |
| `--risk-unwind`                   | On risk halt, flatten all positions before stopping. |
| (plus the engine_config risk fields: max drawdown, position value, loss/trade, open orders, exposure — settable via JSON or future flags) | |

### Live Safety / Reconciliation
| Flag                          | What it does |
|-------------------------------|--------------|
| `--reconcile-tolerance-bps <f>` | Max allowed local vs. exchange drift at startup (live only; default 10 bps). Refusal above this. |
| `--kill-switch-deadline-ms <ms>` | Hard deadline for cancel-all + flatten on shutdown (default 5s). Warning on miss. |

### Output / Status
| Flag                    | What it does |
|-------------------------|--------------|
| `--output <path>`       | Write results (JSON or CSV). |
| `--output-format <json|csv>` | Results format. |
| `--status-format <auto|tui|plain|ndjson|off>` | Live dashboard mode (default `auto` → rich TUI on tty for shadow/live). |
| `--no-tui`              | Shortcut for `--status-format=plain`. |

### QuestDB Persistence (only when built with `ENABLE_QUESTDB=ON`)
| Flag                        | What it does |
|-----------------------------|--------------|
| `--persist`                 | Capture every order lifecycle event to QuestDB. |
| `--run-tag <string>`        | Table prefix (auto-generated if omitted). |
| `--run-notes <string>`      | Free-form note stored in `runs_meta`. |
| `--questdb-host / --questdb-ilp-port / --questdb-http-port` | Connection details (defaults: 127.0.0.1, 9009, 9000). |

**Implicit (CLI11):** `-h/--help`, `--help-all`.

This surface is intentionally large because the engine is used for backtest, deterministic replay, shadow divergence measurement, Monte Carlo campaigns, and gated live execution from a single binary tree. Most realism/queue/impact flags are ignored (or hard-rejected) in `--mode live`.
