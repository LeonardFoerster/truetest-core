# MODEL.md — Claude model selection for this repo

## Purpose

This document tells AI coding assistants (and humans driving them) which
Claude model to use for which kind of work in this codebase. It exists
because:

1. This repo contains a live-trading path. Some edits carry cross-file
   safety invariants that are easy to break by accident.
2. Different Claude models have measurably different behaviour on those
   invariants — not because one is "smarter," but because they have
   different defaults around defensive coding, retries, and fallback
   logic.
3. Anthropic charges meaningfully more per token for the larger model.
   Using it for every edit wastes budget; using only the smaller model
   risks shipping a subtle live-trading bug.

A short summary lives in `CLAUDE.md`. The full rationale, file list,
anti-patterns, and pre-merge checklist live here.

## TL;DR

- **Default model:** `claude-sonnet-4-6`
- **Before editing any safety-critical file:** `/model opus` to switch to
  `claude-opus-4-7`, then `/model sonnet` again afterwards.
- **When in doubt:** upgrade. The cost of one extra Opus turn is trivial
  compared to one live-trading regression.

## Model profiles in this repo

| Model | Explicit ID | Best for | Known weak spots here |
|---|---|---|---|
| Sonnet 4.6 | `claude-sonnet-4-6` | New strategies, indicators, tests, docs, single-file refactors, provider scaffolds following an existing pattern, CLI flags. | Tends to add "helpful" retry / fallback / soft-warn logic on safety paths. Less reliable at holding cross-file invariants in working memory during multi-file edits. |
| Opus 4.7 | `claude-opus-4-7` | Anything touching `engine.cpp`, kill-switch / dead-man's-switch / reconciler / watchdog interplay, the `TT_TARGET` gate, hot-path code, and threading primitives. Deep safety reviews across multiple files. | Higher per-token cost. Slower wall-clock latency per response. |

The split is roughly 80/20: most edits in this repo are routine and
Sonnet handles them cleanly. The remaining 20% are the load-bearing
safety paths where Opus's stronger invariant-tracking is worth paying
for.

## Sonnet-safe zone

Edit these freely with `claude-sonnet-4-6`:

- `src/strategy/**` — new strategies via `REGISTER_STRATEGY` factory
- `src/indicator/**` — sma, ema, rsi, bollinger, new indicators
- `tests/**` — including new GoogleTest files for safety code (testing
  is not the same as editing)
- `docs/**` — documentation
- `src/ui/panels/**` — TUI panel additions, status display tweaks
- `CMakeLists.txt` flag wiring for new optional backends, provided the
  pattern matches `tt_wire_optional_backends`
- `src/api/truetest_api.{h,cpp}` — C API surface additions (this path is
  isolated from the hot path and from live execution)
- New provider scaffolds under `src/providers/<venue>/` that follow the
  shape of `src/providers/local/` or the data-only parts of
  `src/providers/binance/` (transports, parsers, encoders)
- `src/analytics/**`, `src/market_maker/**`
- `src/bin/main.inc` CLI parsing additions

Real examples from the recent commit history that Sonnet would have
handled cleanly: adding a new indicator, registering a new strategy,
extending a TUI panel, adding a new test file, adding a CLI flag and
wiring it into `engine_config`.

## Opus-required zone

Switch to `claude-opus-4-7` before editing any of the following. Each
row names a concrete file or pattern, the invariant it protects, and
what breaks if that invariant is violated:

