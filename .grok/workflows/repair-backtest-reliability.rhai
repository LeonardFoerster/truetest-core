// repair-backtest-reliability — analyze backtest logic reliability, run tests,
// fix confirmed defects, re-test and independent check-work.
//
// Args (all optional):
//   focus    — focus hint string (default "full")
//   max_fix  — max confirmed issues to auto-fix (default 8, max 12)
//   skip_fix — if true, analysis + test only (no edits)
//
// Overall PASS requires TESTING PASS + CHECK-WORK PASS after resolve.

let meta = #{
    name: "repair-backtest-reliability",
    description: "Analyze, test, and repair backtest logic reliability (engine_backtest only)",
    when_to_use: "When backtest results look untrustworthy, defect-closure WIP needs closure, or before trusting MC/research runs",
    phases: [
        #{ title: "Survey", detail: "parallel specialist lenses over backtest reliability" },
        #{ title: "Verify", detail: "adversarial verification of each finding" },
        #{ title: "Baseline", detail: "build + backtest-focused test suite" },
        #{ title: "Resolve", detail: "minimal fixes for confirmed high-impact defects" },
        #{ title: "Testing", detail: "full suite + gate scripts after repairs" },
        #{ title: "Check-Work", detail: "independent correctness verifier" },
        #{ title: "Verdict", detail: "reliability report and residual risk" },
    ],
};

// --- schemas ----------------------------------------------------------------

let findings_schema = #{
    "type": "object",
    "required": ["lens", "findings", "coverage_notes", "summary"],
    "properties": #{
        "lens": #{ "type": "string" },
        "findings": #{
            "type": "array",
            "maxItems": 8,
            "items": #{
                "type": "object",
                "required": ["id", "severity", "category", "title", "detail", "evidence", "files", "fix_hint", "usability_impact"],
                "properties": #{
                    "id": #{ "type": "string" },
                    "severity": #{ "type": "string" },
                    "category": #{ "type": "string" },
                    "title": #{ "type": "string" },
                    "detail": #{ "type": "string" },
                    "evidence": #{ "type": "string" },
                    "files": #{ "type": "string" },
                    "fix_hint": #{ "type": "string" },
                    "usability_impact": #{ "type": "string" },
                },
            },
        },
        "coverage_notes": #{
            "type": "array",
            "maxItems": 8,
            "items": #{ "type": "string" },
        },
        "summary": #{ "type": "string" },
    },
};

let verdict_schema = #{
    "type": "object",
    "required": ["finding_id", "real", "severity_adjusted", "reason", "evidence", "safe_to_auto_fix"],
    "properties": #{
        "finding_id": #{ "type": "string" },
        "real": #{ "type": "boolean" },
        "severity_adjusted": #{ "type": "string" },
        "reason": #{ "type": "string" },
        "evidence": #{ "type": "string" },
        "safe_to_auto_fix": #{ "type": "boolean" },
        "primary_file": #{ "type": "string" },
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
        "tests_added": #{ "type": "boolean" },
    },
};

let testing_schema = #{
    "type": "object",
    "required": ["verdict", "suite_green", "gates_green", "backtest_focused_green", "summary", "blockers"],
    "properties": #{
        "verdict": #{ "type": "string" },
        "suite_green": #{ "type": "boolean" },
        "gates_green": #{ "type": "boolean" },
        "backtest_focused_green": #{ "type": "boolean" },
        "summary": #{ "type": "string" },
        "blockers": #{ "type": "array", "items": #{ "type": "string" }, "maxItems": 20 },
        "commands_run": #{ "type": "array", "items": #{ "type": "string" }, "maxItems": 30 },
        "failed_tests": #{ "type": "array", "items": #{ "type": "string" }, "maxItems": 20 },
    },
};

let checkwork_schema = #{
    "type": "object",
    "required": ["verdict", "summary", "issues", "reliability_rating"],
    "properties": #{
        "verdict": #{ "type": "string" },
        "summary": #{ "type": "string" },
        "reliability_rating": #{ "type": "string" },
        "correctness": #{ "type": "string" },
        "determinism": #{ "type": "string" },
        "fill_realism": #{ "type": "string" },
        "test_confidence": #{ "type": "string" },
        "issues": #{ "type": "array", "items": #{ "type": "string" }, "maxItems": 20 },
        "residual_risks": #{ "type": "array", "items": #{ "type": "string" }, "maxItems": 12 },
    },
};

// --- args -------------------------------------------------------------------

let focus = "full";
let max_fix = 8;
let skip_fix = false;

if args != () {
    if args.focus != () && type_of(args.focus) == "string" && args.focus != "" {
        focus = args.focus;
    }
    if args.max_fix != () {
        if type_of(args.max_fix) == "i64" || type_of(args.max_fix) == "int" {
            max_fix = args.max_fix;
        }
    }
    if args.skip_fix == true {
        skip_fix = true;
    }
}
if max_fix < 1 { max_fix = 1; }
if max_fix > 12 { max_fix = 12; }

