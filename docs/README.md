# TrueTest Documentation

**Welcome to the documentation for the TrueTest (hft-engine) modular C++23 trading engine.**

This directory contains the full documentation set for the project. The documentation is intentionally split between a few **living governance documents** at the repository root and focused technical documents here.

## Primary Starting Points

| Document | Purpose | Audience |
|----------|---------|----------|
| **[instructions.md](instructions.md)** | **Master Consolidated Reference** - Build, run, CLI, providers, realism models, safety layers, QuestDB, threading, Phase 0 procedures, everything in one place. | Everyone (operators, developers, reviewers) |
| **[user-manual.md](user-manual.md)** | High-level technical overview and architecture | New users & operators |
| [prod.md](../prod.md) | Production readiness playbook, capital-tier phases, Go-Live gates (root) | Anyone increasing live size |
| [CLAUDE.md](../CLAUDE.md) | AI coding rules, model selection, live-safety freeze policy (root) | AI assistants + human reviewers |

---

## Documentation Structure

### Architecture & Design
Located in [`architecture/`](architecture/):

- **target-architecture.md** - Long-term target design and invariants
- **migration.md** - Chronological deepdive change log
- **MODEL.md** - Detailed rationale for AI model selection (Opus vs Sonnet zones)
- **performance.md** + **perf-baseline.md** - Performance methodology and locked baselines
- **engine-optimization.md** - How to investigate and improve hot-path performance
- **realism.md** - Current realism models (fills, latency, impact, queue position)
- **strategy-validation.md** - Planned tooling for honest strategy evaluation

### Operations & Futures Guides
Located in [`operations/`](operations/):

- **futures-phase0-operator-sop.md** - Mandatory printable checklist for every tiny-size mainnet futures session
- **futures-testnet.md** - Full testnet validation guide + 5-scenario DMS playbook
- **testnet.md** - Spot testnet guide (simpler counterpart)
- **demo-trading-workflow.md** - Record real mainnet tape -> deterministic replay -> live shadow
- **futures-order-lifecycle.md** - End-to-end view of what happens to an order on Binance USDT-M futures
- **killswitch-lan-unplug-timeline.md** - Detailed postmortem of a total network partition failure case

### Reference Material
Located in [`reference/`](reference/):

- **db.md** - Complete QuestDB schema, ILP format, and integration details
- **c-api.md** - Stable C embedding API + Python ctypes example
- **licenses.md** - Third-party license inventory and rules

### Production Governance (at repository root)

These are intentionally kept at the root for maximum visibility:

- **prod.md** - The central production contract and phase definitions
- **prerequisites.md** - Checklist that must be green before touching frozen safety surface
- **todo.md** - Current active phase tasks and future phase roadmap

### Phase 0 Evidence (at repository root)

- **`reports/phase0/`** - Live operational evidence for the 15 qualifying tiny-size sessions (PROGRESS.md is the single source of truth, signed notes, batch reviews, etc.)

---

## Historical & Archived Material

Located in [`archive/`](archive/):

- **realism-wiring.md** - Original implementation plan (tasks completed)
- **grok.md** - Dual-portfolio shadow P&L implementation log
- **drift-claude-analysis.md** + **drift-grok-analysis.md** - Two detailed plans for a future Solana/Drift liquidation keeper bot (highly overlapping)

These are retained for audit and historical context but are no longer the active reference.

---

## How to Navigate

1. **I just want to build and run the engine** -> Start with [instructions.md](instructions.md)
2. **I'm preparing for live trading or Phase 0** -> Read [prod.md](../prod.md) + [futures-phase0-operator-sop.md](operations/futures-phase0-operator-sop.md)
3. **I'm reviewing a PR that touches safety** -> Read [CLAUDE.md](../CLAUDE.md) + [MODEL.md](architecture/MODEL.md) + [prerequisites.md](../prerequisites.md)
4. **I need the full technical picture** -> [user-manual.md](user-manual.md) + [instructions.md](instructions.md) + relevant file in `architecture/`

---

**Last updated**: 2026-05 (major reorganization + consolidation)

All cross-references in the master `instructions.md` and scripts have been updated to reflect the new structure. If you find a broken link, please open an issue or PR.