# docs/todos/ Overview & Governance

**Purpose**: Navigate and maintain the numbered living task files. The high-level entry is `docs/governance/03-todo.md`; detailed task text lives here.

**Numbering & Files**:
- 00-OVERVIEW.md (this file): Structure, rules, mapping, reference guide, maintenance.
- 01-P0-phase0.md: Phase 0 Immediate (P0-01..P0-04 + standing + status 0/15).
- 02-P1-freeze.md: Phase 1 / Live-Safety Freeze (P1-01..P1-05 + frozen files list + checklist notes).
- 03-MC-simulation.md: Monte Carlo Simulation (MC-01..MC-06 + standing invariants + landed notes).
- 04-R-risk-management.md: Risk Management (R-01..R-05 + additional Go-Live row 3).
- 05-S-dms-kill-brackets.md: DMS / Kill-Switch / Bracket Hardening (S-01..S-06 + additional).
- 06-D-documentation-structure.md: Documentation & Structure (D-01..D-06 + ongoing rules + aspirational dirs).
- 07-A-adaptive-hybrid.md: Adaptive Hybrid Strategy (A-01..A-07 + note on lower-pri + MC context).
- 08-H-persistence-observability.md: Persistence, Observability & Hardening (H-01..H-07 + additional + QuestDB branch note). H-07 (2026-08-18): mainnet durable-log gate doesn't reject non-regular-file sinks — frozen (`main.inc`) track.
- 09-other-future-gates.md: Other / Nice-to-Have / Future Venues + Go-Live Gate (9 rows) + invariants from AGENTS.md/ENGINE + AI coding rules summary.
- 10-BF-backtest-engine-bugfixes.md: Backtest Engine Bug Fixes (BF-01..BF-17) from the 2026-08-14 entry/exit/PnL/risk/analytics audit; splits into a non-frozen fast track and a frozen-surface (`engine.cpp`) CCB track.
- 11-F-forensic-lifecycle-audit.md: Forensic Trade-Lifecycle Audit Fixes (F-01..F-10b) from the 2026-08-22 runtime-trace audit of `ema-rsi-atr-pullback`. **All items RESOLVED 2026-08-22** in one CCB cycle: F-01 (phantom stop-outs), F-02 (silent strategy deadlock on rejected delayed orders — now one engine-owned terminal-transition emitter, the same seam BF-02/BF-09 want), F-03 (unarmed brackets under synchronous fills), F-04 (sizing blind to execution cost), F-05 (no bankruptcy stop, no spot cash rule), F-06..F-10b. Open questions 4 (live adapter fill-timing certification) and 6 (tick/L2/streaming/shadow/MC paths unaudited) remain.


**How to Reference in PRs / Commits** (authoritative):
- Cite the file and item ID, for example: "Addresses `docs/todos/01-P0-phase0.md` P0-01".
- Bold list labels such as `**P0-01**` do **not** create Markdown fragment anchors. Do not append `#P0-01` unless the target file gains an explicit heading or HTML anchor.
- Every frozen-surface PR (or docs significantly describing that surface) **must** reference relevant item(s) per `docs/governance/02-prerequisites.md` and `AGENTS.md`.

