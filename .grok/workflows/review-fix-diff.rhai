// review-fix-diff — review the current working-tree git diff, adversarially
// verify findings, fix confirmed issues, then run testing + independent check-work.
//
// Args (all optional):
//   base   — git rev for the diff base (default: HEAD). Working-tree changes vs this.
//   focus  — extra focus area string for reviewers / check-work
//   path   — optional path prefix filter (e.g. "src/engine/")
//   max_fix — max confirmed issues to attempt fixing (default 6, max 10)
//
// Overall PASS requires:
//   TESTING PASS + CHECK-WORK PASS after resolve phase
//   No remaining critical confirmed issues left unfixed

let meta = #{
    name: "review-fix-diff",
    description: "Review working-tree diff, verify findings, fix issues, re-test and check-work",
    when_to_use: "After a batch of uncommitted changes; review + fix + gate before commit",
    phases: [
        #{ title: "Scope", detail: "inventory working tree vs base (incl. untracked)" },
        #{ title: "Review", detail: "parallel correctness / hot-path / safety / tests" },
        #{ title: "Verify", detail: "adversarial verify each finding" },
        #{ title: "Resolve", detail: "fix confirmed real issues in the workspace" },
        #{ title: "Testing", detail: "build + full suite + gate scripts" },
        #{ title: "Check-Work", detail: "independent correctness verifier" },
        #{ title: "Verdict", detail: "synthesize overall report" },
    ],
};

// --- schemas ----------------------------------------------------------------

let scope_schema = #{
    "type": "object",
    "required": ["changed_files", "untracked_files", "hot_path", "safety_surface", "freeze_files", "themes", "summary"],
    "properties": #{
        "changed_files": #{ "type": "array", "items": #{ "type": "string" }, "maxItems": 80 },
        "untracked_files": #{ "type": "array", "items": #{ "type": "string" }, "maxItems": 40 },
        "hot_path": #{ "type": "boolean" },
        "safety_surface": #{ "type": "boolean" },
        "freeze_files": #{ "type": "array", "items": #{ "type": "string" }, "maxItems": 20 },
        "themes": #{ "type": "array", "items": #{ "type": "string" }, "maxItems": 12 },
        "summary": #{ "type": "string" },
        "risk_notes": #{ "type": "string" },
    },
};

let findings_schema = #{
    "type": "object",
    "required": ["findings", "dimension", "summary"],
    "properties": #{
        "dimension": #{ "type": "string" },
        "summary": #{ "type": "string" },
        "findings": #{
            "type": "array",
            "maxItems": 8,
            "items": #{
                "type": "object",
                "required": ["id", "file", "severity", "issue", "fix_hint"],
                "properties": #{
                    "id": #{ "type": "string" },
                    "file": #{ "type": "string" },
                    "severity": #{ "type": "string" },
                    "issue": #{ "type": "string" },
                    "fix_hint": #{ "type": "string" },
                    "line_hint": #{ "type": "string" },
                },
            },
        },
    },
};

let verdict_schema = #{
    "type": "object",
    "required": ["real", "reason", "evidence", "severity"],
    "properties": #{
        "real": #{ "type": "boolean" },
        "reason": #{ "type": "string" },
        "evidence": #{ "type": "string" },
        "severity": #{ "type": "string" },
        "safe_to_auto_fix": #{ "type": "boolean" },
    },
};

let fix_schema = #{
    "type": "object",
    "required": ["fixed", "files_touched", "summary", "residual"],
    "properties": #{
        "fixed": #{ "type": "boolean" },
        "files_touched": #{ "type": "array", "items": #{ "type": "string" }, "maxItems": 12 },
        "summary": #{ "type": "string" },
        "residual": #{ "type": "string" },
        "commands_run": #{ "type": "array", "items": #{ "type": "string" }, "maxItems": 12 },
    },
};

let testing_schema = #{
    "type": "object",
    "required": ["verdict", "suite_green", "gates_green", "diff_analyzed", "summary", "blockers"],
    "properties": #{
        "verdict": #{ "type": "string" },
        "suite_green": #{ "type": "boolean" },
        "gates_green": #{ "type": "boolean" },
        "diff_analyzed": #{ "type": "boolean" },
        "summary": #{ "type": "string" },
        "blockers": #{ "type": "array", "items": #{ "type": "string" }, "maxItems": 16 },
        "commands_run": #{ "type": "array", "items": #{ "type": "string" }, "maxItems": 24 },
    },
};

let checkwork_schema = #{
    "type": "object",
    "required": ["verdict", "summary", "issues"],
    "properties": #{
        "verdict": #{ "type": "string" },
        "summary": #{ "type": "string" },
        "correctness": #{ "type": "string" },
        "adequacy": #{ "type": "string" },
        "quality": #{ "type": "string" },
        "issues": #{ "type": "array", "items": #{ "type": "string" }, "maxItems": 16 },
    },
};

