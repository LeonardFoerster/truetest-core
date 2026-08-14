let meta = #{
    name: "analyze-backtest",
    description: "Audit backtest-only path for code/logic/usability defects; ignore shadow and live",
    when_to_use: "When assessing engine_backtest quality, fill realism, determinism, CLI/report usability, or MC campaigns",
    phases: [
        #{ title: "Survey", detail: "parallel specialist lenses over backtest surface" },
        #{ title: "Verify", detail: "adversarial verification of each finding" },
        #{ title: "Synthesize", detail: "usability verdict and prioritized defect list" },
    ],
};

let findings_schema = #{
    "type": "object",
    "required": ["lens", "findings", "coverage_notes", "summary"],
    "properties": #{
        "lens": #{ "type": "string" },
        "findings": #{
            "type": "array",
            "maxItems": 10,
            "items": #{
                "type": "object",
                "required": ["id", "severity", "category", "title", "detail", "evidence", "files", "usability_impact"],
                "properties": #{
                    "id": #{ "type": "string" },
                    "severity": #{ "type": "string" },
                    "category": #{ "type": "string" },
                    "title": #{ "type": "string" },
                    "detail": #{ "type": "string" },
                    "evidence": #{ "type": "string" },
                    "files": #{ "type": "string" },
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
    "required": ["finding_id", "real", "severity_adjusted", "reason", "evidence"],
    "properties": #{
        "finding_id": #{ "type": "string" },
        "real": #{ "type": "boolean" },
        "severity_adjusted": #{ "type": "string" },
        "reason": #{ "type": "string" },
        "evidence": #{ "type": "string" },
    },
};

// Optional focus string; empty = full backtest surface.
let focus = if args == () { () } else { args.focus };
if focus == () { focus = "full"; }

phase("Survey");

let common = "";
common += "You are auditing TrueTest CORE BACKTEST functionality only (engine_backtest / TT_TARGET_BACKTEST). ";
common += "IGNORE shadow mode, live trading, kill-switch, dead-man switch, live reconciler, and live-only safety freeze paths ";
common += "unless they incorrectly bleed into backtest. ";
common += "READ-ONLY analysis: use read_file, grep, list_dir. Do NOT edit files. ";
common += "Do not invent APIs — quote real types/functions/flags from code you read. ";
common += "Severity: critical | high | medium | low | info. ";
common += "Category: bug | logic | determinism | realism | usability | docs | test-gap | design. ";
common += "Every finding needs concrete evidence (file:symbol or flag behavior) and usability_impact ";
common += "(how it hurts an R&D user running backtests/MC). ";
common += "Empty findings are valid ONLY after real inspection of the named paths. ";
common += "Focus hint from user: " + focus + ". ";
common += "Prefer backtest-relevant paths: src/engine/, src/execution/ (non-live), src/simulation/, ";
common += "src/data/, src/strategy/, src/exits/, src/risk/ (backtest use), src/bin/, src/api/, ";
common += "src/providers/* for historical/synthetic only, tests/, docs/reference/, docs/architecture/. ";

let p1 = common;
p1 += "LENS id=engine-loop. Map backtest engine lifecycle: engine mode backtest, event replay, ";
p1 += "publish_event, strategy callbacks, order submit path in backtest, end-of-run report. ";
p1 += "Search for off-by-one, double-process, wrong mode branches, silent no-ops, unhandled errors. ";
p1 += "Read engine.h/cpp hot sections carefully (skim live-only blocks). ";
p1 += "Return lens=engine-loop, findings, coverage_notes, summary.";

let p2 = common;
p2 += "LENS id=fill-realism. Audit backtest fill/execution realism: fee_model, latency_model, ";
p2 += "impact_model, queue models, portfolio, position sizing, stop/exit fill pricing, ";
p2 += "DefaultExitPolicy / exit intents. Look for look-ahead, wrong side fills, zero-cost defaults, ";
p2 += "missing partial fills, SL/TP inconsistencies, mark-to-market errors. ";
p2 += "Return lens=fill-realism, findings, coverage_notes, summary.";