log("repair-backtest-reliability focus=" + focus
    + " max_fix=" + max_fix.to_string()
    + " skip_fix=" + skip_fix.to_string());

// --- Phase 1: Survey --------------------------------------------------------

phase("Survey");

let common = "";
common += "You are auditing TrueTest CORE BACKTEST reliability (engine_backtest / TT_TARGET_BACKTEST). ";
common += "IGNORE shadow mode, live trading, kill-switch, DMS, live reconciler unless they incorrectly bleed into backtest. ";
common += "READ-ONLY: use read_file, grep, list_dir, git status/diff. Do NOT edit files. ";
common += "There may already be WIP defect-closure changes in the working tree (engine, hybrid adapter, MC, portfolio, tests/test_backtest_defect_closure.cpp). ";
common += "Inspect CURRENT tree state, not just committed HEAD. ";
common += "Do not invent APIs — quote real types/functions/flags from code you read. ";
common += "Severity: critical | high | medium | low | info. ";
common += "Category: bug | logic | determinism | realism | usability | docs | test-gap | design. ";
common += "Every finding needs concrete evidence, fix_hint (how to fix), and usability_impact. ";
common += "Prefer reliability defects that make research conclusions wrong (wrong fills, non-determinism, silent data loss, double-process, look-ahead, PnL errors). ";
common += "Empty findings are valid ONLY after real inspection of named paths. ";
common += "Focus hint: " + focus + ". ";
common += "Key paths: src/engine/, src/execution/ (non-live), src/simulation/, src/data/, src/strategy/, ";
common += "src/exits/, src/risk/ (backtest use), src/bin/main.inc, src/api/, tests/, docs/architecture/03-realism.md. ";

let p1 = common;
p1 += "LENS id=engine-loop. Map backtest lifecycle: mode backtest, event replay, publish_event, ";
p1 += "strategy callbacks (on_market/on_tick), order submit path, end-of-run report. ";
p1 += "Search for off-by-one, double-process, wrong mode branches, silent no-ops, unhandled errors, ";
p1 += "DAY cancel vs hybrid queue, adapter routing bugs. Read engine.h/cpp carefully (skim live-only). ";
p1 += "Return lens=engine-loop, findings, coverage_notes, summary.";

let p2 = common;
p2 += "LENS id=fill-realism. Audit fill/execution reliability: fee_model, latency_model, impact_model, ";
p2 += "queue models, HybridPaperAdapter, QueueAwareBookAdapter, portfolio, stop/exit fill pricing, ";
p2 += "DefaultExitPolicy. Look for look-ahead, wrong-side fills, zero-cost silent defaults, ";
p2 += "partial fills missing, SL/TP inconsistencies, mark-to-market errors, modify false-success. ";
p2 += "Return lens=fill-realism, findings, coverage_notes, summary.";

let p3 = common;
p3 += "LENS id=data-replay. Audit historical/synthetic data → engine: data_handler, CSV/tick/binary, ";
p3 += "DataBridge historical, symbol resolution, bar vs tick, time ordering, gaps, warmup. ";
p3 += "Flag silent row drops, timezone/parse bugs, non-deterministic sort, empty-data crash vs clear error. ";
p3 += "Return lens=data-replay, findings, coverage_notes, summary.";

let p4 = common;
p4 += "LENS id=determinism-mc. Audit determinism & Monte Carlo: seeding (base+trial), ";
p4 += "monte_carlo_controller/reporter/generators, --mc-reuse-objects, thread preset inline, ";
p4 += "shared mutable state between trials, report trials[]/seed_used, object reset completeness. ";
p4 += "Flag racey shared state, non-reproducible defaults, MC CLI footguns. ";
p4 += "Return lens=determinism-mc, findings, coverage_notes, summary.";

let p5 = common;
p5 += "LENS id=accounting-report. Audit portfolio accounting, analytics, report JSON reproducibility fields, ";
p5 += "open PnL, equity curve consistency, dashboard snapshot for backtest. ";
p5 += "Flag wrong cash/position updates, missing report fields, analytics split soft counters that hide losses. ";
p5 += "Return lens=accounting-report, findings, coverage_notes, summary.";

let p6 = common;
p6 += "LENS id=strategy-sdk. Audit strategy author reliability: strategy interface, indicators, order APIs, ";
p6 += "position queries, exit_intent, sma/ma_crossover examples. ";
p6 += "Flag footguns that produce wrong research conclusions (same-bar fill look-ahead, wrong bar access, ";
p6 += "unclear PnL, SMA reuse maps, state not reset across MC trials). ";
p6 += "Return lens=strategy-sdk, findings, coverage_notes, summary.";