// --- args -------------------------------------------------------------------

let base = "HEAD";
let focus = "";
let path_filter = "";
let max_fix = 6;

if args != () {
    if args.base != () && type_of(args.base) == "string" && args.base != "" {
        base = args.base;
    }
    if args.focus != () && type_of(args.focus) == "string" {
        focus = args.focus;
    }
    if args.path != () && type_of(args.path) == "string" {
        path_filter = args.path;
    }
    if args.max_fix != () {
        // accept number-like; clamp later via comparison
        if type_of(args.max_fix) == "i64" || type_of(args.max_fix) == "int" {
            max_fix = args.max_fix;
        }
    }
}
if max_fix < 1 { max_fix = 1; }
if max_fix > 10 { max_fix = 10; }

log("review-fix-diff base=" + base
    + " path_filter=" + (if path_filter == "" { "(none)" } else { path_filter })
    + " max_fix=" + max_fix.to_string()
    + " focus=" + (if focus == "" { "(none)" } else { focus }));

// --- Phase 1: Scope ---------------------------------------------------------

phase("Scope");

let scope_prompt = "";
scope_prompt += "You are the SCOPE classifier for TrueTest working-tree review-fix.\n";
scope_prompt += "READ-ONLY. Use git, read_file, grep, list_dir. Do not edit files.\n\n";
scope_prompt += "Workspace is the TrueTest core/ C++ engine repo (cwd is typically core/).\n";
scope_prompt += "Diff base: " + base + "\n";
if path_filter != "" {
    scope_prompt += "Path filter (prefer files under this prefix): " + path_filter + "\n";
}
if focus != "" {
    scope_prompt += "User focus: " + focus + "\n";
}
scope_prompt += "\nMANDATORY steps (run every command):\n";
scope_prompt += "1. git rev-parse --verify " + base + "\n";
scope_prompt += "2. git status -sb\n";
scope_prompt += "3. git diff --name-status " + base + "\n";
scope_prompt += "4. git diff --stat " + base + "\n";
scope_prompt += "5. git diff --cached --name-status\n";
scope_prompt += "6. List untracked source/test files: git ls-files --others --exclude-standard\n";
scope_prompt += "7. Read enough of the actual diffs AND untracked file contents to classify risk.\n";
scope_prompt += "   Untracked files are part of the change set — do not ignore them.\n\n";
scope_prompt += "Classify:\n";
scope_prompt += "- hot_path=true if any change touches engine event loop, orderbook, strategy on_*, SPSC rings, pools, pre-trade risk on event, provider parse/dispatch hot path, or hotpath tests.\n";
scope_prompt += "- safety_surface=true if freeze list, halt/kill/DMS/reconciler/watchdog, TT_TARGET, live order permission, or fail-closed risk paths.\n";
scope_prompt += "  Freeze files: src/core/tt_target.h, src/engine/engine.cpp, binance futures provider/DMS/kill/reconciler, risk_manager, futures_risk_check, live_safety.h, worker_watchdog.h\n";
scope_prompt += "- themes: short theme strings (e.g. hybrid-paper-adapter, MC seeding, strategy exits, portfolio PnL).\n";
scope_prompt += "- freeze_files: only freeze-surface paths actually touched.\n\n";
scope_prompt += "Return structured output only. summary = 3-6 sentences covering what the change set is trying to do.\n";
scope_prompt += "Do NOT invent files — only paths from git status/diff/ls-files.\n";

let scope_r = agent(scope_prompt, #{
    label: "scope",
    capability_mode: "read-only",
    output_schema: scope_schema,
});

let changed_files = [];
let untracked_files = [];
let hot_path = false;
let safety_surface = false;
let freeze_files = [];
let themes = [];
let scope_summary = "Scope agent failed or returned unusable output.";
let risk_notes = "";

if scope_r != () && scope_r.success && scope_r.output != () {
    if scope_r.output.changed_files != () { changed_files = scope_r.output.changed_files; }
    if scope_r.output.untracked_files != () { untracked_files = scope_r.output.untracked_files; }
    if scope_r.output.hot_path == true { hot_path = true; }
    if scope_r.output.safety_surface == true { safety_surface = true; }
    if scope_r.output.freeze_files != () { freeze_files = scope_r.output.freeze_files; }
    if scope_r.output.themes != () { themes = scope_r.output.themes; }
    if scope_r.output.summary != () { scope_summary = scope_r.output.summary; }
    if scope_r.output.risk_notes != () { risk_notes = scope_r.output.risk_notes; }
} else {
    log("WARNING: scope agent failed — fail-closed defaults hot_path+safety_surface.");
    hot_path = true;
    safety_surface = true;
}