**Maintenance Rules** (enforce per `AGENTS.md`, `docs/governance/01-prod.md`, `docs/governance/02-prerequisites.md`, `docs/README.md`, and D-04/D-06):
- Edit the *specific* numbered file for item text, status, additions, or moves/strikes.
- When a phase exit is declared in `docs/governance/01-prod.md`: update the corresponding numbered todo, the high-level `docs/governance/03-todo.md` summary, and any affected prerequisites or Phase 0 evidence docs.
- Anti-rot: Before any capital tier increase, include "docs verified + links resolve + todo summary updated".
- Extraction: Long-form rituals, templates, and gates stay in governance or a dedicated SOP. `docs/todos/` holds actionable items, status, and context.
- Planned language: aspirational paths must be marked planned and point to the current authoritative document.
- When MC work lands in src/simulation/: update MC section in docs/reference/01-instructions.md + governance (incl. relevant todos/ file + docs/governance/03-todo.md + docs/governance/01-prod.md).
- If broken/stale cross-ref found: treat as doc bug; fix or note exact string.
- Sync lists: Frozen files list in 02-P1-freeze.md must match scripts/check-live-safety-freeze.sh + prod.md + prerequisites.md + AGENTS.md.
- `docs/governance/03-todo.md` is the thin high-level entry; detailed content is authoritative in `docs/todos/`.
- On every update: update "Last update" with summary (e.g. "P0-02 batch review landed; see docs/todos/01-P0-phase0.md").
- Historical: Completed items moved/stuck in the relevant file's "Completed in this group" subsection (or OVERVIEW summary). Large plans stay in docs/archive/.

**Current Focus Summary** (keep in sync with governance): Phase 0: 0/15. Phase 1 enforcement active. Monte Carlo is a research tool only and does not relax safety gates. Read first: `AGENTS.md`, `docs/governance/01-prod.md`, `02-prerequisites.md`, `03-todo.md`, `04-summary.md`, and this file.

**Mapping from Original todo.md Sections**: See section 3 below.

**Historical split decision**: Sections 3–5 record the completed 2026-07 migration; old root-path wording there is historical, not current navigation.

---

## 1. Recommended Directory Structure for docs/todos/

```
docs/todos/
├── 00-OVERVIEW.md
├── 01-P0-phase0.md
├── 02-P1-freeze.md
├── 03-MC-simulation.md
├── 04-R-risk-management.md
├── 05-S-dms-kill-brackets.md
├── 06-D-documentation-structure.md
├── 07-A-adaptive-hybrid.md
├── 08-H-persistence-observability.md
├── 09-other-future-gates.md
├── 10-BF-backtest-engine-bugfixes.md
└── 11-F-forensic-lifecycle-audit.md
```


(No subdirectories; completed items remain in their owning file or the overview summary.)

Rationale for grouping + numbering (logical by existing todo.md sections; sequential for easy ordering; P0 first as current focus):
- 00: Nav, rules, mapping, reference examples (always first)
- 01: Phase 0 (active, highest operational priority; P0-*)
- 02: Phase 1 (enforcement active; P1-*)
- 03: Monte Carlo (recently integrated; MC-*)
- 04: Risk (highest remaining technical per gaps/todo)
- 05: DMS/Kill/Brackets (S-* hardening)
- 06: Documentation (D-* + ongoing rules; self-referential to split)
- 07: Adaptive Hybrid (lower-pri A-*; historical + MC context)
- 08: Persistence/QuestDB/Hardening (H-*; note: all QuestDB work on `database` branch per todo)
- 09: Other/Nice-to-Have + Go-Live Gate rows + future venues (unnumbered bullets + overarching gates)
- 10: Backtest engine defects and their implementation/verification tracks (BF-*)
- 11: Forensic trade-lifecycle audit defects and their verification evidence (F-*)


Filenames chosen for readability (e.g. 01-P0-phase0.md not 01-P0.md) and to match examples in task ("01-P0-phase0.md").

---

## 2. Content for Each File (Full Extraction + Status + Context)

See the individual numbered files. Each contains verbatim extraction of sections/items/status/context paragraphs from the canonical (governance/03-todo.md), with added framing: header, "See 00-OVERVIEW.md", anchors like **P0-01**, last-updated, pointers to prod/reports.

---

## 3. Historical migration mapping (completed 2026-07)

The remaining sections preserve the original split rationale. References to a root `todo.md` describe the pre-migration proposal; current navigation uses `docs/governance/03-todo.md`.