let p3 = common;
p3 += "LENS id=data-replay. Audit historical/synthetic data → engine: data_handler, CSV/tick/binary sources, ";
p3 += "DataBridge historical path, symbol resolution, bar vs tick replay, time ordering, gaps, ";
p3 += "warmup, multi-symbol. Flag silent drop of rows, timezone/parse bugs, non-deterministic sort, ";
p3 += "empty-data crash vs clear error. Return lens=data-replay, findings, coverage_notes, summary.";

let p4 = common;
p4 += "LENS id=determinism-mc. Audit determinism & Monte Carlo: seeding (base+trial), ";
p4 += "monte_carlo_controller/reporter/generators, --mc-reuse-objects, --thread-preset inline, ";
p4 += "shared mutable state between trials, report trials[]/seed_used. Flag racey shared state, ";
p4 += "non-reproducible defaults, MC CLI footguns. Return lens=determinism-mc, findings, coverage_notes, summary.";

let p5 = common;
p5 += "LENS id=cli-report-usability. Audit operator UX for backtest: CLI flags in main.inc / engine_config, ";
p5 += "defaults for engine_backtest, error messages, --output JSON schema fields for reproducibility, ";
p5 += "status-format, headless --no-tui, docs/reference/01-instructions.md and strategy docs. ";
p5 += "Flag confusing flags, missing validation, docs drift, report fields users need but lack. ";
p5 += "Return lens=cli-report-usability, findings, coverage_notes, summary.";

let p6 = common;
p6 += "LENS id=strategy-sdk. Audit strategy author experience on backtest: strategy interface, indicators, ";
p6 += "order APIs, position queries, exit_intent, examples (sma etc), docs/reference/07-strategy-development.md. ";
p6 += "Flag footguns that produce wrong research conclusions (wrong bar access, signal on same bar fill, ";
p6 += "unclear PnL, missing hooks). Return lens=strategy-sdk, findings, coverage_notes, summary.";

let p7 = common;
p7 += "LENS id=tests-gaps. Map tests covering backtest realism/determinism/fills/MC/report. ";
p7 += "Search tests/ for fill, portfolio, stop, monte, backtest, orderbook, latency, fee. ";
p7 += "Identify high-risk untested logic and flaky patterns. Prefer gaps that hide user-facing wrongness. ";
p7 += "Return lens=tests-gaps, findings, coverage_notes, summary.";

let survey = parallel([
    #{ prompt: p1, label: "engine-loop", capability_mode: "read-only", output_schema: findings_schema, agent_type: "explore" },
    #{ prompt: p2, label: "fill-realism", capability_mode: "read-only", output_schema: findings_schema, agent_type: "explore" },
    #{ prompt: p3, label: "data-replay", capability_mode: "read-only", output_schema: findings_schema, agent_type: "explore" },
    #{ prompt: p4, label: "determinism-mc", capability_mode: "read-only", output_schema: findings_schema, agent_type: "explore" },
    #{ prompt: p5, label: "cli-report-usability", capability_mode: "read-only", output_schema: findings_schema, agent_type: "explore" },
    #{ prompt: p6, label: "strategy-sdk", capability_mode: "read-only", output_schema: findings_schema, agent_type: "explore" },
    #{ prompt: p7, label: "tests-gaps", capability_mode: "read-only", output_schema: findings_schema, agent_type: "explore" },
]);

// Collect findings with stable ids; drop empties/failures.
let all_findings = [];
let lens_labels = [
    "engine-loop",
    "fill-realism",
    "data-replay",
    "determinism-mc",
    "cli-report-usability",
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
                // Stable id: lens short + index if agent id empty
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

// Cap verification fan-out to keep budget headroom for synthesis.
const MAX_VERIFY = 24;
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
        vp += "Adversarially VERIFY this backtest-audit finding. READ-ONLY: read_file/grep the cited files. ";
        vp += "Set real=true ONLY if you independently confirm the defect or usability issue with concrete code evidence. ";
        vp += "Default real=false if speculative, out of scope (shadow/live), already fixed, or misread. ";
        vp += "severity_adjusted: keep or lower/raise with justification (critical|high|medium|low|info). ";
        vp += "finding_id must equal the given id.\n\n";
        vp += "id: " + f.id + "\n";
        vp += "lens: " + f.lens + "\n";
        vp += "claimed severity: " + f.severity + "\n";
        vp += "category: " + f.category + "\n";
        vp += "title: " + f.title + "\n";
        vp += "detail: " + f.detail + "\n";
        vp += "claimed evidence: " + f.evidence + "\n";
        vp += "files: " + f.files + "\n";
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
                usability_impact: src.usability_impact,
            });
        } else {
            rejected.push(#{
                id: src.id,
                title: src.title,
                reason: v.output.reason,
            });
        }
        i += 1;
    }
}