let files_blob = json_encode(changed_files);
let untracked_blob = json_encode(untracked_files);
let freeze_blob = json_encode(freeze_files);
let themes_blob = json_encode(themes);
log("scope: changed=" + changed_files.len().to_string()
    + " untracked=" + untracked_files.len().to_string()
    + " hot_path=" + hot_path.to_string()
    + " safety_surface=" + safety_surface.to_string()
    + " freeze=" + freeze_files.len().to_string());

// --- Phase 2: Multi-dimension review ----------------------------------------

phase("Review");

let review_common = "";
review_common += "You are a STRICT code reviewer for TrueTest core (C++23 trading engine).\n";
review_common += "READ-ONLY. You MUST use tools (git diff, read_file, grep) — never answer from memory.\n";
review_common += "An empty findings list is valid ONLY after you have read the actual diffs and untracked sources.\n\n";
review_common += "Diff base: " + base + "\n";
review_common += "Scope summary: " + scope_summary + "\n";
review_common += "Themes: " + themes_blob + "\n";
review_common += "Changed files (JSON): " + files_blob + "\n";
review_common += "Untracked files (JSON): " + untracked_blob + "\n";
review_common += "hot_path=" + hot_path.to_string() + " safety_surface=" + safety_surface.to_string() + "\n";
if path_filter != "" {
    review_common += "Path filter: " + path_filter + "\n";
}
if focus != "" {
    review_common += "User focus: " + focus + "\n";
}
review_common += "\nInventory commands (run them):\n";
review_common += "  git status -sb\n";
review_common += "  git diff " + base + " -- <relevant paths>\n";
review_common += "  git ls-files --others --exclude-standard\n";
review_common += "  read_file on untracked sources/tests that matter for your dimension\n\n";
review_common += "Finding rules:\n";
review_common += "- severity must be one of: critical, high, medium, low\n";
review_common += "- Only report REAL defects: bugs, invariant breaks, missing tests for new behaviour, AGENTS.md red-line violations, broken determinism, fail-open safety, hot-path allocs, layer violations, compile/link risks\n";
review_common += "- Do NOT report style nits, speculative future work, or praise\n";
review_common += "- id: short stable slug like corr-1, perf-2\n";
review_common += "- fix_hint: concrete 1-3 sentence how to fix\n";
review_common += "- file: primary path; line_hint optional (e.g. function name or nearby symbol)\n";
review_common += "- Max 8 findings; prefer highest severity first\n";

let dims = [
    #{
        key: "correctness",
        label: "review:correctness",
        extra: "DIMENSION: correctness & determinism bugs.\nLook for: wrong PnL/fill logic, off-by-one, uninit state, broken MC seeding, strategy signal errors, date parse edge cases, missing null checks, API contract breaks, untracked hybrid adapter miswiring, portfolio accounting errors, silent data loss.\n",
    },
    #{
        key: "hotpath",
        label: "review:hotpath",
        extra: "DIMENSION: hot-path performance (AGENTS.md R1-R10).\nLook for: heap new/make_shared on event path, string/vector growth, nlohmann::json on hot path, locks/syscalls in engine loop, second SPSC producer, pool runtime grow, virtual dispatch on tight loops without measurement, heavy logging/fmt on event path.\nCold path (startup, workers, TUI, MC setup between trials) may allocate — do not false-positive cold code.\n",
    },
    #{
        key: "safety",
        label: "review:safety",
        extra: "DIMENSION: live-safety & fail-closed (AGENTS.md S1-S10).\nLook for: live order path weakened, halt non-terminal/auto-clear, reconciler accept-when-should-refuse, TT_TARGET bypass, retries on kill/DMS/watchdog, freeze-file behaviour changes without CCB awareness, fail-open risk checks.\nengine.cpp is on the freeze list — scrutinize any behaviour change there carefully.\n",
    },
    #{
        key: "tests",
        label: "review:tests",
        extra: "DIMENSION: test adequacy & build registration.\nLook for: new behaviour without tests, untracked tests not in cmake/Sources.cmake, flaky timing assumptions, missing defect-closure coverage for claimed fixes, Sources.cmake omissions for new headers/cpp, tests that cannot fail (always-true asserts).\n",
    },
];

let review_jobs = [];
for d in dims {
    let p = "";
    p += review_common;
    p += d.extra;
    p += "\nSet dimension field to \"" + d.key + "\".\n";
    p += "summary: 2-4 sentences on what you inspected and residual risk.\n";
    review_jobs.push(#{
        prompt: p,
        label: d.label,
        capability_mode: "read-only",
        output_schema: findings_schema,
    });
}

let review_results = parallel(review_jobs);