| Original todo.md Section Header (approx line) | Target File | Items Included | Notes / Extra Context to Pull |
|-----------------------------------------------|-------------|----------------|--------------------------------|
| (Header + Status + Post-merge note + How to reference + Current focus + Read first) | 00-OVERVIEW.md (intro) + thin root todo.md | N/A (structure) | Full header philosophy; "Last update"; MC integration note. |
| ## Phase 0 Immediate (Unblock...; 0/15) (~19) + P0-01..04 + Standing + Current status | 01-P0-phase0.md | P0-01 to P0-04 + Standing + Current status | Pull ritual details context from prod.md (template) + reports/phase0/README.md + PROGRESS.md (but keep pointers; do not duplicate long-form). Sync 0/15 note. |
| ## Phase 1 / Live-Safety Freeze (~31) + P1-01..05 + Frozen Files + Mandatory pre-PR + On every phase exit | 02-P1-freeze.md | P1-01 to P1-05 + full frozen-surface mirror + checklist para | Include full P1-02 validation evidence block exactly. Note plan.md ref. Sync the mirror with the authoritative check script. |
| ## Monte Carlo Simulation (integrated) (~69) + MC-01..06 + Standing invariants + From code incompletes | 03-MC-simulation.md | MC-01 to MC-06 + full Standing + incompletes | Landed notes for MC-01/MC-02. Pull caveats from user-manual.md + docs/README.md + MERGE_PLAN.md (historical). Note MC does not relax P0/1. |
| ## Risk Management (~89) + R-01..05 + Additional | 04-R-risk-management.md | R-01..R-05 + Additional (Go-Live row 3) | Reference production-readiness-gaps historical if needed (archive). |
| ## DMS / Kill-Switch / Bracket Hardening (~99) + S-01..06 + Additional | 05-S-dms-kill-brackets.md | S-01..S-06 + Additional (Go-Live row 4) | Note DMS in binance_futures_dead_mans_switch.h etc. |
| ## Documentation & Structure (~110) + D-01..06 + Ongoing + Aspirational / missing dirs | 06-D-documentation-structure.md | D-01 to D-06 + full Ongoing rules list + Aspirational list | This split is execution of D-06. Update cross-refs in this file + others during impl. Explicit planned language examples. |
| ## Adaptive Hybrid Strategy (~139) + A-01..07 + (See detailed spec...) | 07-A-adaptive-hybrid.md | A-01 to A-07 + full note | Link to docs/reference/06-adaptive-hybrid-strategy.md (keep status note in sync). MC-05 context. |
| ## Persistence, Observability & Hardening (~158) + H-01..06 + Additional + (from code/gaps) | 08-H-persistence-observability.md | H-01 to H-06 + Additional + branch note | Link to docs/reference/03-db.md + docs/archive/questdb-multi-week-hardening-guide.md (H-01 explicit). MC-06 note. |
| ## Other / Nice-to-Have / Future Venues (~172) + multi-symbol list + Go-Live Gate rows + Risk resume + QuestDB hard-fail + From AGENTS.md/ENGINE + AI coding rules | 09-other-future-gates.md | All unnumbered bullets + full 9-row Go-Live + invariants + AI rules summary | Cross-ref prod.md Go-Live table (keep long-form in prod). |
| ## Completed / Superseded Items (~196) + Maintenance note + final para | 00-OVERVIEW.md (summary subsection) + bottoms of relevant numbered files | N/A | Move landed (e.g. D-01, MC-01/02, initial governance) with PR context. Maintenance note distributed to 00-OVERVIEW + AGENTS.md/prod cross-refs. |

---

## 4. Historical decision: thin canonical todo