let p7 = common;
p7 += "LENS id=tests-gaps. Map tests covering backtest realism/determinism/fills/MC/report/defect-closure. ";
p7 += "Search tests/ for fill, portfolio, stop, monte, backtest, defect, hybrid, queue, latency, fee. ";
p7 += "Check cmake/Sources.cmake registers test_backtest_defect_closure and new sources. ";
p7 += "Identify high-risk untested logic and tests that cannot fail. ";
p7 += "Return lens=tests-gaps, findings, coverage_notes, summary.";

let survey = parallel([
    #{ prompt: p1, label: "engine-loop", capability_mode: "read-only", output_schema: findings_schema, agent_type: "explore" },
    #{ prompt: p2, label: "fill-realism", capability_mode: "read-only", output_schema: findings_schema, agent_type: "explore" },
    #{ prompt: p3, label: "data-replay", capability_mode: "read-only", output_schema: findings_schema, agent_type: "explore" },
    #{ prompt: p4, label: "determinism-mc", capability_mode: "read-only", output_schema: findings_schema, agent_type: "explore" },
    #{ prompt: p5, label: "accounting-report", capability_mode: "read-only", output_schema: findings_schema, agent_type: "explore" },
    #{ prompt: p6, label: "strategy-sdk", capability_mode: "read-only", output_schema: findings_schema, agent_type: "explore" },
    #{ prompt: p7, label: "tests-gaps", capability_mode: "read-only", output_schema: findings_schema, agent_type: "explore" },
]);

let all_findings = [];
let lens_labels = [
    "engine-loop",
    "fill-realism",
    "data-replay",
    "determinism-mc",
    "accounting-report",
    "strategy-sdk",
    "tests-gaps",
];
let survey_bundle = "";
let lenses_ok = 0;
let li = 0;
for r in survey {
    let lab = lens_labels[li];
    if r == () || !r.success {
        survey_bundle += "### " + lab + "\nFAILED — treat as uncovered.\n\n";
    } else {
        lenses_ok += 1;
        survey_bundle += "### " + lab + "\n";
        survey_bundle += "summary: " + r.output.summary + "\n";
        if r.output.coverage_notes != () {
            survey_bundle += "coverage:\n";
            for n in r.output.coverage_notes {
                survey_bundle += "- " + n + "\n";
            }
        }
        if r.output.findings != () {
            let fi = 0;
            for f in r.output.findings {
                let fid = f.id;
                if fid == () || fid == "" {
                    fid = lab + "-" + fi.to_string();
                }
                let item = #{
                    id: fid,
                    lens: lab,
                    severity: f.severity,
                    category: f.category,
                    title: f.title,
                    detail: f.detail,
                    evidence: f.evidence,
                    files: f.files,
                    fix_hint: if f.fix_hint != () { f.fix_hint } else { "" },
                    usability_impact: f.usability_impact,
                };
                all_findings.push(item);
                survey_bundle += "- [" + f.severity + "/" + f.category + "] " + f.title + " (" + fid + ")\n";
                fi += 1;
            }
        }
        survey_bundle += "\n";
    }
    li += 1;
}

log(lenses_ok.to_string() + "/7 survey lenses OK; raw findings=" + all_findings.len().to_string());

// Cap verification fan-out
const MAX_VERIFY = 20;
let to_verify = [];
let dropped = 0;
let vi = 0;
for f in all_findings {
    if vi < MAX_VERIFY {
        to_verify.push(f);
    } else {
        dropped += 1;
    }
    vi += 1;
}
if dropped > 0 {
    log("Dropped " + dropped.to_string() + " findings from verify panel (cap " + MAX_VERIFY.to_string() + ")");
}

// --- Phase 2: Verify --------------------------------------------------------

phase("Verify");

let confirmed = [];
let rejected = [];
let unverified = [];

if to_verify.len() == 0 {
    log("No findings to verify");
} else {
    let vjobs = [];
    for f in to_verify {
        let vp = "";
        vp += "Adversarially VERIFY this BACKTEST reliability finding against the CURRENT working tree. ";
        vp += "READ-ONLY: read_file/grep/git diff the cited files. ";
        vp += "Set real=true ONLY if you independently confirm the defect with concrete code evidence. ";
        vp += "Default real=false if speculative, already fixed in WIP, out of scope (shadow/live), or misread. ";
        vp += "severity_adjusted: critical|high|medium|low|info. ";
        vp += "safe_to_auto_fix=true only if a small local fix is clear and does not require product design choices. ";
        vp += "primary_file: single best path to edit. ";
        vp += "finding_id must equal the given id.\n\n";
        vp += "id: " + f.id + "\n";
        vp += "lens: " + f.lens + "\n";
        vp += "claimed severity: " + f.severity + "\n";
        vp += "category: " + f.category + "\n";
        vp += "title: " + f.title + "\n";
        vp += "detail: " + f.detail + "\n";
        vp += "claimed evidence: " + f.evidence + "\n";
        vp += "files: " + f.files + "\n";
        vp += "fix_hint: " + f.fix_hint + "\n";
        vp += "usability_impact: " + f.usability_impact + "\n";
        vjobs.push(#{
            prompt: vp,
            label: "verify:" + f.id,
            capability_mode: "read-only",
            output_schema: verdict_schema,
            agent_type: "explore",
        });
    }
    let verdicts = parallel(vjobs);
    let i = 0;
    for v in verdicts {
        let src = to_verify[i];
        if v == () || !v.success {
            unverified.push(src);
        } else if v.output.real == true && v.output.evidence != () && v.output.evidence != "" {
            let sev = v.output.severity_adjusted;
            if sev == () || sev == "" { sev = src.severity; }
            let pfile = "";
            if v.output.primary_file != () { pfile = v.output.primary_file; }
            if pfile == "" {
                // use first path token from files if present
                pfile = src.files;
            }
            confirmed.push(#{
                id: src.id,
                lens: src.lens,
                severity: sev,
                category: src.category,
                title: src.title,
                detail: src.detail,
                evidence: v.output.evidence,
                reason: v.output.reason,
                files: src.files,
                primary_file: pfile,
                fix_hint: src.fix_hint,
                usability_impact: src.usability_impact,
                safe_to_auto_fix: v.output.safe_to_auto_fix == true,
            });
        } else {
            rejected.push(#{
                id: src.id,
                title: src.title,
                reason: if v.output.reason != () { v.output.reason } else { "not real" },
            });
        }
        i += 1;
    }
}

