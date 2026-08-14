# Skill Proposal: ui-refactor

**Proposed name**: `ui-refactor`  
**Category**: Refactor Guardian  
**Priority**: High  
**Target scope**: Project  
**Status**: Executed 2026-08-14 (see completion note below) — kept as reference for the pattern, not as an open item
**Source**: ` (retired one-time review docs) 05-ui-large-files.md`

## Completion note (2026-08-14)

Executed as part of a broader desk+TUI improvement pass on the `imgui` branch. Findings and outcome, since the numbers below have moved since this doc was written:

- The `tabbed_dashboard.cpp` (~1215) / `console_dashboard.cpp` (~1097) LOC cited below were already stale by the time this ran — actual counts were 993/892 (the `src/ui/panels/*` extraction and `console_format.cpp` split referenced in this doc had already happened). Both were already under the 1k guideline, so "get under 1000 lines" was not the live problem; the real remaining issues were a 448-line monolithic `ConsoleDashboard::render_tui()` (over half its file) and concrete duplication between the two files.
- `render_tui()` was split into 12 named `append_*_row()` methods (title/state/price/threads/events/pnl/unrealized/toxicity/quotes/risk/rings/recent), each independently readable; `render_tui()` itself is now a ~65-line orchestrator that calls them in order plus the unchanged diff-based repaint engine.
- Extracted `spread_bps()` and `total_ring_drops()` into `console_format.h`/`.cpp` (both ncurses-free, already the shared formatting layer) - previously each was hand-computed inline, separately, in both `console_dashboard.cpp` and `tabbed_dashboard.cpp`. Covered by new tests in `tests/test_console_format.cpp`.
- Fixed a real bug found via this dedup: `TabbedDashboard::compute_render_digest()`'s change-detector was only mixing 3 of the 6 `ring_drops_*` counters (logging/risk/stats — missing observer/risk_stats/mm), so drops in those three could climb without ever triggering a status-bar redraw. Now uses `total_ring_drops()`, covering all 6.
- `TabbedDashboard::handle_input()`'s self-contained `/`-filter prompt (own RAII guard, own blocking `getch()` loop) was extracted to `run_filter_prompt()`.
- **Deliberately NOT merged**: `state_label()`/`state_color()` (console_format.h) vs. `tabbed_dashboard.cpp`'s local `state_text`/`get_state_color` lambdas, and `fmt_duration()` vs. tabbed's adaptive uptime formatting. Both pairs looked like duplication in the initial analysis but turned out to be *intentionally* different presentations for different space constraints (terse "reconn"/"HALT" status-bar labels vs. full "RECONNECT"/"HALTED" box labels; fixed-width "HH:MM:SS" box uptime vs. adaptive "up 1h23m" status-bar uptime) — merging them would have been a visible UX regression, not a cleanup. Left in place, now with a comment at the tabbed_dashboard.cpp call site explaining why.
- Net effect: LOC went up slightly (993→1000, 892→913) from the added method signatures/comments — this refactor was never primarily about the line count; both files' worst single-function-readability problem is gone, and the cross-file duplication (that had already caused one bug) is now a single source of truth.
- Not done, and deliberately out of scope for this pass: further splitting `handle_input()` (244 lines) or `draw_status_bar()` (219 lines) beyond the filter-prompt extraction. Both are already-coherent single state machines / single status bars; forcing further splits for a line-count target alone wasn't judged worth the added risk. Revisit only if either grows materially from here.

---

## One-Sentence Description

A specialized refactor guardian skill that drives the decomposition of the two oversized UI dashboard implementations (`tabbed_dashboard.cpp` ~1215 LOC and `console_dashboard.cpp` ~1097 LOC) into focused, maintainable components while strictly respecting hot-path discipline, snapshot seam usage, and the read-only nature of the current web + TUI surfaces.

---

## Why This Skill Is Needed

From the July 16 2026 code review:

- Both main dashboard files exceed the project's informal 1k LOC guideline by a significant margin.
- They contain a large amount of rendering, state management, and widget logic mixed together.
- The review explicitly calls out that file-size growth is a first-class maintainability signal.
- Future work (more widgets, better layouts, SaaS web evolution) will make the problem worse.
- Current code mixes ncurses-specific concerns with shared snapshot rendering logic.

A dedicated skill ensures this work is done with the same rigor as engine decomposition rather than ad-hoc cleanup.

---

## Scope of the Refactor (Defined by the Review)

Target files:
- `src/ui/tabbed_dashboard.cpp`
- `src/ui/console_dashboard.cpp`
- Related headers in `src/ui/`