**Recommendation: KEEP a thin canonical `todo.md` at the ROOT (people/scripts continue to reference/edit the high-level stub for status/summary; full item details + context live in docs/todos/*.md).**

**Justification** (based on analysis of refs + maintenance rules + D-06 + AGENTS.md + docs/README):
- **Compatibility**: Dozens of cross-refs assume "root todo.md", "todo.md", "root `todo.md`", "Addresses todo.md #P1-02", "see root `todo.md` (P0-01..)", "MC-* items in root [todo.md]", "update ... / todo.md ...". Changing location of canonical would require coordinated mass edits (README.md, docs/*.md x6+, prerequisites.md, reports/phase0/*, scripts/phase0/create-*.sh, feature.md, user-manual, instructions, AdaptiveHybridStrategy.md, archive/MERGE_PLAN + questdb guide, main.inc comments, test comments, AGENTS.md, prod.md indirect). This is scope creep for "todos/ part".
- **Scripts & rituals**: create-evidence-bundle.sh explicitly says "Update prod.md / todo.md / prerequisites.md". Root path is conventional for governance (alongside prod.md, prerequisites.md, AGENTS.md, summary.md). Thin root preserves "the single source of record" phrasing in header.
- **PR / human ergonomics**: Overview references ("Closes #A-07" or "todo.md P0-03") remain valid. Fine-grained use docs/todos/ paths (as specified in task). Humans start at root todo.md.
- **Maintenance per rules**: Thin root holds high-level "Current focus", "Last update", philosophy, "Read first", and a TOC/links to docs/todos/*.md + "See docs/todos/00-OVERVIEW.md for full split items, numbering, and precise anchors." Detailed edits happen in numbered files. Phase-exit updates hit both (thin root summary + specific file).
- **Alternatives considered & rejected**:
  - Fully move to folder (no thin root): High breakage risk; would force updates to 15+ locations as part of this; contradicts "thin canonical ... that people/scripts edit".
  - docs/todos/00-master.md as canonical editable: Works but less discoverable than root; requires updating "root todo.md" prose everywhere anyway.
  - docs/governance/todo.md: No such dir; would create new convention inconsistent with current "root governance files" language in AGENTS.md/docs/README.
- **Thin root content skeleton** (high-level only; no full item duplication):
  - Header/Status/Post-merge/How to ref/Current focus/Read first (mostly unchanged or slimmed).
  - "Full detailed task items by group live in `docs/todos/` (see `docs/todos/00-OVERVIEW.md` for structure, numbering, anchors, and maintenance). This thin file is the canonical high-level entry point and summary of record."
  - Short status bullets or links: "P0 status: see docs/todos/01-P0-phase0.md (0/15)".
  - "Last update: ... (todos/ split applied; see docs/todos/00-OVERVIEW.md)".
  - Keep "Completed" high-level summary.
- **Future**: If later decided, root can become pure symlink stub or redirect note, but for this overhaul keep thin root.

**Impl note**: After creating docs/todos/ files, thin the root todo.md (do not delete full content until split files are in place and verified; use search/replace to slim + add pointer). Update D-06 language if present.

---

## 5. Cross-Reference Updates Performed

- Updated examples in prerequisites.md (docs/governance/02-prerequisites.md), docs/README.md, reports/phase0/*, user-manual, AdaptiveHybridStrategy (now in reference/), instructions.md, create-evidence-bundle.sh, root README.md, archive notes, src comments where applicable, prod.md, AGENTS.md, summary.md, governance files.
- Precise item refs cite the file and item ID, e.g. `docs/todos/01-P0-phase0.md` P0-01; high-level references note "full details in docs/todos/ (see 00-OVERVIEW.md)".
- Backward compat: "Addresses todo.md #P0-03" preserved in thin file and where historical.
- Frozen files list in 02-P1-freeze.md verified identical to scripts/check-live-safety-freeze.sh + docs/governance/02-prerequisites.md + docs/governance/01-prod.md + AGENTS.md .
- Also updated internal governance references post-reorg for accuracy where they intersected todo split (e.g. governance/03-todo.md thinned to point to docs/todos/).

**Last updated**: 2026-08-22 (F-01..F-10b landed — forensic trade-lifecycle audit closed in one CCB cycle; see 11-F-forensic-lifecycle-audit.md).


See thin `docs/governance/03-todo.md` for high-level canonical entry + individual `docs/todos/NN-*.md` for full verbatim task details.