| Path | Invariant protected | What breaks if violated |
|---|---|---|
| `src/engine/engine.{h,cpp}` | Halt is terminal; no auto-resume; no `HAS_*` guards in core. | Engine continues after a safety abort; or core gains a hidden optional-dep requirement. |
| `src/engine/engine_config.h` | Same as engine.cpp; config struct is the contract for every binary. | Silent semantics drift between targets. |
| `src/core/tt_target.h` | `target_allows_live_orders()` is the **compile-time** gate. Only `engine_live` returns `true`. | A backtest or shadow binary becomes capable of placing real orders. |
| `src/providers/binance/binance_kill_switch.h` | On shutdown: cancel all, flatten free base, no retry loops, terminal failure escalates to operator. | Process exits while leaving open orders / positions on the venue. |
| `src/providers/binance/binance_futures_kill_switch.h` | Same as spot, plus: closes positions via reduceOnly MARKET, never sweeps balances (no "free base" on futures). | Futures position left open after shutdown, or stray balance sweep on a derivatives account. |
| `src/providers/binance/binance_futures_dead_mans_switch.h` | Server-side `countdownCancelAll` with fixed heartbeat cadence; watchdog catches stalled heartbeat thread; cancels orders only — does NOT flatten. | Catastrophic-shutdown protection silently disabled; or DMS starts flattening positions (overlaps with kill-switch and double-fires). |
| `src/providers/binance/binance_reconciler.h` | Startup drift refusal blocks the engine from starting with stale local state. Only documented soft-warn: spot testnet monthly reset. | Engine starts with positions that don't match the venue → first order builds on a false base. |
| `src/providers/binance/binance_futures_reconciler.h` | No testnet-reset soft-warn shortcut for futures (different reset cadence than spot). | Spot-style heuristic masks real drift on a real account. |
| `src/threading/worker_watchdog.h` | Watchdog timeout fires `halt_flag_`; fixed multiplier (`3 × heartbeat_ms`); no adaptive timing. | "Adaptive" watchdog can never fire under load → stalled heartbeat goes undetected → server-side countdown wipes orders mid-quote. |
| `src/threading/ring_buffer.h`, `worker.h`, `spin_policy.h` | SPSC discipline: exactly one producer, one consumer per ring. No locks. | Second producer "just for logging" → data race → heisenbug in production. |
| `src/risk/risk_manager.h`, `futures_risk_check.h` | Pre/post-fill checks halt on breach; refusal emits `rejection_event` and engine continues. Halt is terminal once set. | Soft-warn replacement → bad orders flow through. |
| Anywhere `halt_flag_` appears | Atomic boolean, write-once-true, never resettable from inside the engine. Only process restart clears it. | Auto-resume on `halt_flag_ = false` → all the rest of the safety net was for nothing. |
| Hot-path code | No `nlohmann::json` (CI-enforced by `scripts/check-hotpath-json.sh`); no heap allocations on event path; object pools only. | Latency regression; CI fails on the JSON check; or worse, silently breaks the hot path's allocation profile. |
| `src/providers/binance/binance_auth.h` and the REST signing path | HMAC-SHA256 with reusable `EVP_MAC_CTX`; nonce / timestamp / recvWindow discipline. | Signature failures, request replays, or worse: a leaked private key path. |
| `src/providers/binance/binance_oco_bracket_adapter.h`, `binance_futures_bracket_adapter.h` | OCO is two non-atomic POSTs on futures; cancel-other-on-fill is exchange-side via `closePosition=true`. Partial fractions are declined (`qty_fraction != 1.0`). | Engine starts trusting the venue to do partial-fraction OCO that the venue doesn't actually support. |

If you are editing a file not in this table but it lives under one of
these paths, treat it as Opus-required by default. Err on the side of
upgrading.

## The invariants, restated plainly

These are the rules the Opus-required zone exists to protect. Memorize
them; they apply across many files.

1. **The live-order gate is compile-time, not runtime.**
   `target_allows_live_orders()` is `constexpr`. Any runtime check is
   already a bug — it means the gate was lifted from compile-time to
   runtime, which is a regression.

2. **Halt is terminal.** `halt_flag_` goes from `false` to `true` once
   and never resets inside the running process. Manual operator
   recovery only — i.e. process restart. No `SIGUSR1` reset, no auto-clear
   after a cooldown, no "if safe again, resume."

3. **Kill-switch / dead-man's-switch failure is loud, not retried.**
   If the cancel-all-and-flatten path fails, the engine warns on stderr
   and the operator intervenes. No retry-with-backoff inside the safety
   path. The whole point of these mechanisms is to be deterministic; a
   retry loop introduces non-determinism at exactly the wrong moment.

4. **Hot path has no JSON and no allocations.** CI enforces the JSON
   half via `scripts/check-hotpath-json.sh`. The allocation half is
   enforced by code review and the object-pool pattern in `src/types/object_pool.h`.

5. **SPSC means one producer, one consumer.** If you find yourself
   wanting a second producer on a ring, you actually want a second
   ring. Adding a lock to a "lock-free" ring is a category error.

6. **The core compiles with zero external dependencies.** No `HAS_*`
   guards in `src/engine/engine.{h,cpp}` or `src/engine/engine_config.h`.
   Optional backends wire in via `tt_wire_optional_backends` in
   `cmake/Dependencies.cmake`.

7. **Reconciler refusal is the default; soft-warn is a documented
   exception.** Only the spot testnet monthly-reset case downgrades a
   drift to a warning. Adding new soft-warn cases requires explicit
   reasoning, not "this seemed inconvenient."

## Anti-patterns

Concrete diffs to **reject** in code review (or in your own output if
you catch yourself writing them). All of these are things `claude-sonnet-4-6`
has a tendency to produce; the explicit name here is to make the rule
operational, not to disparage the model.

