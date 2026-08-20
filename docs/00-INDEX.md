# TrueTest Documentation — Master 01-N Reading / Processing Order

**Status**: Canonical **linear** reading sequence for operators, reviewers, and implementers.

**Topic navigation** (by task): [`docs/README.md`](README.md).  
**Authoritative gates/tasks**: root `AGENTS.md`, `docs/governance/`, `docs/todos/`, `reports/phase0/`.

**Last updated**: 2026-08-19 (engine-decomposition Phase 3: added `docs/architecture/05-engine-boundaries.md`)

## Recommended 01-N order

01. Root `README.md` — High-level overview, binaries, phases table, safety surface, quick start.
02. `AGENTS.md` — AI/agent coding rules, Phase 1 freeze, hot-path + safety red lines, preferred build commands.
03. `docs/governance/01-prod.md` — Production contract: phases, Go-Live Gate, Phase 0 ritual.
04. `docs/governance/02-prerequisites.md` — Mandatory pre-PR checklist for frozen-surface edits.
05. `docs/governance/03-todo.md` (thin) + `docs/todos/` (detailed `00-OVERVIEW` … `10-…`) — Living tasks; cite the file and item ID in PRs.
06. `docs/governance/04-summary.md` — Condensed status.
07. `reports/phase0/README.md` + `PROGRESS.md` + `templates/phase0-session-note.md` — Phase 0 evidence.
08. `docs/reference/01-instructions.md` — Master technical reference (CLI, providers, realism, MC).
09. `docs/reference/02-user-manual.md` — Architecture + operator overview.
10. `docs/architecture/01-target-architecture.md` — Target architecture sketch.
11. `docs/architecture/02-model.md` — Model selection + anti-patterns.
12. `docs/architecture/03-realism.md` — Latency/impact/queue/fill/fee models.
13. `docs/architecture/05-engine-boundaries.md` — Engine invariants, state-ownership matrix, dependency-direction rules, "when do I modify Engine?" (engine-decomposition Phase 3).
14. `docs/operations/01-futures-phase0-operator-sop.md` — Qualifying Phase 0 SOP (Binance mainnet).
15. `docs/operations/02-futures-testnet.md` — Non-qualifying testnet drills.
16. `docs/operations/03-bitget-demo.md` — Bitget demo (not Phase 0 qualifying).
17. `docs/platforms/README.md` (+ `bitget.md`, `bitunix.md`) — Multi-venue status.
18. `docs/architecture/04-performance.md` — Capacities, threading, limits.
19. `docs/reference/03-db.md`, `04-flags.md`, `05-web-ui.md`, `06-adaptive-hybrid-strategy.md`, `07-strategy-development.md` — Specialized reference (strategy authors → 07).
20. `docs/archive/` — Historical only (MERGE_PLAN, gaps snapshot, QuestDB logs, Edge1). Not current status.
21. `docs/internal/engine-decomposition.md` + `engine-decomposition-design.md` — `engine.{h,cpp}` decomposition plan (+ skill `engine-decomposition`).
22. `docs/internal/data-pipeline.md` — Market-data path redesign (`MarketSeries`, `DataWrapper`; mostly shipped).
23. `docs/internal/imgui-desk-design.md` — ImGui desk status and remaining research-panel wiring.
24. `docs/skills/00-OVERVIEW.md` — Proposed agent skills and retained execution records.
25. `docs/decisions/2026-08-14-live-safety-repair.md` — Approved live-safety repair decision and acceptance criteria.
26. `docs/todos/10-BF-backtest-engine-bugfixes.md` — Backtest defect backlog and frozen-surface sequencing.
27. `docs/internal/r3-authoritative-risk-accounting.md` — Authoritative order/position ledger and mark-to-market risk snapshot (risk register R3): removed proxies, data flow, VaR and funding decisions.

## Processing notes

| Goal | Path through the index |
|------|------------------------|
| **Phase 0 operator** | 01 → 03 → 07 → 13 → 08 |
| **Safety PR reviewer** | 02 → 04 → 05 → run freeze script |
| **Full technical picture** | 09 → 08 → 10–12 → 17 |
| **MC / research** | 08 (MC section) + 09 + `todos/03-MC-simulation.md` |
| **Strategy author** | 18 → `reference/07-strategy-development.md` (+ 08 flags) |
| **Multi-venue** | 16 + 15 + CMake presets `linux-bitget` / `linux-bitunix` / `linux-venues` |
| **Engine decomp implementer** | 20 + `internal/engine-decomposition-design.md` + skill |

Broken links = documentation bugs. Prefer explicit “Planned …” language for unbuilt paths.
