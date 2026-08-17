# TrueTest Documentation

Documentation for the TrueTest (`core/`) modular C++23 trading engine.

**Start here for topic-based navigation.** For a linear reading order, use [`00-INDEX.md`](00-INDEX.md). Agent rules live in root [`AGENTS.md`](../AGENTS.md).

## Primary starting points

| Document | Purpose | Audience |
|----------|---------|----------|
| **[reference/01-instructions.md](reference/01-instructions.md)** | Master technical reference — build, CLI, providers, realism, MC, checklists | Everyone |
| **[reference/02-user-manual.md](reference/02-user-manual.md)** | High-level architecture and operator overview | New users & operators |
| **[governance/01-prod.md](governance/01-prod.md)** | Production contract, capital tiers, Phase 0 ritual, Go-Live gates | Anyone raising live size |
| **[../AGENTS.md](../AGENTS.md)** | AI/agent coding rules, freeze surface, hot-path red lines | Agents + reviewers |
| **[00-INDEX.md](00-INDEX.md)** | Recommended 01-N reading / processing order | Structured onboarding |

---

## Directory map

```text
docs/
├── README.md                 # this file — topic nav
├── 00-INDEX.md               # linear reading order
├── architecture/             # target architecture, model, realism, performance
├── governance/               # prod contract, freeze prereqs, thin todo, summary
├── todos/                    # detailed living work items (P0/P1/MC/R/S/D/…)
├── reference/                # CLI, manual, flags, DB, web UI, strategies
├── operations/               # Phase 0 SOP, testnet, venue demo drills
├── platforms/                # multi-venue status + provider design notes
├── internal/                 # active engineering design plans (not operator SoT)
├── decisions/                # dated engineering decisions and acceptance criteria
├── skills/                   # proposed agent-skill designs (planning only)
├── archive/                  # historical snapshots; not current status
└── assets/                   # static images
```