### Anti-pattern 1: retry on the kill-switch path

```cpp
// WRONG — in binance_futures_kill_switch.h
for (int attempt = 0; attempt < 3; ++attempt) {
    if (cancel_all_and_flatten()) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(100 * (1 << attempt)));
}
```

The kill-switch runs **on shutdown**, often under time pressure or
network degradation. A 700ms retry budget burned inside the safety
path means 700ms less budget for the actual cancel/flatten. If the
first attempt fails, the venue is the source of truth — log loudly
and let the operator intervene.

### Anti-pattern 2: resettable halt flag

```cpp
// WRONG — anywhere
if (!halt_flag_.load() || config_.auto_resume_after_ms > 0) {
    // ...
}
// or
void clear_halt() { halt_flag_.store(false); }
```

`halt_flag_` is write-once-true by design. The "manual recovery only"
philosophy is the whole point: a safety system that auto-resumes is
not a safety system.

### Anti-pattern 3: hot-path JSON

```cpp
// WRONG — in binance_executor.h or any hot-path send_order
std::string body = nlohmann::json{
    {"symbol", symbol},
    {"side", side},
    {"type", type},
}.dump();
```

Caught by `scripts/check-hotpath-json.sh`. The hand-rolled
`BinanceOrderEncoder` exists precisely so this never happens.
`nlohmann::json` is permitted only in `src/bin/main.inc` (CLI config)
and `src/api/truetest_api.cpp` (C API config + result JSON).

### Anti-pattern 4: adaptive heartbeat / watchdog

```cpp
// WRONG — in binance_futures_dead_mans_switch.h or worker_watchdog.h
auto effective_heartbeat = std::max(
    heartbeat_interval_ms_,
    measured_recent_latency_ms_ * 2
);
```

The watchdog uses a fixed `3 × heartbeat_ms` multiplier on purpose: it
must fire under exactly the conditions where the system is degraded
and an adaptive timeout would mask the problem. "Make it more
forgiving" is the opposite of what a watchdog is for.

### Anti-pattern 5: runtime live-order check

```cpp
// WRONG — in any executor
if (config_.mode == engine_mode::live && config_.allow_live_orders) {
    submit_to_venue(order);
}
```

The gate is already enforced at compile time via `TT_TARGET` and
`target_allows_live_orders()`. Re-introducing a runtime check creates
the appearance of safety while regressing the actual guarantee, which
is that the `engine_backtest` and `engine_shadow` binaries are
**incapable** of placing real orders — not "configured not to."

### Anti-pattern 6: `HAS_*` guard inside core engine

```cpp
// WRONG — in src/engine/engine.cpp
#ifdef HAS_QUESTDB
    if (config_.questdb_persist) capture_event(ev);
#endif
```

The core engine must compile with zero external dependencies. Optional
backends wire in via `tt_wire_optional_backends` and are reached
through interfaces held by `engine_config`. The pattern to follow is
elsewhere in the same file: `if (config_.provider) ...`.

### Anti-pattern 7: second producer on an SPSC ring

```cpp
// WRONG — anywhere a worker ring is shared
// "Just for the debug logger, we'll also push from the stats thread."
ring_->push(debug_event);  // from a non-owner thread
```

`ring_buffer.h` is SPSC. Adding a second producer corrupts the
producer-side state and creates a data race that will show up as a
heisenbug in production. If two threads need to publish into the
worker, that's two rings.

### Anti-pattern 8: reconciler soft-warn for "convenience"

```cpp
// WRONG — in binance_futures_reconciler.h
if (drift_bps > tolerance && !is_first_run_) {
    log_warning("Drift detected, continuing anyway");
    return ReconcileResult::ok;
}
```

The reconciler **refuses startup** when local state disagrees with the
venue. The only documented soft-warn is the spot testnet monthly-reset
case, and it lives in `binance_reconciler.h` only — the futures
reconciler explicitly does not have it because futures testnet doesn't
reset on the same cadence.

## How to switch models in Claude Code

- Show current model: `/model`
- Switch to Opus: `/model opus` (resolves to `claude-opus-4-7`)
- Switch to Sonnet: `/model sonnet` (resolves to `claude-sonnet-4-6`)
- The choice is per-session; restart the CLI and you're back to your
  configured default.

Recommended operator habit:

1. Switch **before** the first `Read` of a safety file. Reading sets
   the model's context; you want Opus to be the one absorbing it.
2. Make the edit.
3. Switch back to Sonnet for follow-up routine work (tests, docs).
4. Note the switch in your PR description so reviewers know.

## Output length considerations