log("Verified: confirmed=" + confirmed.len().to_string()
    + " rejected=" + rejected.len().to_string()
    + " unverified=" + unverified.len().to_string());

phase("Synthesize");

let conf_json = json_encode(confirmed);
let rej_json = json_encode(rejected);
let unv_json = json_encode(unverified);

let synth = "";
synth += "You are the final synthesizer for a BACKTEST-ONLY usability and defect audit of TrueTest core. ";
synth += "Shadow and live trading are out of scope. READ-ONLY: you may re-read a few key files to resolve contradictions.\n\n";
synth += "Produce a single markdown report for humans with these exact sections:\n";
synth += "1. Executive usability verdict (one of: production-ready research tool | usable with caveats | research prototype | not reliable for decisions) + 5-8 sentence rationale\n";
synth += "2. What works well (concrete strengths from evidence)\n";
synth += "3. Confirmed defects (table or bullets: severity | category | title | files | impact) ordered critical→info\n";
synth += "4. Unverified / rejected claims (brief; do not treat as true)\n";
synth += "5. Usability scorecard (1-5 each): CLI/docs, data ingest, fill realism trust, determinism/MC, strategy authoring, reports/reproducibility, test confidence — with one-line justification each\n";
synth += "6. Top 10 prioritized fixes for backtest usability (ordered by user harm)\n";
synth += "7. Recommended smoke checklist for a human (commands to run engine_backtest headless, what to inspect in JSON)\n";
synth += "8. Scope limits of this audit\n\n";
synth += "Rules: only elevate CONFIRMED findings as defects. Quote paths. Keep under ~300 lines. ";
synth += "Focus on whether an R&D user can trust backtest/MC results and operate the tool without tribal knowledge.\n\n";
synth += "SURVEY COVERAGE (" + lenses_ok.to_string() + "/7):\n" + survey_bundle + "\n";
synth += "CONFIRMED JSON:\n" + conf_json + "\n\n";
synth += "REJECTED JSON:\n" + rej_json + "\n\n";
synth += "UNVERIFIED JSON:\n" + unv_json + "\n";

let final_r = agent(synth, #{
    label: "synthesize-usability",
    capability_mode: "read-only",
    agent_type: "explore",
});

if final_r == () || !final_r.success {
    let fallback = "# Backtest analysis (partial)\n\n";
    fallback += "Lenses OK: " + lenses_ok.to_string() + "/7\n";
    fallback += "Confirmed: " + confirmed.len().to_string() + "\n";
    fallback += "Rejected: " + rejected.len().to_string() + "\n";
    fallback += "Unverified: " + unverified.len().to_string() + "\n\n";
    fallback += "## Survey\n" + survey_bundle + "\n";
    fallback += "## Confirmed\n" + conf_json + "\n";
    let path = write_scratch_file("backtest-usability-report.md", fallback);
    complete(#{
        path: path,
        summary: "Partial: synthesizer failed; raw survey + confirmed written",
        lenses_ok: lenses_ok,
        confirmed_count: confirmed.len(),
        rejected_count: rejected.len(),
        unverified_count: unverified.len(),
        success: false,
    });
}

let report_text = final_r.output;
if type_of(report_text) != "string" {
    report_text = json_encode(report_text);
}

let path = write_scratch_file("backtest-usability-report.md", report_text);
log("Wrote report to " + path);
complete(#{
    path: path,
    summary: "Backtest usability audit complete",
    lenses_ok: lenses_ok,
    raw_findings: all_findings.len(),
    confirmed_count: confirmed.len(),
    rejected_count: rejected.len(),
    unverified_count: unverified.len(),
    report: report_text,
    success: true,
});