log("Verified: confirmed=" + confirmed.len().to_string()
    + " rejected=" + rejected.len().to_string()
    + " unverified=" + unverified.len().to_string());

// Select fix targets: critical/high first, then safe_to_auto_fix mediums
let to_fix = [];
let deferred = [];
// Pass 1: critical/high
for c in confirmed {
    let sev = if c.severity != () { c.severity } else { "medium" };
    let is_high = sev == "critical" || sev == "high" || sev == "CRITICAL" || sev == "HIGH";
    if is_high && to_fix.len() < max_fix {
        to_fix.push(c);
    }
}
// Pass 2: safe medium/low
for c in confirmed {
    let already = false;
    for t in to_fix {
        if t.id == c.id { already = true; }
    }
    if already { continue; }
    let sev = if c.severity != () { c.severity } else { "medium" };
    let is_high = sev == "critical" || sev == "high" || sev == "CRITICAL" || sev == "HIGH";
    if !is_high && c.safe_to_auto_fix == true && to_fix.len() < max_fix {
        to_fix.push(c);
    } else {
        deferred.push(c);
    }
}
// Anything high that didn't fit max_fix goes deferred
for c in confirmed {
    let already = false;
    for t in to_fix {
        if t.id == c.id { already = true; }
    }
    if already { continue; }
    let in_def = false;
    for d in deferred {
        if d.id == c.id { in_def = true; }
    }
    if !in_def {
        deferred.push(c);
    }
}

log("fix queue=" + to_fix.len().to_string() + " deferred=" + deferred.len().to_string());

// --- Phase 3: Baseline tests ------------------------------------------------

phase("Baseline");

let baseline_prompt = "";
baseline_prompt += "You are the BASELINE tester for TrueTest backtest reliability.\n";
baseline_prompt += "Run builds and tests. Do NOT edit source to make tests pass (report FAIL instead).\n";
baseline_prompt += "capability: execute. cwd is the core/ workspace.\n\n";
baseline_prompt += "Confirmed defects so far (for context; do not fix yet):\n";
for c in confirmed {
    baseline_prompt += "- [" + c.severity + "] " + c.id + ": " + c.title + "\n";
}
baseline_prompt += "\nMANDATORY protocol:\n";
baseline_prompt += "1) git status -sb; git diff --stat HEAD; git ls-files --others --exclude-standard\n";
baseline_prompt += "2) Ensure tests are registered: grep test_backtest_defect_closure cmake/Sources.cmake\n";
baseline_prompt += "3) Build:\n";
baseline_prompt += "   Prefer: cmake --preset linux-tests && cmake --build --preset linux-tests -j\n";
baseline_prompt += "   Fallback: cmake -B build -DBUILD_TESTS=ON && cmake --build build -j\n";
baseline_prompt += "4) Backtest-focused suite first:\n";
baseline_prompt += "   ctest --test-dir out/build/linux-tests -R 'Backtest|Defect|Portfolio|MonteCarlo|Fill|Queue|Hybrid|Realistic|Stop|Strategy|Engine' --output-on-failure\n";
baseline_prompt += "   (adjust dir if using build/)\n";
baseline_prompt += "5) If focused suite green, run full: ctest --test-dir <dir> --output-on-failure\n";
baseline_prompt += "6) Gates:\n";
baseline_prompt += "   ./scripts/check-hotpath-json.sh\n";
baseline_prompt += "   ./scripts/check-layer-deps.sh\n";
baseline_prompt += "   ./scripts/check-live-safety-freeze.sh\n\n";
baseline_prompt += "verdict PASS only if suite_green AND gates_green AND backtest_focused_green.\n";
baseline_prompt += "If build fails due to incomplete WIP, still report FAIL with concrete blockers — that is useful baseline data.\n";
baseline_prompt += "failed_tests: list failing test names if any.\n";

