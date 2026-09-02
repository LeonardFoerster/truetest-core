# TrueTest Production Readiness Playbook (prod.md)

**Status**: Authoritative – the central production contract and capital-tier governance document.  
**Last major update**: 2026 (post-merge; Monte Carlo to master merge — all phases completed successfully; todos/ split cross-refs 2026-07)  
**Owners**: Core maintainers + Phase 0/1 operators (every phase exit must update this file).

This document defines the exact phase definitions, capital-tier gates, Go-Live checklist, Phase 0 operator ritual, and "why we are careful" philosophy. It is the single source of truth that reviewers, CCB members, and future operators consult before any increase in live capital.

See also:
- `../../AGENTS.md` (AI + human reviewer rules + live-safety freeze mechanics)
- `02-prerequisites.md` (mandatory pre-PR checklist for the frozen safety surface)
- `03-todo.md` (thin high-level canonical task list; every frozen-surface PR must reference items here or a specific todo file and item ID, e.g. `docs/todos/01-P0-phase0.md` P0-01, per 00-OVERVIEW.md)
- `reports/phase0/PROGRESS.md` (evidence tracker)
- Historical snapshot: `../archive/production-readiness-gaps-2026-05.md` (May 2026 view; current gaps/status tracked in `03-todo.md` + docs/todos/ + this file)

**docs/ is now the central authoritative documentation home.** Last updated: 2026-07 (docs overhaul). This file (01-prod.md) is the source of truth for phases/gates/ritual. Other files use short pointers.

---

## Philosophy & Invariants

> "The engine already implements strong compile-time and runtime safety primitives that most retail or early-stage systems lack... This playbook exists so that future operators cannot say 'we forgot why we were careful.'"

**Core non-negotiable rules** (repeated in `AGENTS.md`, instructions, architecture docs, and every safety review):
1. Compile-time live-order gate is absolute (`TT_TARGET` + `target_allows_live_orders()` in `src/core/tt_target.h`).
2. Halt is terminal (`halt_flag_` is write-once; only manual restart clears it).
3. Safety paths are loud and non-retrying (no auto-resume, no helpful fallbacks on kill/DMS/reconciler/watchdog).
4. Hot-path discipline (no `nlohmann::json`, no allocations, lock-free SPSC only, CI-enforced).
5. Reconciler refusal is default (drift > tolerance blocks live start).
6. User-data WebSocket is source of truth.
7. Venue DMS protects orders only. Its first heartbeat failure latches terminal halt; the engine-owned exact-once kill session performs the single orderly flatten attempt before provider close. A failed/unknown kill leaves the countdown armed.
8. Futures mandates (one-way mode, `closePosition=true` conditional brackets
   without the venue-forbidden `reduceOnly`/quantity combination, venue
   `FuturesRiskCheck` first).
9. Small capital first + evidence-based gates (no tier increase without artifacts + two signatures on the full Go-Live Gate).

**Intended Use & Scope**: TrueTest is a private, personal research and retail tool for the author only. It is not, and will never be, an enterprise-ready, institutional, or production trading system. Monte Carlo simulation, high-fidelity backtesting, and shadow divergence analysis are the primary mature capabilities. The live execution paths (`engine_live`) exist with unusually strong compile-time (`TT_TARGET`) and runtime safety layers (reconciler, DMS, kill-switch, venue risk checks, terminal halt, user-data source of truth, etc.). Any use of live paths is experimental, tiny-size, fully attended by the operator, and done at the author's own risk. The Phase 0/1 rituals and Go-Live language in this repository describe the author's personal evidence-gathering hygiene and self-imposed discipline — they are **not** a formal production release process or claim of readiness for others.

---

## Capital-Tier Phases & Exit Criteria

**Strict rule** (for the author only): Moving to a higher personal capital tier requires all prior phase exit criteria + two-person sign-off on the 9-row Go-Live Gate table below. The author will never treat this as a path to external or institutional use.

### Phase 0 — Safe Tiny-Size Mainnet Futures (Current Active Phase)

**Exact recommended command template** (conservative caps; must meet or exceed these for a qualifying session):

```bash
RUN_TAG="p0_$(date -u +%Y%m%d_%H%M)"
./build/engine_live \
  --provider binance-futures \
  --symbol BTCUSDT \
  --stream trade --depth-stream depth20@100ms \
  --thread-preset standard \
  --live \
  --api-key "${BINANCE_FUTURES_KEY}" --api-secret "${BINANCE_FUTURES_SECRET}" \
  --log-events "./event_log_${RUN_TAG}.bin" \
  --persist --run-tag "${RUN_TAG}" \
  --reconcile-tolerance-bps 3 \
  --dead-man-countdown-ms 30000 --dead-man-heartbeat-ms 8000 \
  --max-notional 15000 --max-leverage 2.5 --min-liq-distance-pct 0.07 \
  --max-daily-loss 80 --risk-unwind
```