| Directory | Role | Stability |
|-----------|------|-----------|
| **governance/** | Authoritative prod gates, freeze checklist, thin todo stub | **Do not rename** — cited by scripts + PR process |
| **todos/** | Numbered work items cited by file and item ID (e.g. `docs/todos/01-P0-phase0.md` P0-01) | **Do not rename** |
| **reference/** | How-to and developer/operator reference | Stable numbered files |
| **architecture/** | Extracted architecture model (not full design drafts) | Stable numbered files |
| **operations/** | Operator SOPs and venue drills | Stable; phase0 scripts print these paths |
| **platforms/** | Venue status (Binance golden, Bitget landed, Bitunix MD/shadow with live refused) | Renamed from `upcoming_platform/` |
| **internal/** | Active design plans (engine decomp, data pipeline, cmake design) | Plans may move; update AGENTS + skill refs |
| **skills/** | Proposed Grok skills plus retained execution records | Renamed from `upcoming_skills/` |
| **archive/** | Historical / deferred packs (Edge1, old gaps, QuestDB hardening logs) | Historical only |
| **assets/** | Images | Stable |

Evidence for Phase 0 sessions lives outside this tree: **`reports/phase0/`**.

---

## What lives where (quick index)

### Governance & tasks
- [`governance/01-prod.md`](governance/01-prod.md) — phases, ritual, Go-Live Gate
- [`governance/02-prerequisites.md`](governance/02-prerequisites.md) — freeze PR checklist
- [`governance/03-todo.md`](governance/03-todo.md) — thin high-level todo
- [`todos/`](todos/) — detailed items (`00-OVERVIEW.md`, `01-P0-…` … `10-BF-…`)
- [`governance/04-summary.md`](governance/04-summary.md) — condensed status

### Reference (operators & developers)
- [`reference/01-instructions.md`](reference/01-instructions.md) — CLI / build / MC / checklists
- [`reference/02-user-manual.md`](reference/02-user-manual.md) — architecture overview
- [`reference/03-db.md`](reference/03-db.md) — QuestDB
- [`reference/04-flags.md`](reference/04-flags.md) — flag matrix
- [`reference/05-web-ui.md`](reference/05-web-ui.md) — browser cockpit
- [`reference/06-adaptive-hybrid-strategy.md`](reference/06-adaptive-hybrid-strategy.md) — retired prototype and rebuild contract
- [`reference/07-strategy-development.md`](reference/07-strategy-development.md) — **IStrategy SDK** (indicators, entry, exit, TP/SL)
- [`reference/LAUNCH_SCRIPTS.md`](reference/LAUNCH_SCRIPTS.md) — launcher inventory; see its status warning and prefer the direct commands in `01-instructions.md`

### Architecture
- [`architecture/01-target-architecture.md`](architecture/01-target-architecture.md)
- [`architecture/02-model.md`](architecture/02-model.md)
- [`architecture/03-realism.md`](architecture/03-realism.md)
- [`architecture/04-performance.md`](architecture/04-performance.md)

### Operations
- [`operations/01-futures-phase0-operator-sop.md`](operations/01-futures-phase0-operator-sop.md) — qualifying Phase 0 SOP
- [`operations/02-futures-testnet.md`](operations/02-futures-testnet.md) — non-qualifying testnet drills
- [`operations/03-bitget-demo.md`](operations/03-bitget-demo.md) — Bitget demo (not Phase 0 qualifying)

### Platforms (venues)
- [`platforms/README.md`](platforms/README.md) — multi-venue map
- [`platforms/bitget.md`](platforms/bitget.md), [`platforms/bitunix.md`](platforms/bitunix.md)

### Internal design plans (engineering)
- [`internal/engine-decomposition.md`](internal/engine-decomposition.md) — `engine.{h,cpp}` decomp plan (+ skill `engine-decomposition`)
- [`internal/engine-decomposition-design.md`](internal/engine-decomposition-design.md) — Phase 1 design + PR DAG
- [`internal/data-pipeline.md`](internal/data-pipeline.md) — market-data path redesign (`MarketSeries` / `DataWrapper`; mostly shipped)
- [`internal/2026-07-cmake-modernization-design.md`](internal/2026-07-cmake-modernization-design.md)
- [`internal/imgui-desk-design.md`](internal/imgui-desk-design.md) — ImGui desk status and planned research wiring

### Decisions
- [`decisions/2026-08-14-live-safety-repair.md`](decisions/2026-08-14-live-safety-repair.md) — approved live-safety repair scope and acceptance criteria

### Skills (proposals + execution records)
- [`skills/00-OVERVIEW.md`](skills/00-OVERVIEW.md) — proposed agent skills and retained execution records; no matching project SKILL.md files

### Archive
- [`archive/`](archive/) — MERGE_PLAN, production-readiness-gaps-2026-05, QuestDB hardening logs, Edge1 cointegration packs

---

## How to navigate (by task)

1. **Build and run the engine** → [reference/01-instructions.md](reference/01-instructions.md)
2. **Phase 0 / live capital** → [governance/01-prod.md](governance/01-prod.md) + `reports/phase0/` + [operations/01-futures-phase0-operator-sop.md](operations/01-futures-phase0-operator-sop.md)
3. **Safety PR review** → [../AGENTS.md](../AGENTS.md) + [governance/02-prerequisites.md](governance/02-prerequisites.md) + [todos/02-P1-freeze.md](todos/02-P1-freeze.md)
4. **Full technical picture** → [reference/02-user-manual.md](reference/02-user-manual.md) + [reference/01-instructions.md](reference/01-instructions.md) + [architecture/](architecture/)
5. **Monte Carlo / stochastic backtests** → MC section in [reference/01-instructions.md](reference/01-instructions.md) + [todos/03-MC-simulation.md](todos/03-MC-simulation.md)
6. **AI coding agent** → [../AGENTS.md](../AGENTS.md) first, then this README + [00-INDEX.md](00-INDEX.md)
7. **Multi-venue (Bitget / Bitunix)** → [platforms/README.md](platforms/README.md) + [operations/03-bitget-demo.md](operations/03-bitget-demo.md)
8. **Write a strategy (`IStrategy`)** → [reference/07-strategy-development.md](reference/07-strategy-development.md) (retired Adaptive Hybrid rebuild contract: [06](reference/06-adaptive-hybrid-strategy.md))
9. **Engine god-class decomposition** → [internal/engine-decomposition.md](internal/engine-decomposition.md)
10. **Data ingress / CSV / DataWrapper** → [internal/data-pipeline.md](internal/data-pipeline.md)

---

## Documentation hygiene

- **Code is truth** for APIs; docs must match shipped behaviour.
- **Do not rename** `governance/` or `todos/` paths without updating scripts, AGENTS, and PR citation conventions.
- Aspirational / unbuilt paths must say so explicitly: *“Planned … — current details live in …”*.
- Broken links = documentation bugs.
- Long-form phase/ritual content lives in `governance/01-prod.md` (or ops SOPs); `reference/01-instructions.md` keeps pointers + command templates.
- Build lists: `cmake/Sources.cmake`; presets: `CMakePresets.json` (`out/build/<preset>/` vs ad-hoc `build/`). Build presets and test presets default to one job; `linux-release-low-memory` also disables LTO. Details: [reference/01-instructions.md](reference/01-instructions.md).

**Last updated**: 2026-08-16 — serial preset defaults and low-memory build guidance.
