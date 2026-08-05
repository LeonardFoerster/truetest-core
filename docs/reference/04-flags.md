# CLI Flags Reference

**All CLI flags live in one place:** `src/bin/main.inc` (function `register_cli_options`, using CLI11). The three binaries (`engine_backtest`, `engine_shadow`, `engine_live`) share the exact same registration via `#include "../main.inc"`. QuestDB and web flags are conditional on `HAS_QUESTDB` / `HAS_WEB` at build time.

**Precedence:** explicit CLI > `--config` JSON file > hard defaults.  
(There is **no** `TRUETEST_CONFIG` environment variable — only `--config <path>`.)  
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
| `--strategy <names>`  | Comma-separated list (e.g. `sma,mean-reversion,structure-continuation`). First is primary. Registered set: `mean-reversion`, `sma`, `ma-crossover`, `breakout`, `coiled-spring`, `adaptive-hybrid`, `structure-continuation`, `larry_connor`, `hedge-demo`. Default if empty: `mean-reversion`. |
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
| `--stream <type>`       | `trade`, `kline`, `kline_1m` / venue kline ids, etc. |
| `--depth-stream <spec>` | L2 depth on same WS (Binance e.g. `depth20@100ms`; Bitget e.g. `books5`). Enables queue/impact realism and real-book seeding. |
| `--live`                | Required safety flag for real-money orders (mainnet triggers math captcha; sandbox skips it). Only works on `engine_live` binary. |
| `--testnet`             | Sandbox routing: Binance → spot/futures testnet hosts; Bitget → demo/paptrading (same as `--demo`). |
| `--demo`                | Bitget demo/paptrading endpoints (also set by `--testnet` when provider is `bitget*`). |

### Futures risk / DMS (venue futures providers)
| Flag                              | What it does |
|-----------------------------------|--------------|
| `--margin-type <isolated\|crossed>` | Advisory margin-mode check at startup. |
| `--margin-type-strict`            | Turn margin-mode mismatch into a hard refusal. |
| `--liquidation-warn-pct <f>`      | Warn if any position is within this % of liquidation (default 5%). |
| `--max-notional / --max-leverage / --min-liq-distance-pct` | Pre-trade venue risk caps (`FuturesRiskCheck`). |
| `--dead-man-countdown-ms / --dead-man-heartbeat-ms` | DMS countdown settings (defaults ~30s / auto). |
| `--disarm-deadman`                | Explicitly do not arm DMS for this run. |
| `--dms-attempt-position-close`    | On persistent DMS heartbeat failure, also send a reduceOnly MARKET flatten (pairs with external watchdog). |

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
| `--realistic-fills`         | **Deprecated warn-noop** (passive-side fill pricing is always on). |
| `--bar-spread-bps`          | **Deprecated warn-noop** (calibrate `--mm-spread-pct` instead). |
| `--walked-book-impact`      | On L2 symbols, use actual VWAP of walked levels instead of mid + impact model. |
| `--fill-prob / --fill-fade / --fill-decay` | Probabilistic limit-fill model (default off). |
| `--mm-levels / --mm-base-depth / --mm-spread-pct / --mm-vol-mult / --mm-max-spread-pct` | Synthetic-book calibration. |
| `--queue-model <none\|l2-snapshot>` | Shadow queue-position model (requires `--depth-stream`). |
| `--maker-queue-model <none\|uniform\|front\|back>` | Maker queue model for paper limit orders (requires `--depth-stream`). |
| `--instrument <spec>`       | Per-symbol rules: `SYM:tick=...,lot=...,minq=...,minn=...,maker=...,taker=...` (repeatable). |

### Platform exits (all strategies)
| Flag | What it does |
|------|----------------|
| `--exit-policy <mode>` | **Platform** protective exits applied after each accepted strategy order. Modes: `floor` (default — ensure SL/TP when strategy omitted them), `strategy_only` (legacy research: only strategy `exit_intent`s), `engine_only` (ignore strategy intents), `union` (keep strategy intents; append SL if missing). Position-reducing signal closes do **not** get inverted short/long brackets. |
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

### ImGui strategy desk (only when built with `ENABLE_IMGUI=ON` / `HAS_IMGUI_DESK`)
| Flag | What it does |
|------|----------------|
| `--desk` | Open the personal ImGui desk (Monitor panels + operator pause/flatten/kill). Prefer over rich TUI when set. Batch runs keep the window open on the final snapshot until closed. See `docs/internal/imgui-desk-design.md`. |
| `--desk-demo-data` | Start the desk with deterministic DEMO DATA research panels already enabled (same as the menu toggle) - headless visual QA / manual smoke without a mouse click. |
| `--no-footprint` | Disable footprint public-trade collection, which otherwise auto-activates with `--desk`. Never affects trading behavior either way - purely observational (`footprint.md`). |
| `--footprint-tick-size` | Exact decimal tick-size override for the footprint panel (e.g. `0.01`), used only when official instrument metadata disagrees or is unavailable; conflicting values make the footprint unavailable rather than guessing. |

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
| `--persist-strict`          | Hard-fail on QuestDB problems + local ILP fallback. |

**Implicit (CLI11):** `-h/--help`, `--help-all`.

This surface is intentionally large because the engine is used for backtest, deterministic replay, shadow divergence measurement, Monte Carlo campaigns, and gated live execution from a single binary tree. Most realism/queue/impact flags are ignored (or hard-rejected) in `--mode live`.