**Why each element is mandatory**:
- `--depth-stream depth20@100ms`: enables realistic queue/impact/L2 models in shadow.
- `--log-events`: reserves a unique new mainnet session ledger. Keep `--log-max-size 0` (the default) and a dedicated logging worker (`standard`, `full`, or `extended`).
- `--persist`: writes the secondary, queryable QuestDB observability stream (see questdb-multi-week-hardening-guide.md).
- DMS + reconciler + three futures risk caps + daily-loss/unwind: layered safety nets.
- Tiny notional + low leverage + conservative liq distance: "prove the system, not the P&L".

**Event-log authority and durability contract**: Only a cleanly sealed,
current-v3, non-segmented event log is eligible for authoritative ledger replay.
Mainnet live atomically creates a unique new regular file beneath existing,
symlink-free, trusted directory ancestry (owned by root/the namespace root/the
operator; group/world-writable ancestors must be sticky). The reservation is
claimed once, before any logger can
truncate the file; a failed logger construction burns that session path rather
than allowing another writer to reuse it. Mainnet pins and verifies the file and
parent identities, synchronizes the preamble before provider startup, refuses
rotation and non-dedicated logging presets, and revalidates the same file during
checkpoints and finalization. Every logger acquires an exclusive advisory lock
before truncation; rotation retains the sealed segment lock until the successor
base pathname is also locked. A sealed replayer pins the opened inode, holds a
shared lock, and rechecks pathname identity, size, link count, owner/mode, and modification
metadata before and after record delivery; a changed source loses authority.
The writable `EventLogger` is a move-only capability held outside the copyable
`engine_config`: startup transfers it exactly once into the engine and then the
logging worker. The copyable reservation contains no writer authority and a
second logger claim is permanently refused. The reverse durability channel is
also role-separated: a move-only producer endpoint exists only in the logging
worker and a move-only consumer endpoint exists only in the engine, so producer
code cannot pop and consumer code cannot push.

Before a normal order or the engine's generic `risk_unwind` order can reach an
execution adapter, the engine publishes that exact order intent and waits up to
the fixed two-second admission deadline for the logging worker's matching
`(order, order_id)` acknowledgement. The worker emits that acknowledgement only
after the reserved logger has flushed and file/directory-fsynced the order and
rechecked path identity. Both durable completion and engine consumption of the
ACK must occur before the deadline. The engine then covers audit/setup and the
adapter call with a lock-free terminal admission gate; its CAS is the
linearization point. A command admitted before a concurrent halt is already in
flight, while the halt closes admission for every later command. Mismatch,
timeout, logging/setup failure, post-ACK admission refusal, or a halt observed
before admission makes the ledger compromise sticky, halts, and refuses
submission. The two-second deadline bounds the engine's ACK wait, not a kernel
`fsync` call. It also does not bound any earlier blocking publication to another
worker ring, provider shutdown, or the later worker join.

Fills, funding, and rejections (as well as cancel/amend records once emitted)
still receive immediate durable checkpoints only when the logging worker
consumes them. This implementation is therefore **not** a complete command WAL,
an exactly-once execution protocol, or crash recovery. Durable-before-command
intent/outcome coverage for cancel, amend, venue-native bracket operations, and
kill-switch/DMS/native safety cancel-or-flatten commands, plus state
reconstruction, remains H-03/follow-up work.