let baseline_r = agent(baseline_prompt, #{
    label: "baseline-test",
    capability_mode: "execute",
    output_schema: testing_schema,
});

let baseline_verdict = "FAIL";
let baseline_summary = "Baseline agent failed (fail closed).";
let baseline_blockers = ["baseline agent did not return usable output"];
let baseline_suite = false;
let baseline_gates = false;
let baseline_focus = false;

if baseline_r != () && baseline_r.success && baseline_r.output != () {
    if baseline_r.output.verdict != () {
        let tv = baseline_r.output.verdict;
        if tv == "PASS" || tv == "pass" || tv == "Pass" || tv == "TESTING VERDICT: PASS" {
            baseline_verdict = "PASS";
        }
    }
    if baseline_r.output.summary != () { baseline_summary = baseline_r.output.summary; }
    if baseline_r.output.blockers != () { baseline_blockers = baseline_r.output.blockers; }
    if baseline_r.output.suite_green == true { baseline_suite = true; }
    if baseline_r.output.gates_green == true { baseline_gates = true; }
    if baseline_r.output.backtest_focused_green == true { baseline_focus = true; }
    if baseline_verdict == "PASS" && (!baseline_suite || !baseline_gates || !baseline_focus) {
        baseline_verdict = "FAIL";
        baseline_blockers.push("script override: PASS without suite+gates+backtest_focused all true");
    }
}

log("BASELINE: " + baseline_verdict + " suite=" + baseline_suite.to_string()
    + " gates=" + baseline_gates.to_string() + " focus=" + baseline_focus.to_string());

// --- Phase 4: Resolve -------------------------------------------------------

phase("Resolve");

let fix_results_summary = [];
let fixed_count = 0;
let fix_failed_count = 0;

if skip_fix {
    log("skip_fix=true — resolve is a no-op");
} else if to_fix.len() == 0 {
    log("No auto-fix targets — resolve is a no-op");
} else {
    // Group by primary_file to avoid parallel thrash on same file
    let file_groups = []; // #{ file, issues: [] } rebuilt as list of issue arrays per file
    let file_keys = [];
    for c in to_fix {
        let pf = if c.primary_file != () && c.primary_file != "" { c.primary_file } else { c.files };
        // normalize: take first path-like token up to comma/space if multi
        let key = pf;
        let found_idx = -1;
        let ki = 0;
        for k in file_keys {
            if k == key { found_idx = ki; }
            ki += 1;
        }
        if found_idx < 0 {
            file_keys.push(key);
            file_groups.push(#{ file: key, issues: [c] });
        } else {
            // rebuild group with extra issue (Rhai maps/arrays are mutable)
            file_groups[found_idx].issues.push(c);
        }
    }

    let fjobs = [];
    let fgi = 0;
    for g in file_groups {
        let issues_text = "";
        let ids = "";
        for c in g.issues {
            ids += c.id + ",";
            issues_text += "### " + c.id + " [" + c.severity + "/" + c.category + "]\n";
            issues_text += "title: " + c.title + "\n";
            issues_text += "detail: " + c.detail + "\n";
            issues_text += "evidence: " + c.evidence + "\n";
            issues_text += "fix_hint: " + c.fix_hint + "\n";
            issues_text += "files: " + c.files + "\n";
            issues_text += "impact: " + c.usability_impact + "\n\n";
        }

        let fp = "";
        fp += "You are a FIX agent for TrueTest BACKTEST reliability. Apply MINIMAL correct fixes.\n";
        fp += "capability: all (edit + focused build/test). Follow AGENTS.md hot-path (R1-R10) and safety (S1-S10).\n";
        fp += "Scope: BACKTEST only — do not weaken live safety; engine.cpp is freeze-surface — surgical only.\n\n";
        fp += "Primary file group: " + g.file + "\n";
        fp += "Issue ids: " + ids + "\n";
        fp += "Baseline test status was: " + baseline_verdict + " — " + baseline_summary + "\n\n";
        fp += issues_text;
        fp += "Rules:\n";
        fp += "1. Read current files first; confirm each bug still exists.\n";
        fp += "2. Smallest correct change only — no drive-by refactors.\n";
        fp += "3. Register new sources in cmake/Sources.cmake if needed.\n";
        fp += "4. Prefer adding/extending tests/test_backtest_defect_closure.cpp for regressions.\n";
        fp += "5. Hot path: zero heap; pools; no JSON; no runtime pool grow.\n";
        fp += "6. Determinism: MC trial isolation, seed_used, no shared mutable trial state.\n";
        fp += "7. After edits, try: cmake --build --preset linux-tests -j --target truetest_tests\n";
        fp += "   and focused ctest -R 'Backtest|Defect|...' if build ok.\n";
        fp += "8. If unsafe/ambiguous product intent, set fixed=false with residual why.\n";
        fp += "9. Do NOT commit or push.\n";
        fp += "10. If multiple issues in this group, fix all you can safely; residual lists the rest.\n\n";
        fp += "Return fixed=true only if at least one confirmed issue is actually repaired.\n";

        fjobs.push(#{
            prompt: fp,
            label: "fix:" + fgi.to_string() + ":" + g.file,
            capability_mode: "all",
            output_schema: fix_schema,
        });
        fgi += 1;
    }

    // Sequential-ish: still parallel but file-grouped to reduce conflicts
    let fix_outs = parallel(fjobs);
    let foi = 0;
    for fo in fix_outs {
        let g = file_groups[foi];
        let label = g.file;
        if fo != () && fo.success && fo.output != () {
            let was_fixed = fo.output.fixed == true;
            if was_fixed { fixed_count += 1; } else { fix_failed_count += 1; }
            let sm = if fo.output.summary != () { fo.output.summary } else { "(no summary)" };
            let rs = if fo.output.residual != () { fo.output.residual } else { "" };
            fix_results_summary.push(label + ": fixed=" + was_fixed.to_string() + " — " + sm
                + (if rs != "" { " residual=" + rs } else { "" }));
        } else {
            fix_failed_count += 1;
            fix_results_summary.push(label + ": fix agent failed");
        }
        foi += 1;
    }
}