let all_findings = [];
let review_summaries = [];
let ri = 0;
for r in review_results {
    let dim_name = dims[ri].key;
    if r != () && r.success && r.output != () {
        if r.output.summary != () {
            review_summaries.push(dim_name + ": " + r.output.summary);
        }
        if r.output.findings != () {
            for f in r.output.findings {
                // attach dimension if missing
                let item = f;
                all_findings.push(item);
            }
        }
    } else {
        review_summaries.push(dim_name + ": reviewer failed (no findings from this lens)");
        log("WARNING: review agent failed for " + dim_name);
    }
    ri += 1;
}

// Cap total findings before verify to control fan-out (prefer critical/high by order reviewers gave)
const MAX_VERIFY = 16;
if all_findings.len() > MAX_VERIFY {
    log("truncating findings from " + all_findings.len().to_string() + " to " + MAX_VERIFY.to_string());
    let truncated = [];
    let ti = 0;
    for f in all_findings {
        if ti < MAX_VERIFY {
            truncated.push(f);
        }
        ti += 1;
    }
    all_findings = truncated;
}

log("review: raw_findings_for_verify=" + all_findings.len().to_string());

// --- Phase 3: Adversarial verify --------------------------------------------

phase("Verify");

let confirmed = [];
let rejected = [];

if all_findings.len() == 0 {
    log("No review findings — skipping verify panel.");
} else {
    let vjobs = [];
    let fi = 0;
    for f in all_findings {
        let f_file = if f.file != () { f.file } else { "(unknown)" };
        let f_issue = if f.issue != () { f.issue } else { "(no issue text)" };
        let f_sev = if f.severity != () { f.severity } else { "medium" };
        let f_id = if f.id != () { f.id } else { "f" + fi.to_string() };
        let f_hint = if f.fix_hint != () { f.fix_hint } else { "" };
        let f_line = if f.line_hint != () { f.line_hint } else { "" };

        let vp = "";
        vp += "You are an ADVERSARIAL verifier. Your job is to REFUTE false positives.\n";
        vp += "READ-ONLY. Independently inspect the code with git diff / read_file / grep.\n";
        vp += "Default real=false unless you find concrete evidence the defect exists in the CURRENT working tree.\n\n";
        vp += "Claim id: " + f_id + "\n";
        vp += "File: " + f_file + "\n";
        vp += "Claimed severity: " + f_sev + "\n";
        vp += "Issue: " + f_issue + "\n";
        vp += "Suggested fix: " + f_hint + "\n";
        vp += "Line hint: " + f_line + "\n";
        vp += "Diff base: " + base + "\n\n";
        vp += "Steps:\n";
        vp += "1. Read the file and the relevant git diff " + base + " -- " + f_file + "\n";
        vp += "2. If untracked, read the full file.\n";
        vp += "3. Check surrounding call sites if needed.\n";
        vp += "4. Set real=true ONLY with evidence string citing what you saw (symbol/snippet/behaviour).\n";
        vp += "5. severity: re-rate critical|high|medium|low if real; else keep claimed or low.\n";
        vp += "6. safe_to_auto_fix=true only if a small local fix is clear and does not require product design choices or live-capital policy changes.\n";
        vp += "   For freeze-surface behaviour changes (engine.cpp live paths), prefer safe_to_auto_fix=false unless the fix is an obvious bug regression introduced by this diff.\n";

        vjobs.push(#{
            prompt: vp,
            label: "verify:" + f_id,
            capability_mode: "read-only",
            output_schema: verdict_schema,
        });
        fi += 1;
    }

    let verdicts = parallel(vjobs);
    let vi = 0;
    for v in verdicts {
        let f = all_findings[vi];
        let f_id = if f.id != () { f.id } else { "f" + vi.to_string() };
        if v != () && v.success && v.output != () && v.output.real == true
            && v.output.evidence != () && v.output.evidence != "" {
            let entry = #{
                id: f_id,
                file: if f.file != () { f.file } else { "" },
                issue: if f.issue != () { f.issue } else { "" },
                fix_hint: if f.fix_hint != () { f.fix_hint } else { "" },
                line_hint: if f.line_hint != () { f.line_hint } else { "" },
                severity: if v.output.severity != () { v.output.severity } else { if f.severity != () { f.severity } else { "medium" } },
                evidence: v.output.evidence,
                reason: if v.output.reason != () { v.output.reason } else { "" },
                safe_to_auto_fix: v.output.safe_to_auto_fix == true,
            };
            confirmed.push(entry);
        } else {
            rejected.push(f_id);
        }
        vi += 1;
    }
}

log("verify: confirmed=" + confirmed.len().to_string()
    + " rejected_or_unverified=" + rejected.len().to_string());