Finalization is explicit and completeness-gated after provider
quiesce/kill/close succeeds. The engine drains stable final tracked funding,
synthetic metadata, submit results, and fills before joining workers. Late fills
use teardown-only accounting/logging that cannot invoke strategies, ExitManager,
or position callbacks and therefore cannot place or cancel venue brackets after
close. Failed/ambiguous provider shutdown, final-outcome processing failure,
uncertain mutation outcomes, funding-ingress loss, any expected worker/ring
topology mismatch, any worker failure/backlog/drop, a missing or non-empty
market-maker outbound ring, a missing/compromised/non-empty ACK channel, or an
earlier sticky compromise abandons the ledger without invoking finalization.
Never-submitted latency/bar-delay/stop scheduler entries are retired locally
during teardown without calling an adapter; failure to retire them also blocks
a reserved seal.
Streaming callback/transport exceptions converted by `DataBridge` to
`runtime_failure` enter the same terminal rollback; the halt reference is
scoped to the active call and is never retained by a reusable bridge. Unknown
venue-native bracket/kill
outcomes remain in the explicit H-03 non-goal above. A reserved logger
destructor never seals the file implicitly. Each v3 record carries CRC32C;
finalization first writes and flushes
the index/trailer and fsyncs file plus parent while no seal exists, then appends
a fixed terminal seal committing the covered byte length, record count, index
offset, and CRC32C of all covered bytes, followed by a second flush/fsync and
identity check. A complete, internally valid terminal seal is the crash-recovery
commit authority even if the writer reported an uncertain final `fsync`; a
qualifying operational run must additionally record both phases as successful.
Observational records checkpoint after 256 logger-consumed records or when a
later record observes at least 100 ms since completion of the prior sync. Worker
backlog, an idle final observational record, or abrupt termination can leave an
unsealed prefix, which is diagnostic only. CRC32C detects accidental corruption;
it is not authentication or cryptographic tamper evidence. Advisory locks are a
cooperation contract, not mandatory kernel write prevention: malicious same-UID
code can ignore them and can still alter the path or file. Such an actor is
outside this integrity boundary, and non-mainnet logs do not use the mainnet
reservation policy.

Worker shutdown itself is not deadline-bounded today. A callback blocked in a
kernel filesystem operation can prevent `join()` from returning; C++ offers no
safe timed join or portable cancellation for that case. Unsafe detach would
outlive engine-owned state, so bounded termination requires H-08's cooperative
cancellation or supervised-helper redesign. This is fail-closed for ledger
authority—a blocked logger never reaches the explicit seal—but remains an
availability and operational-supervision gap.

**Exit criteria for Phase 0 → Phase 1**:
- 15+ fully documented qualifying sessions across ≥3 volatility regimes (High/Med/Low, classified via `scripts/phase0/volatility-classifier.sh` on 7/14d BTC realized vol).
- Zero unexplained drift > tolerance in any session.
- **Full artifacts for every session**: zstd binary event log, QuestDB `run_tag`, signed one-page session note (template in `reports/phase0/templates/`), row in `reports/phase0/PROGRESS.md`, post-halt `grep` review for POSITION-SNAPSHOT/funding/drift.
- Two-person batch reviews every 5 sessions.
- All evidence committed under `reports/phase0/`.

**Ritual** (see the printable SOP in `../operations/01-futures-phase0-operator-sop.md`; this file remains authoritative for gates and exit criteria):
Print/sign the SOP, use `new-session.sh`, keep math-captcha visible the entire session, stay at the terminal, confirm one-way mode, watch DMS counter, run mandatory post-halt grep, run `post-session.sh` + classifier, commit artifacts + note, update PROGRESS.md.

**Current status (2026-09-01)**: 0/15 qualifying. The literal
`LIVE_SAFETY_CCB_APPROVED` token was supplied for the current worktree edit, but
there is no commit/body-token evidence, human two-person CCB approval, or clean
continuous ≥4-hour mainnet `engine_shadow` evidence for it. The worktree is not
merge-ready or live-ready, and this technical implementation does not advance
Phase 0 or close the Phase-1 exit gates.

### Phase 1 — Deepdive Stabilization & Live-Safety Freeze (Required before meaningful size)

**Already completed in planning / mechanical artifacts**:
- The engine, execution, Binance/Bitget provider, REST safety lane, risk, worker, and target-gate files that can change live halt/kill behavior are mechanically frozen. `scripts/check-live-safety-freeze.sh` is the exact source of truth; `AGENTS.md` mirrors it.
- Enforcement script wired into pre-commit + CI.
- AGENTS.md and reference/01-instructions.md updated with model-selection + CCB rules.
- `02-prerequisites.md` created (mandatory pre-PR checklist).

**Remaining exit criteria**:
- Complete the current deepdive + per-lot / queue-position / hybrid executor refactor.
- Clean 8-hour (or longer) mainnet `engine_shadow` run with zero drops / unexplained divergence.
- Two-person sign-off recorded in `decisions/phase1-freeze-*.md` (or equivalent under the decisions/ tree).
- Update this file and `03-todo.md` (thin) + relevant docs/todos/ file to mark Phase 1 complete.
- All future edits to any frozen file must carry the token `LIVE_SAFETY_CCB_APPROVED` in the commit message + pass the check script + 4h+ shadow validation.