log("resolve: groups_fixed=" + fixed_count.to_string() + " groups_failed=" + fix_failed_count.to_string());

// --- Phase 5: Testing after repair ------------------------------------------

phase("Testing");

let testing_prompt = "";
testing_prompt += "You are the TESTING hard gate for TrueTest backtest reliability AFTER repairs.\n";
testing_prompt += "You MUST actually run builds, tests, and gate scripts — no pass-from-memory.\n";
testing_prompt += "capability: execute. Prefer FAIL with blockers over greenwash. Only apply a trivial compile-typo fix if you introduced it; otherwise do not edit product logic.\n\n";
testing_prompt += "Baseline was: " + baseline_verdict + " — " + baseline_summary + "\n";
testing_prompt += "Fixes this run: groups_fixed=" + fixed_count.to_string()
    + " groups_failed=" + fix_failed_count.to_string() + " skip_fix=" + skip_fix.to_string() + "\n";
testing_prompt += "Fix notes:\n";
for fr in fix_results_summary {
    testing_prompt += "  - " + fr + "\n";
}
testing_prompt += "\nConfirmed defects context:\n";
for c in confirmed {
    testing_prompt += "- " + c.id + " [" + c.severity + "] " + c.title + "\n";
}
testing_prompt += "\nMANDATORY protocol (cwd = core/):\n";
testing_prompt += "1) git status -sb; git diff --stat HEAD; git ls-files --others --exclude-standard\n";
testing_prompt += "2) Build tests: cmake --preset linux-tests && cmake --build --preset linux-tests -j\n";
testing_prompt += "   Fallback: cmake -B build -DBUILD_TESTS=ON && cmake --build build -j\n";
testing_prompt += "3) Backtest-focused:\n";
testing_prompt += "   ctest --test-dir out/build/linux-tests -R 'Backtest|Defect|Portfolio|MonteCarlo|Fill|Queue|Hybrid|Realistic|Stop|Strategy|Engine|hotpath|Hotpath' --output-on-failure\n";
testing_prompt += "4) Full suite: ctest --test-dir out/build/linux-tests --output-on-failure\n";
testing_prompt += "5) Gates:\n";
testing_prompt += "   ./scripts/check-hotpath-json.sh\n";
testing_prompt += "   ./scripts/check-layer-deps.sh\n";
testing_prompt += "   ./scripts/check-live-safety-freeze.sh\n";
testing_prompt += "6) Optional smoke if binary exists:\n";
testing_prompt += "   ./out/build/linux-tests/engine_backtest --provider synthetic --strategy sma --seed 424242 --no-pin --status-format off --no-tui --output /tmp/bt-reliability.json\n";
testing_prompt += "   (or build/engine_backtest if that is your tree)\n\n";
testing_prompt += "verdict PASS only if suite_green, gates_green, backtest_focused_green all true.\n";
testing_prompt += "blockers and failed_tests must be concrete on FAIL.\n";

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
let focus_green = false;

if testing_r != () && testing_r.success && testing_r.output != () {
    if testing_r.output.verdict != () {
        let tv = testing_r.output.verdict;
        if tv == "PASS" || tv == "pass" || tv == "Pass" || tv == "TESTING VERDICT: PASS" {
            testing_verdict = "PASS";
        }
    }
    if testing_r.output.summary != () { testing_summary = testing_r.output.summary; }
    if testing_r.output.blockers != () { testing_blockers = testing_r.output.blockers; }
    if testing_r.output.suite_green == true { suite_green = true; }
    if testing_r.output.gates_green == true { gates_green = true; }
    if testing_r.output.backtest_focused_green == true { focus_green = true; }
    if testing_verdict == "PASS" && (!suite_green || !gates_green || !focus_green) {
        testing_verdict = "FAIL";
        testing_blockers.push("script override: PASS without suite+gates+backtest_focused all true");
    }
}