// Select fix targets: prefer critical/high, respect max_fix, require safe_to_auto_fix OR high/critical
let to_fix = [];
let deferred = [];
for c in confirmed {
    let sev = if c.severity != () { c.severity } else { "medium" };
    let is_high = sev == "critical" || sev == "high" || sev == "CRITICAL" || sev == "HIGH";
    let auto_ok = c.safe_to_auto_fix == true || is_high;
    if auto_ok && to_fix.len() < max_fix {
        to_fix.push(c);
    } else {
        deferred.push(c);
    }
}
if confirmed.len() > to_fix.len() {
    log("fix queue capped/deferred: fixing " + to_fix.len().to_string()
        + " of " + confirmed.len().to_string() + " confirmed");
}

// --- Phase 4: Resolve -------------------------------------------------------

phase("Resolve");

let fix_results_summary = [];
let fixed_count = 0;
let fix_failed_count = 0;

if to_fix.len() == 0 {
    log("No auto-fix targets — resolve phase is a no-op.");
} else {
    // Sequential-ish via parallel of independent file groups would be ideal;
    // use parallel agents but each owns one issue; instruct not to thrash shared files.
    // If two issues share a file, batch them into one agent.
    let fix_groups = []; // array of #{ file, issues: [] }
    for c in to_fix {
        let cf = if c.file != () { c.file } else { "unknown" };
        let found = false;
        let gi = 0;
        for g in fix_groups {
            if g.file == cf {
                // cannot mutate nested easily in all dialects — rebuild
                found = true;
            }
            gi += 1;
        }
        // simpler: one agent per issue (max_fix already small); warn about shared files
        fix_groups.push(#{ file: cf, issue: c });
    }

    let fjobs = [];
    let fji = 0;
    for g in fix_groups {
        let c = g.issue;
        let c_id = if c.id != () { c.id } else { "fix" + fji.to_string() };
        let c_file = if c.file != () { c.file } else { "" };
        let c_issue = if c.issue != () { c.issue } else { "" };
        let c_hint = if c.fix_hint != () { c.fix_hint } else { "" };
        let c_ev = if c.evidence != () { c.evidence } else { "" };
        let c_sev = if c.severity != () { c.severity } else { "medium" };

        let fp = "";
        fp += "You are a FIX agent for TrueTest core. Apply a MINIMAL correct fix for ONE confirmed issue.\n";
        fp += "capability: all (edit + build/test allowed). Follow AGENTS.md hot-path and safety red lines.\n\n";
        fp += "Issue id: " + c_id + "\n";
        fp += "Severity: " + c_sev + "\n";
        fp += "File: " + c_file + "\n";
        fp += "Issue: " + c_issue + "\n";
        fp += "Evidence from verifier: " + c_ev + "\n";
        fp += "Suggested fix: " + c_hint + "\n";
        fp += "Diff base was: " + base + "\n\n";
        fp += "Rules:\n";
        fp += "1. Read the current file(s) first. Confirm the bug still exists before editing.\n";
        fp += "2. Smallest correct change only — no drive-by refactors, no formatting sweeps.\n";
        fp += "3. If cmake/Sources.cmake needs a new source registered, do that.\n";
        fp += "4. If a test is required for the fix, add/update a focused test.\n";
        fp += "5. Hot path: zero heap; use pools; no JSON; no runtime pool grow.\n";
        fp += "6. Safety: halt terminal; no live-order bypass; no fail-open.\n";
        fp += "7. engine.cpp is freeze-surface — only change it if this issue requires it; keep change surgical.\n";
        fp += "8. After edit, try a focused compile if feasible (cmake --build --preset linux-tests -j --target truetest_tests) or at least note residual.\n";
        fp += "9. If you cannot safely fix (ambiguous product intent, needs human CCB), set fixed=false and residual explaining why.\n";
        fp += "10. Do NOT commit or push.\n\n";
        fp += "Return fixed, files_touched, summary, residual, commands_run.\n";

        fjobs.push(#{
            prompt: fp,
            label: "fix:" + c_id,
            capability_mode: "all",
            output_schema: fix_schema,
        });
        fji += 1;
    }

    let fix_outs = parallel(fjobs);
    let foi = 0;
    for fo in fix_outs {
        let c = to_fix[foi];
        let c_id = if c.id != () { c.id } else { "fix" + foi.to_string() };
        if fo != () && fo.success && fo.output != () {
            let was_fixed = fo.output.fixed == true;
            if was_fixed { fixed_count += 1; } else { fix_failed_count += 1; }
            let sm = if fo.output.summary != () { fo.output.summary } else { "(no summary)" };
            let rs = if fo.output.residual != () { fo.output.residual } else { "" };
            fix_results_summary.push(c_id + ": fixed=" + was_fixed.to_string() + " — " + sm
                + (if rs != "" { " residual=" + rs } else { "" }));
        } else {
            fix_failed_count += 1;
            fix_results_summary.push(c_id + ": fix agent failed");
        }
        foi += 1;
    }
}

