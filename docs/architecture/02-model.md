# Model Selection + Anti-Patterns

**Status**: Thin extraction (Doc Phase). 

**Planned / extracted; see reference/ for current** authoritative details. Full rationale lives in `AGENTS.md` + `docs/reference/01-instructions.md` (§2) + `docs/governance/01-prod.md`.

**Last updated**: 2026-07 (new content impl — synthesized from AGENTS.md + instructions.md anti-patterns list).

---

## AI Coding Assistant Rules (from AGENTS.md)

**Default — Sonnet 4.6** sufficient for:
- New strategies, indicators, tests, CLI flags, docs
- Single-file refactors
- Provider-stack additions following existing patterns
- Work in `src/simulation/` + synthetic provider (MC path gen, controller)

**Must switch to Opus 4.7 (`/model opus`) before editing any of**:
- `src/engine/engine.{h,cpp}` + `engine_config.h`
- `*kill_switch*`, `*dead_mans_switch*`, `*reconciler*`, `*watchdog*`
- `src/core/tt_target.h` + any `TT_TARGET` / `target_allows_live_orders` callsites
- `src/threading/` (SPSC, spin, affinity)
- `src/risk/` + any `halt_flag_` code
- Hot-path code (no `nlohmann/json` — CI via `check-hotpath-json.sh`)
- Binance live safety glue (refusal gates, time sync, OCO/brackets, REST signing, DMS heartbeats)

**Why**: These areas carry **cross-file invariants**:
- Compile-time live-order gating (absolute via TT_TARGET)
- Manual-recovery-only / terminal halt semantics
- No hot-path allocations or JSON
- No auto-retry on safety paths

Sonnet has a measurable tendency to add "helpful" fallback/retry logic that violates invariants.

**Monte Carlo / simulation layer note**: Generally safe for Sonnet. Invariants to preserve: deterministic per-trial seeding (`base_seed` derives `trial_id ^ magic`), no hidden shared state between trials, `MonteCarloReporter` allocation-light, `--mc-parallel` only with `--thread-preset inline` (conflicts with core pinning otherwise).

## Explicit Anti-Patterns (rejected; from instructions + AGENTS.md)

These are **non-negotiable rejects** (pre-merge checklist must confirm none introduced):
- Retry / backoff / cooldown / auto-resume on kill-switch, DMS, reconciler, watchdog, or halt paths
- Resettable `halt_flag_` (must remain write-once atomic; only manual restart clears)
- `nlohmann::json` (or any JSON) on hot path
- Runtime `allow_live_orders` check or bypass of `target_allows_live_orders()` / compile-time gate
- `HAS_*` / venue specifics in core engine (provider is the sole extension point)
- Second producer on any SPSC RingBuffer (engine event loop is sole producer)
- Reconciler soft-warn / force-continue for convenience (refusal is default except documented spot-testnet carve-out)
- Adaptive / variable heartbeat (DMS uses fixed conservative timers)
- Any change that would allow live orders from non-`engine_live` binary

**Pre-merge safety checklist** (copy into PRs when touching surface):
- Model used (Sonnet/Opus)
- `target_allows_live_orders` not bypassed
- Halt remains terminal
- `check-hotpath-json.sh` + layer-deps pass
- No new retry/soft-warn/HAS_/second-producer/runtime gate
- Model noted in commit / PR
- `LIVE_SAFETY_CCB_APPROVED` token (for frozen files) + clean shadow run + CCB
- References relevant `todo.md` item(s)

## Phase 1 Live-Safety Freeze (mechanical + cultural)

The expanded engine/execution/provider safety surface is mechanically frozen; the exact list is in `scripts/check-live-safety-freeze.sh` and mirrored in `AGENTS.md` and `prerequisites.md`.

- Every edit (even "docs only" describing the surface) requires the token in commit message.
- Enforcement: pre-commit + CI script.
- Requires two-person CCB review + clean ≥4h (target 8h) mainnet `engine_shadow` run (0 drops / unexplained divergence) before merge.
- Even Opus changes must carry token + process.

See `docs/governance/02-prerequisites.md` (full mandatory checklist) and `docs/governance/03-todo.md` (P1-* items).

---

**See for current**:
- `AGENTS.md` (primary)
- `docs/reference/01-instructions.md` (consolidated rules + anti-pattern list)
- `docs/governance/01-prod.md` (invariants + freeze context)
- `docs/governance/02-prerequisites.md`
- `docs/architecture/01-target-architecture.md` (high-level invariants)

Thin extract only. Long-form and evolving rules live in the sources above. Update this pointer on any material change to model policy.