log("TESTING VERDICT: " + testing_verdict);

// --- Phase 6: Check-work ----------------------------------------------------

phase("Check-Work");

let cw_prompt = "";
cw_prompt += "You are an INDEPENDENT verifier for BACKTEST reliability (/check-work). You did NOT implement the fixes.\n";
cw_prompt += "Re-read code and evidence. Do not rubber-stamp prior agents.\n";
cw_prompt += "Focus: can an R&D user trust engine_backtest results for research decisions?\n\n";
cw_prompt += "Survey lenses OK: " + lenses_ok.to_string() + "/7\n";
cw_prompt += "Confirmed: " + confirmed.len().to_string() + "  Fixed groups: " + fixed_count.to_string()
    + "  Fix failed groups: " + fix_failed_count.to_string() + "  Deferred: " + deferred.len().to_string() + "\n";
cw_prompt += "Baseline: " + baseline_verdict + " — " + baseline_summary + "\n";
cw_prompt += "Testing: " + testing_verdict + " — " + testing_summary + "\n\n";
cw_prompt += "Confirmed issues:\n";
for c in confirmed {
    cw_prompt += "- [" + c.id + "][" + c.severity + "] " + c.title + " | " + c.files + "\n";
    cw_prompt += "  " + c.detail + "\n";
}
cw_prompt += "\nDeferred (not auto-fixed):\n";
for d in deferred {
    cw_prompt += "- [" + d.id + "][" + d.severity + "] " + d.title + "\n";
}
cw_prompt += "\nFix notes:\n";
for fr in fix_results_summary {
    cw_prompt += "- " + fr + "\n";
}
cw_prompt += "\nProtocol:\n";
cw_prompt += "1) git status; git diff HEAD; read untracked key files (hybrid adapter, defect_closure tests).\n";
cw_prompt += "2) For each confirmed issue: fixed, still present, or wrongly marked fixed?\n";
cw_prompt += "3) Spot-check determinism (MC seed, trial reset), fill path, portfolio accounting, cmake Sources.\n";
cw_prompt += "4) Remaining critical/high unfixed => FAIL.\n";
cw_prompt += "5) If TESTING is FAIL, check-work must FAIL unless you re-prove suite green.\n";
cw_prompt += "6) reliability_rating one of: production-ready-research | usable-with-caveats | research-prototype | not-reliable\n\n";
cw_prompt += "verdict: PASS or FAIL only. issues: concrete blockers (empty only if PASS).\n";
cw_prompt += "residual_risks: known caveats even on PASS.\n";

let cw_r = agent(cw_prompt, #{
    label: "check-work",
    capability_mode: "execute",
    output_schema: checkwork_schema,
});

let cw_verdict = "FAIL";
let cw_summary = "Check-work agent failed or returned unusable output (fail closed).";
let cw_issues = ["check-work agent did not return a usable structured result"];
let cw_rating = "not-reliable";
let cw_correctness = "";
let cw_determinism = "";
let cw_fill = "";
let cw_testconf = "";
let cw_residuals = [];

if cw_r != () && cw_r.success && cw_r.output != () {
    if cw_r.output.verdict != () {
        let cv = cw_r.output.verdict;
        if cv == "PASS" || cv == "pass" || cv == "Pass" || cv == "VERDICT: PASS" {
            cw_verdict = "PASS";
        }
    }
    if cw_r.output.summary != () { cw_summary = cw_r.output.summary; }
    if cw_r.output.issues != () { cw_issues = cw_r.output.issues; }
    if cw_r.output.reliability_rating != () { cw_rating = cw_r.output.reliability_rating; }
    if cw_r.output.correctness != () { cw_correctness = cw_r.output.correctness; }
    if cw_r.output.determinism != () { cw_determinism = cw_r.output.determinism; }
    if cw_r.output.fill_realism != () { cw_fill = cw_r.output.fill_realism; }
    if cw_r.output.test_confidence != () { cw_testconf = cw_r.output.test_confidence; }
    if cw_r.output.residual_risks != () { cw_residuals = cw_r.output.residual_risks; }
    if testing_verdict != "PASS" && cw_verdict == "PASS" {
        cw_verdict = "FAIL";
        cw_issues.push("script override: TESTING was not PASS — check-work cannot PASS");
    }
}

log("CHECK-WORK VERDICT: " + cw_verdict + " rating=" + cw_rating);

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

