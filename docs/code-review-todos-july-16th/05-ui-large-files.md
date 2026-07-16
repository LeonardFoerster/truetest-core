# Issue 5 — Oversized UI Dashboard Files

**Severity:** nit (but part of broader maintainability culture)  
**Date:** 2026-07-16 (from `/code-review`)  
**Files:**
- `src/ui/tabbed_dashboard.cpp` — 1215 lines
- `src/ui/console_dashboard.cpp` — 1097 lines

## Context from Review
While the reviewed changes did not touch the UI layer, the `/code-review` skill explicitly calls out file size as a first-class signal:

> "Do not let a PR push a file from under 1k lines to over 1k lines without a very strong reason."
> "Treat this as a strong code-quality smell by default."
> "Prefer extracting helpers, subcomponents, modules, or local abstractions instead of letting a file sprawl past 1000 lines."

These two files are already over the line and are the largest non-engine, non-dep source files in the project (after excluding build artifacts).

## Why This Matters
- The UI is complex (multiple panels, input handling, overlays, toasts, themes, snapshots, rendering loops).
- Large files reduce scanability and increase the chance of tangled state + drawing code.
- Future features will make them worse unless a boundary is drawn now.
- Consistent application of the 1k rule across the whole codebase (engine is the extreme case; these are the next).

## Step-by-Step Guide for Grok Build

### Phase 0: Exploration
1. Read both files (in sections — they are large).
   - Focus on class structure, `render_loop`, `draw_*` methods, panel handling, input, state.
2. List all included headers and understand dependencies on `dashboard_snapshot.h`, panels, etc.
3. Grep inside the files for:
   - Large functions
   - Repeated drawing patterns
   - State management (`prefs_state`, toasts, confirm overlays, etc.)
   - Switch statements on themes or panel types
4. Look at the panel files under `src/ui/panels/` (there are several: overview, orders, positions, risk, debug, etc.). Understand the current extraction level.
5. Read `src/ui/dashboard_snapshot.h` and any shared ui headers.
6. Check tests that may exercise the UI (`tests/test_cli.cpp`, `tests/test_ui.h`?).
7. Run the app (if possible in the environment) or build the UI targets to understand runtime structure.

### Phase 1: Identify Extraction Opportunities
Common good cuts for dashboard-style code:
- Separate **rendering / drawing** concerns from **state & input handling**.
- Extract **overlay / modal** logic (confirm, help, halt banner, toast) into their own small classes or free functions with clear ownership.
- Extract **preference / config persistence** (`load_prefs`, `save_prefs`).
- Panel orchestration vs individual panel drawing (many panels already exist; improve the contract).
- Theme / chrome drawing helpers.
- Input handling state machine (if it is ad-hoc).

Target: bring both files comfortably under 900-950 lines, with new focused files <400 lines each.

### Phase 2: Resolution Steps
1. **Do not start editing the giant files first.** Create new small focused files / classes.
2. Introduce clear types for the pieces you will extract (e.g. `ToastManager`, `OverlayRenderer`, `DashboardChrome`, `PrefsIO`).
3. Move one coherent piece at a time (e.g. all toast drawing + logic first).
4. Update includes and call sites.
5. For tabbed vs console: look for shared code that can live in a base or utility layer so the two dashboards don't duplicate logic.
6. After each extraction:
   - Compile.
   - Run any UI / CLI tests.
   - Manually inspect the line counts of the original files.
7. Clean up any now-smaller functions that can be further simplified.
8. Add comments or `// TODO(size)` markers only if truly temporary.

### Phase 3: Verification
- `wc -l src/ui/tabbed_dashboard.cpp` and console version both report < 1000 (ideally << 1000).
- No loss of functionality (run the binaries if feasible, or at least compile + unit tests).
- The new extracted modules have single responsibility and good names.
- Re-run `/code-review` skill — these files should no longer be highlighted.
- Consider adding a simple size guard in CI or a pre-commit / script (consistent with other checks in `scripts/`).

### Phase 4: Prevention
- Add a note in the `quality` skill and/or a new or existing size-related rule.
- When future UI work is done, the reviewer (or `/code-review`) must call out any file approaching 800 lines.
- Update `docs/reference/05-web-ui.md` or any UI docs if the structure changes meaningfully (note: these are terminal dashboards, separate from the web UI).

## Success Criteria
- Both dashboard implementation files are well under the 1k line threshold.
- Code is more modular and easier for a new reader to navigate.
- Extraction does not introduce unnecessary indirection or allocation in rendering hot paths (keep it simple / boring as the skill prefers).
- Future dashboard growth is naturally directed into new focused components.

## References
- `01-engine-god-class.md` (for the overarching 1k-line philosophy)
- `src/ui/panels/` directory
- `src/ui/dashboard_snapshot.h`
- `~/.grok/skills/quality/SKILL.md`

**Status:** Planning artifact. Lower priority than Issues 1-4 but should be addressed as part of general codebase hygiene.

---
*Generated from `/code-review` on 2026-07-16.*