log("resolve: fixed=" + fixed_count.to_string() + " failed=" + fix_failed_count.to_string());

// --- Phase 5: Testing -------------------------------------------------------

phase("Testing");

let testing_prompt = "";
testing_prompt += "You are the TESTING hard gate for TrueTest core after review-fix.\n";
testing_prompt += "You MUST actually run builds, tests, and gate scripts — do not claim pass from memory.\n";
testing_prompt += "capability: execute. Prefer report FAIL with blockers over silent greenwash. Only apply a trivial one-line fix if a test cannot even compile due to an obvious typo you introduced context; otherwise do not edit.\n\n";
testing_prompt += "Diff base: " + base + "\n";
testing_prompt += "Scope: " + scope_summary + "\n";
testing_prompt += "Changed files: " + files_blob + "\n";
testing_prompt += "Untracked: " + untracked_blob + "\n";
testing_prompt += "Fixes applied this run: " + fixed_count.to_string() + " fixed, " + fix_failed_count.to_string() + " failed\n";
testing_prompt += "Fix notes (JSON-ish text):\n";
for fr in fix_results_summary {
    testing_prompt += "  - " + fr + "\n";
}
testing_prompt += "\nMANDATORY protocol (cwd = core/ workspace):\n";
testing_prompt += "1) git status -sb; git diff --stat " + base + "; git ls-files --others --exclude-standard\n";
testing_prompt += "2) Build tests:\n";
testing_prompt += "   cmake --preset linux-tests\n";
testing_prompt += "   cmake --build --preset linux-tests -j\n";
testing_prompt += "   Fallback: cmake -B build -DBUILD_TESTS=ON && cmake --build build -j\n";
testing_prompt += "3) Full suite:\n";
testing_prompt += "   ctest --test-dir out/build/linux-tests --output-on-failure\n";
testing_prompt += "   or ctest --test-dir build --output-on-failure\n";
testing_prompt += "4) Change-aware extras:\n";
testing_prompt += "   ctest --test-dir <dir> -R 'hotpath|Hotpath|Backtest|Defect|Portfolio|MonteCarlo|Strategy' --output-on-failure\n";
testing_prompt += "   Ensure new tests (e.g. test_backtest_defect_closure) are registered and run.\n";
testing_prompt += "5) Gate scripts:\n";
testing_prompt += "   ./scripts/check-hotpath-json.sh\n";
testing_prompt += "   ./scripts/check-layer-deps.sh\n";
testing_prompt += "   ./scripts/check-live-safety-freeze.sh\n\n";
testing_prompt += "VERDICT: exactly PASS or FAIL.\n";
testing_prompt += "PASS only if suite_green, gates_green, diff_analyzed all true.\n";
testing_prompt += "blockers: concrete command+error strings on FAIL.\n";
testing_prompt += "commands_run: important commands executed.\n";

let testing_r = agent(testing_prompt, #{
    label: "testing",
    capability_mode: "execute",
    output_schema: testing_schema,
});

let testing_verdict = "FAIL";
let testing_summary = "Testing agent failed or returned unusable output (fail closed).";
let testing_blockers = ["testing agent did not return a usable structured result"];
let suite_green = false;
let gates_green = false;

if testing_r != () && testing_r.success && testing_r.output != () {
    if testing_r.output.verdict != () && type_of(testing_r.output.verdict) == "string" {
        let tv = testing_r.output.verdict;
        if tv == "PASS" || tv == "pass" || tv == "Pass" || tv == "TESTING VERDICT: PASS" {
            testing_verdict = "PASS";
        } else {
            testing_verdict = "FAIL";
        }
    }
    if testing_r.output.summary != () { testing_summary = testing_r.output.summary; }
    if testing_r.output.blockers != () { testing_blockers = testing_r.output.blockers; }
    if testing_r.output.suite_green == true { suite_green = true; }
    if testing_r.output.gates_green == true { gates_green = true; }
    if testing_verdict == "PASS" && (!suite_green || !gates_green) {
        testing_verdict = "FAIL";
        testing_blockers.push("script override: PASS claimed without suite_green and gates_green both true");
    }
}

log("TESTING VERDICT: " + testing_verdict);

// --- Phase 6: Check-work ----------------------------------------------------

phase("Check-Work");