Anthropic's Pro and Max tiers have different per-response output caps.
For most work in this repo, that difference is invisible, because the
dominant workflow is:

```
Read → Edit → Edit → Edit
```

`Edit` sends only the diff, not the full file. You can refactor across
ten 500-line files in a single turn and never hit an output cap.

Where the cap **does** bite:

| Workflow | Output cap matters? | Mitigation if on Pro |
|---|---|---|
| New strategy / indicator / test (~200–400 LoC across 2 files) | No | — |
| Provider scaffold (~500–2000 LoC across many files, written from scratch) | Yes | Generate in chunks: header first, transport next, parser next. |
| Refactor across 5+ existing files (uses `Edit`) | No | — |
| Deep cross-file safety review with Opus (the "trace every path through kill-switch / DMS / watchdog / halt_flag") | Yes, this is the main case | Ask for structured output: "per path, one line description, one line failure mode, one line mitigation." Or split into two turns: inventory first, then per-path analysis. |
| Generating a new doc file in one shot (~500+ lines) | Sometimes | Generate by section. |

**Practical conclusion:** output length does not meaningfully change the
Pro-vs-Max calculus for this repo. The one case where Max would genuinely
help — long Opus safety reviews "stretching out" — can be emulated on Pro
with structured prompts and chunked turns. Plan accordingly; this is a
prompting discipline question, not a tier-blocking one.

## Pre-merge checklist for safety-touching edits

Copy this into the PR description when the diff touches any path in
the Opus-required zone:

```markdown
### Safety-edit checklist
- [ ] Diff produced with `claude-opus-4-7` (or fully line-by-line
      reviewed by a human)
- [ ] `target_allows_live_orders()` not bypassed; no new runtime
      live-order gate
- [ ] Halt path remains terminal (no resume, no retry, no clear)
- [ ] `scripts/check-hotpath-json.sh` passes
- [ ] `scripts/check-layer-deps.sh` passes
- [ ] Kill-switch failure path still escalates to operator (no silent
      retry loop)
- [ ] No new threads sharing data without SPSC or explicit sync
- [ ] Reconciler refusal preserved (no new soft-warn cases except the
      documented spot-testnet-reset one)
- [ ] No new `#ifdef HAS_*` guards inside `src/engine/engine.{h,cpp}`
      or `src/engine/engine_config.h`
- [ ] Model used: `claude-opus-4-7` / `claude-sonnet-4-6` / human-only
```

## When the Opus rule does not apply

To avoid the rule being treated as ceremonial and silently bypassed,
here are the explicit carve-outs. These edits inside the Opus-required
zone are safe with `claude-sonnet-4-6`:

- **Pure comment edits** — including doc-comment additions and typo
  fixes. Verify with `git diff -w` showing only comment lines.
- **`git mv` renames** with no logic change. Verify with
  `git diff -M --stat` showing only renames.
- **Whitespace and formatting**. Verify with `git diff -w` being empty.
- **Doc-string additions** that describe existing behaviour without
  asserting new semantics.
- **Test additions** under `tests/` that exercise an Opus-zone file but
  do not edit it.

If your edit fits one of these categories cleanly, document it in the
PR ("formatting only" / "rename only") and Sonnet is fine.

## Cost rationale

Why this split exists in concrete terms:

- `claude-opus-4-7` costs noticeably more per token than
  `claude-sonnet-4-6`.
- On a Pro subscription, swapping to Opus for ~20% of edits in this
  repo is well within usage limits and roughly emulates Max's
  always-Opus behaviour for the parts that actually matter.
- The 80% routine work on Sonnet is both cheaper and faster.
- The cost of one missed live-trading bug — orders left open, position
  not flattened, halt auto-resumed under bad conditions — dominates
  the entire annual subscription delta several times over. The split
  is therefore not "save money, accept more risk"; it's "spend the
  premium-model budget where it actually buys risk reduction."

## Open questions / future work

Not implemented yet; tracked here so they don't get lost:

- **Pre-commit hook that flags Opus-zone edits.** The hook cannot
  observe which model produced a diff, but it can match file paths
  against this document and warn the committer to confirm. Would live
  in `scripts/pre-commit` alongside the existing checks.
- **PR template field for "model used."** Forces the question to be
  answered in the PR rather than left implicit.
- **CI check that this file's path list stays in sync with the
  filesystem.** If `src/providers/binance/binance_futures_dead_mans_switch.h`
  gets renamed, this document should fail CI until updated.
- **Per-file `// AI-MODEL: opus` markers.** Lightweight in-source
  signal that survives file moves. Would let tools (and humans) flag
  the rule at the point of edit rather than only at PR time.