let report = "";
report += "# Backtest Reliability Repair Report\n\n";
report += "**Overall: " + overall + "**\n\n";
report += "| Stage | Result |\n";
report += "|-------|--------|\n";
report += "| Survey lenses | " + lenses_ok.to_string() + "/7 |\n";
report += "| Raw findings | " + all_findings.len().to_string() + " |\n";
report += "| Confirmed | " + confirmed.len().to_string() + " |\n";
report += "| Rejected | " + rejected.len().to_string() + " |\n";
report += "| Unverified | " + unverified.len().to_string() + " |\n";
report += "| Fix groups ok | " + fixed_count.to_string() + " |\n";
report += "| Fix groups failed | " + fix_failed_count.to_string() + " |\n";
report += "| Deferred | " + deferred.len().to_string() + " |\n";
report += "| Baseline | " + baseline_verdict + " |\n";
report += "| Testing | " + testing_verdict + " |\n";
report += "| Check-Work | " + cw_verdict + " |\n";
report += "| Reliability rating | " + cw_rating + " |\n\n";

report += "## Executive summary\n\n";
report += "Focus: `" + focus + "`  skip_fix=" + skip_fix.to_string() + "  max_fix=" + max_fix.to_string() + "\n\n";
report += "Baseline: " + baseline_summary + "\n\n";
report += "Testing: " + testing_summary + "\n\n";
report += "Check-work: " + cw_summary + "\n\n";

report += "## Survey coverage\n\n";
report += survey_bundle + "\n";

report += "## Confirmed defects\n\n";
if confirmed.len() == 0 {
    report += "None survived adversarial verification.\n\n";
} else {
    for c in confirmed {
        report += "### " + c.id + " (" + c.severity + " / " + c.category + ") — " + c.lens + "\n\n";
        report += "**" + c.title + "**\n\n";
        report += c.detail + "\n\n";
        report += "- Files: `" + c.files + "`\n";
        report += "- Evidence: " + c.evidence + "\n";
        report += "- Fix hint: " + c.fix_hint + "\n";
        report += "- Impact: " + c.usability_impact + "\n\n";
    }
}

report += "## Resolve\n\n";
if fix_results_summary.len() == 0 {
    report += "No fixes attempted";
    if skip_fix { report += " (skip_fix=true)"; }
    report += ".\n\n";
} else {
    for fr in fix_results_summary {
        report += "- " + fr + "\n";
    }
    report += "\n";
}
if deferred.len() > 0 {
    report += "Deferred (not auto-fixed):\n";
    for d in deferred {
        report += "- " + d.id + " [" + d.severity + "] " + d.title + "\n";
    }
    report += "\n";
}

report += "## Rejected claims\n\n";
if rejected.len() == 0 {
    report += "None.\n\n";
} else {
    for r in rejected {
        report += "- " + r.id + ": " + r.title + " — " + r.reason + "\n";
    }
    report += "\n";
}

report += "## Testing blockers\n\n";
if testing_verdict == "PASS" {
    report += "None — suite and gates green.\n\n";
} else {
    for b in testing_blockers {
        report += "- " + b + "\n";
    }
    report += "\n";
}

report += "## Check-work dimensions\n\n";
if cw_correctness != "" { report += "- Correctness: " + cw_correctness + "\n"; }
if cw_determinism != "" { report += "- Determinism: " + cw_determinism + "\n"; }
if cw_fill != "" { report += "- Fill realism: " + cw_fill + "\n"; }
if cw_testconf != "" { report += "- Test confidence: " + cw_testconf + "\n"; }
report += "\n";
if cw_verdict != "PASS" && cw_issues.len() > 0 {
    report += "Issues:\n";
    for iss in cw_issues {
        report += "- " + iss + "\n";
    }
    report += "\n";
}
if cw_residuals.len() > 0 {
    report += "Residual risks:\n";
    for rr in cw_residuals {
        report += "- " + rr + "\n";
    }
    report += "\n";
}

report += "## Overall blockers\n\n";
if overall_blockers.len() == 0 {
    report += "None — hard gates passed. Human judgment still required before promoting strategies on fill-model caveats.\n";
} else {
    for ob in overall_blockers {
        report += "- " + ob + "\n";
    }
    report += "\n**Do not treat backtest results as decision-grade until blockers are addressed and this workflow is re-run.**\n";
}
report += "\n---\nGenerated by workflow `repair-backtest-reliability`.\n";

let report_path = write_scratch_file("backtest-reliability-repair-report.md", report);
log("OVERALL VERDICT: " + overall + " report=" + report_path);

complete(#{
    overall: overall,
    baseline: baseline_verdict,
    testing: testing_verdict,
    check_work: cw_verdict,
    reliability_rating: cw_rating,
    lenses_ok: lenses_ok,
    raw_findings: all_findings.len(),
    confirmed_count: confirmed.len(),
    rejected_count: rejected.len(),
    unverified_count: unverified.len(),
    fixed_groups: fixed_count,
    fix_failed_groups: fix_failed_count,
    deferred_count: deferred.len(),
    blockers: overall_blockers,
    path: report_path,
    report: report,
    success: overall == "PASS",
});