let cw_prompt = "";
cw_prompt += "You are an INDEPENDENT verifier (/check-work). You did NOT implement the original change or the fixes.\n";
cw_prompt += "Re-read code and evidence. Do not rubber-stamp prior agents.\n\n";
cw_prompt += "Diff base: " + base + "\n";
if focus != "" { cw_prompt += "User focus: " + focus + "\n"; }
cw_prompt += "Scope: " + scope_summary + "\n";
cw_prompt += "Risk notes: " + risk_notes + "\n";
cw_prompt += "Themes: " + themes_blob + "\n";
cw_prompt += "Changed: " + files_blob + "\n";
cw_prompt += "Untracked: " + untracked_blob + "\n";
cw_prompt += "Freeze files: " + freeze_blob + "\n";
cw_prompt += "Confirmed issues found: " + confirmed.len().to_string() + "\n";
cw_prompt += "Fixed this run: " + fixed_count.to_string() + "  fix-failed: " + fix_failed_count.to_string() + "\n";
cw_prompt += "Deferred confirmed issues: " + deferred.len().to_string() + "\n";
cw_prompt += "TESTING: " + testing_verdict + " — " + testing_summary + "\n\n";
cw_prompt += "Confirmed issue list for residual check:\n";
for c in confirmed {
    let cid = if c.id != () { c.id } else { "?" };
    let cfile = if c.file != () { c.file } else { "?" };
    let ciss = if c.issue != () { c.issue } else { "?" };
    cw_prompt += "- [" + cid + "] " + cfile + ": " + ciss + "\n";
}
cw_prompt += "\nFix notes:\n";
for fr in fix_results_summary {
    cw_prompt += "- " + fr + "\n";
}
cw_prompt += "\nProtocol:\n";
cw_prompt += "1) git status; git diff " + base + "; read untracked key files.\n";
cw_prompt += "2) For each confirmed issue: is it fixed, still present, or wrongly 'fixed'?\n";
cw_prompt += "3) Evaluate correctness, adequacy, quality vs AGENTS.md (hot-path, safety, layers, determinism).\n";
cw_prompt += "4) Spot-check that new sources are in cmake/Sources.cmake and tests exist for claimed defect closures.\n";
cw_prompt += "5) If TESTING is FAIL, check-work must FAIL unless you prove suite is green now.\n";
cw_prompt += "6) Remaining critical/high unfixed issues => FAIL.\n\n";
cw_prompt += "verdict: PASS or FAIL only. issues: concrete blockers (empty only if PASS).\n";

let cw_r = agent(cw_prompt, #{
    label: "check-work",
    capability_mode: "execute",
    output_schema: checkwork_schema,
});

let cw_verdict = "FAIL";
let cw_summary = "Check-work agent failed or returned unusable output (fail closed).";
let cw_issues = ["check-work agent did not return a usable structured result"];
let cw_correctness = "";
let cw_adequacy = "";
let cw_quality = "";

if cw_r != () && cw_r.success && cw_r.output != () {
    if cw_r.output.verdict != () && type_of(cw_r.output.verdict) == "string" {
        let cv = cw_r.output.verdict;
        if cv == "PASS" || cv == "pass" || cv == "Pass" || cv == "VERDICT: PASS" {
            cw_verdict = "PASS";
        } else {
            cw_verdict = "FAIL";
        }
    }
    if cw_r.output.summary != () { cw_summary = cw_r.output.summary; }
    if cw_r.output.issues != () { cw_issues = cw_r.output.issues; }
    if cw_r.output.correctness != () { cw_correctness = cw_r.output.correctness; }
    if cw_r.output.adequacy != () { cw_adequacy = cw_r.output.adequacy; }
    if cw_r.output.quality != () { cw_quality = cw_r.output.quality; }
    if testing_verdict != "PASS" && cw_verdict == "PASS" {
        cw_verdict = "FAIL";
        cw_issues.push("script override: TESTING was not PASS — check-work cannot PASS");
    }
}

log("CHECK-WORK VERDICT: " + cw_verdict);

// --- Phase 7: Verdict report ------------------------------------------------

phase("Verdict");

let overall = "PASS";
let overall_blockers = [];

if testing_verdict != "PASS" {
    overall = "FAIL";
    overall_blockers.push("Testing gate: " + testing_verdict);
}
if cw_verdict != "PASS" {
    overall = "FAIL";
    overall_blockers.push("Check-work gate: " + cw_verdict);
}
if fix_failed_count > 0 {
    // not automatic overall fail if testing/check-work still pass and residual is low,
    // but record it; high residual handled by check-work
    overall_blockers.push("Resolve: " + fix_failed_count.to_string() + " fix agent(s) did not fix their target");
}
if deferred.len() > 0 {
    overall_blockers.push("Deferred confirmed issues: " + deferred.len().to_string() + " (over max_fix or not auto-safe)");
}

// If we only have soft deferred notes but gates pass, still PASS overall;
// recompute: hard fail only testing/check-work
overall = "PASS";
overall_blockers = [];
if testing_verdict != "PASS" {
    overall = "FAIL";
    overall_blockers.push("Testing gate: " + testing_verdict);
}
if cw_verdict != "PASS" {
    overall = "FAIL";
    overall_blockers.push("Check-work gate: " + cw_verdict);
}