Goals:
- Bring both primary files comfortably under 1k LOC.
- Extract reusable widget / component abstractions (ASCII widgets already exist — leverage them).
- Keep all snapshot consumption going through the existing `snapshot_dashboard()` seam.
- No changes to hot paths (UI is already off-hot-path).
- Preserve identical observable behavior for operators.

---

## Non-Negotiable Invariants

1. **Snapshot seam is sacred** — all data still comes from `engine.snapshot_dashboard()` or equivalent. No direct engine poking from UI code.
2. **Zero hot-path impact** — UI code must never introduce allocations or work that could leak into timing-sensitive paths (even indirectly via shared headers).
3. **Read-only surfaces only** — current web UI and TUI must remain strictly read-only. This refactor must not add any control paths.
4. **ncurses vs web sharing** — maximize shared pure rendering / formatting logic (see existing `ascii_widgets.*`).
5. **No new dependencies** on heavy UI frameworks in the C++ layer.
6. After refactor, running both ncurses TUI and `--web` must produce equivalent data presentation.

---

## Detailed Workflow

### Phase 0 — Read-Only Analysis
- Read both large dashboard files completely.
- Read `src/analytics/ascii_widgets.*` and other ui helpers.
- Map all widget types currently implemented inline.
- Identify duplication between tabbed and console dashboards.
- Read the web UI side (`src/web/`, frontend) to understand the shared snapshot contract.
- Produce a component inventory + duplication report.
- Re-read ` (retired one-time review docs) 05-ui-large-files.md`.

### Phase 1 — Design
- Use the `design` skill.
- Propose extraction candidates:
  - Position / account panel
  - Risk gauges
  - Order blotter
  - L2 ladder renderer
  - Fills tape
  - Strategy cards
  - Common layout / tab management
- Decide on file organization under `src/ui/`.
- Define clear ownership (who owns ncurses drawing vs pure data formatting).

### Phase 2 — Extraction
- Extract pure data formatting / string building first (highest reuse).
- Extract widget components.
- Refactor one dashboard at a time (start with the one that yields biggest net reduction).
- After each significant extraction:
  - Full build
  - Manual smoke of both TUI modes + web UI
  - Relevant UI + analytics tests
  - `quality` skill review

### Phase 3 — Verification Ritual
- Full test suite.
- `check-work` with focus on "UI structure".
- `quality` skill (file size rules, maintainability).
- Re-run the original `/code-review` skill and confirm the two files are no longer flagged.
- Side-by-side comparison of rendered output (TUI + web) before vs after.
- Update any screenshots or docs in `docs/reference/05-web-ui.md` if behavior description changes.
- Document new component boundaries in architecture or reference docs.

### Phase 4 — Hygiene
- Add a soft size guard comment or CMake note for the remaining dashboard files.
- Close the item in ` (retired one-time review docs) 05-ui-large-files.md`.
- Consider adding a "ui size" check to a future `repo-doctor` skill.

---

## Integration With Existing Skills

- Works alongside `quality` (the review explicitly says to update quality skill rules for size).
- Should be cross-reviewed by `performance` (even though UI is cold) to catch accidental hot-path bleed.
- Leverages patterns from `engine-decomposition` (read-only first, net reduction, subagents, full ritual).
- Future web work can use the extracted components as a model.

---

## Success Criteria

- Both `tabbed_dashboard.cpp` and `console_dashboard.cpp` are **well under 1000 LOC**.
- Clear, focused component files exist with single responsibilities.
- No observable change in what operators see in TUI or web UI.
- A new developer can add a new panel/widget by extending a small component instead of editing a 1000+ LOC file.
- `/code-review` no longer flags the UI layer for size.

---

## References

- ` (retired one-time review docs) 05-ui-large-files.md` (primary)
- `src/ui/tabbed_dashboard.cpp`
- `src/ui/console_dashboard.cpp`
- `src/analytics/ascii_widgets.h` + `.cpp`
- `docs/reference/05-web-ui.md`
- `src/web/snapshot_json.cpp` (shared data contract)
- `~/.grok/skills/quality/SKILL.md`

---

## SaaS Future Notes

The web UI is the most likely surface to grow dramatically in a SaaS product.

This refactor is **foundational** for:
- Reusing components between the internal TUI and future multi-user web frontend.
- Making it possible to later split "engine snapshot consumer" from "presentation".
- Allowing the frontend team (React) to have a stable, well-documented set of data shapes.

When SaaS work begins, a follow-up skill (`web-component-system` or similar) should build on the boundaries established here.

---

*Use this document as the authoritative spec when scaffolding the skill.*