**All future safety-surface PRs** require the token, CCB review, and clean shadow run even if they are "only docs" that describe the surface.

### Phase 2 — Risk Engine Completion (Highest Impact Remaining Work)

- Funding as first-class event (wired into Portfolio, analytics, risk, QuestDB, TUI, circuit breakers).
- Real tiered maintenance-margin liquidation simulation (already partially landed via `MaintenanceMarginTable` + `/fapi/v1/leverageBracket`).
- Position sizing as % of equity + volatility-adjusted limits.
- Configurable extreme-event circuit breakers (spread, funding spikes, etc.).

**Monte Carlo simulation (research & validation tooling)**

Monte Carlo simulation (introduced on the monte-carlo branch and now mainline) is a research and strategy-robustness tool for stochastic backtesting (`--monte-carlo --mc-trials N`, `--provider synthetic`, object reuse between trials, experimental parallel execution). It reuses the existing strategy, realism, analytics, and ExitManager surfaces.

**This is a research and risk-distribution tool.** It does not replace or accelerate the Phase 0/1 mainnet shadow/live evidence requirements, does not change the live-order safety surface, and should not be used as a substitute for real-market divergence tracking. See `../reference/01-instructions.md` (Monte Carlo section) and `03-todo.md` (MC-* items) for current status, limitations (stylized synthetic L2, experimental parallelism, etc.), and open work items.

Status notes live in `03-todo.md` + docs/todos/ (and historical `../archive/production-readiness-gaps-2026-05.md`).

### Phases 3–6 (High-Level Roadmap)

- **3**: Centralized exact-once kill/flatten on the first DMS heartbeat failure is
  implemented but remains behind the CCB, clean-path, and soak evidence gates.
  The external `tt_watchdog` defense against SIGSTOP remains future work; DMS
  itself never submits a competing close.
- **4**: `--persist-strict` startup/runtime failure propagation is implemented
  for normal engine runs but still needs the required live evidence. Mandatory
  binary-log integrity (xxhash), resumable v2 checkpoints, and crash-replay
  golden tests remain future work.
- **5**: Prometheus + `IAlertSink`, encrypted credential store + rotation, expanded runbooks.
- **6**: 60+ day continuous mainnet shadow divergence report, formal incident post-mortems for every halt, CCB charter + decision log, signed capital-tier exit review.

---

## Final Go-Live Gate Table (9 Rows)

**For Private Retail Use Only**: These gates, phases, and rituals describe the author's personal evidence-gathering discipline and self-imposed limits for tiny, fully attended personal experiments only. They are not a formal release process or claim of readiness for any other use or capital.

**No increase in the author's personal live capital tier is permitted until all nine rows have two signatures + concrete evidence.**

1. All prior phases met.
2. 60-day shadow report (published or internally audited).
3. Funding + tiered MMR exercised for ≥30 days.
4. DMS first-failure latch, callback-race delivery, exact-once kill-before-close, and failed-kill countdown preservation tested.
5. `--persist-strict` + encrypted creds demonstrated on ≥10 sessions.
6. Prometheus / alerting drill executed successfully.
7. All critical runbooks walked by at least two operators.
8. CCB size-increase request formally approved.
9. Independent safety review (internal or external) with written sign-off.

---

## How to Use This Document

- **Preparing a Phase 0 session** → Read the Phase 0 section + the operator SOP + run `new-session.sh`.
- **Reviewing a PR that touches safety** → Read the Phase 1 freeze rules + `prerequisites.md` + run the check script.
- **Considering any increase in the author's personal live capital** → Read the entire Go-Live Gate table + the most recent `reports/phase0/PROGRESS.md` + current `03-todo.md` (or docs/todos/ for items). (Historical May 2026 gaps view lives in `../archive/production-readiness-gaps-2026-05.md`.) When the author chooses to collect evidence toward personal live use...
- **Updating after a phase exit** → Edit this file (mark the phase complete, record sign-offs, update the roadmap), update `03-todo.md` + relevant docs/todos/*.md , and reference the PR in the migration log.

**If you find a broken or stale cross-reference**, treat it as a documentation bug and either fix it or open an issue with the exact string that needs updating.

---

*This playbook is deliberately repetitive on the invariants. Future operators must be able to read only this document (plus the SOP for the current phase) and still understand exactly why every safety mechanism exists and what the capital gates require.*

**Last updated: 2026-07 (docs overhaul)** — docs/ is now the central authoritative documentation home. Cross-refs updated to use docs/governance/ paths.