let report = "";
report += "# Review-Fix-Diff Report\n\n";
report += "**Overall: " + overall + "**\n\n";
report += "| Stage | Result |\n";
report += "|-------|--------|\n";
report += "| Confirmed findings | " + confirmed.len().to_string() + " |\n";
report += "| Fixed | " + fixed_count.to_string() + " |\n";
report += "| Fix failed | " + fix_failed_count.to_string() + " |\n";
report += "| Deferred | " + deferred.len().to_string() + " |\n";
report += "| Testing | " + testing_verdict + " |\n";
report += "| Check-Work | " + cw_verdict + " |\n\n";

report += "## Scope\n\n";
report += "- Diff base: `" + base + "`\n";
report += "- Hot-path: " + hot_path.to_string() + "\n";
report += "- Safety surface: " + safety_surface.to_string() + "\n";
report += "- Themes: " + themes_blob + "\n";
report += "- Changed: " + changed_files.len().to_string() + "  Untracked: " + untracked_files.len().to_string() + "\n";
report += "- Freeze files: " + freeze_blob + "\n\n";
report += scope_summary + "\n\n";
if risk_notes != "" {
    report += "**Risk notes:** " + risk_notes + "\n\n";
}

report += "## Review lens summaries\n\n";
for rs in review_summaries {
    report += "- " + rs + "\n";
}
report += "\n";

report += "## Confirmed findings\n\n";
if confirmed.len() == 0 {
    report += "None survived adversarial verification.\n\n";
} else {
    for c in confirmed {
        let cid = if c.id != () { c.id } else { "?" };
        let csev = if c.severity != () { c.severity } else { "?" };
        let cfile = if c.file != () { c.file } else { "?" };
        let ciss = if c.issue != () { c.issue } else { "?" };
        let cev = if c.evidence != () { c.evidence } else { "" };
        report += "### " + cid + " (" + csev + ") — `" + cfile + "`\n\n";
        report += ciss + "\n\n";
        if cev != "" { report += "Evidence: " + cev + "\n\n"; }
    }
}

report += "## Resolve\n\n";
if fix_results_summary.len() == 0 {
    report += "No fixes attempted.\n\n";
} else {
    for fr in fix_results_summary {
        report += "- " + fr + "\n";
    }
    report += "\n";
}
if deferred.len() > 0 {
    report += "Deferred (not auto-fixed):\n";
    for d in deferred {
        let did = if d.id != () { d.id } else { "?" };
        let dfile = if d.file != () { d.file } else { "?" };
        let diss = if d.issue != () { d.issue } else { "?" };
        report += "- " + did + " `" + dfile + "`: " + diss + "\n";
    }
    report += "\n";
}

report += "## Testing\n\n";
report += testing_summary + "\n\n";
if testing_verdict != "PASS" && testing_blockers.len() > 0 {
    report += "Blockers:\n";
    for b in testing_blockers {
        report += "- " + b + "\n";
    }
    report += "\n";
}

report += "## Check-Work\n\n";
report += cw_summary + "\n\n";
if cw_correctness != "" { report += "- Correctness: " + cw_correctness + "\n"; }
if cw_adequacy != "" { report += "- Adequacy: " + cw_adequacy + "\n"; }
if cw_quality != "" { report += "- Quality: " + cw_quality + "\n"; }
report += "\n";
if cw_verdict != "PASS" && cw_issues.len() > 0 {
    report += "Issues:\n";
    for iss in cw_issues {
        report += "- " + iss + "\n";
    }
    report += "\n";
}

report += "## Overall blockers\n\n";
if overall_blockers.len() == 0 {
    report += "None — hard gates passed. Human judgment still required";
    if freeze_files.len() > 0 {
        report += " (freeze surface touched: LIVE_SAFETY_CCB_APPROVED + CCB/soak for merge)";
    }
    report += ".\n";
} else {
    for ob in overall_blockers {
        report += "- " + ob + "\n";
    }
    report += "\n**Do not commit/PR until blockers are addressed and this workflow is re-run.**\n";
}
report += "\n---\nGenerated by workflow `review-fix-diff`.\n";

let report_path = write_scratch_file("review-fix-diff-report.md", report);
log("OVERALL VERDICT: " + overall + " report=" + report_path);

complete(#{
    overall: overall,
    testing: testing_verdict,
    check_work: cw_verdict,
    confirmed_count: confirmed.len(),
    fixed_count: fixed_count,
    fix_failed_count: fix_failed_count,
    deferred_count: deferred.len(),
    hot_path: hot_path,
    safety_surface: safety_surface,
    freeze_files: freeze_files,
    blockers: overall_blockers,
    path: report_path,
    report: report,
});
