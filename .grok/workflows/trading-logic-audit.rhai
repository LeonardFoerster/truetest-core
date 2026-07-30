// trading-logic-audit — find and resolve logical errors critical for serious
// trading AND honest backtesting (look-ahead, fill realism, MC determinism).
//
// Scans portfolio/PnL, risk, order lifecycle, fills/realism, backtest fidelity,
// safety paths, provider contracts, exits/brackets for correctness bugs that
// would silently corrupt live or research decisions. Confirmed findings are
// fixed when safe; freeze-surface changes are reported for human CCB.
//
// Args (all optional):
//   scope   — "full" (default) | "risk" | "execution" | "safety" | "portfolio"
//             | "provider" | "backtest"
//   fix     — true (default) to apply non-freeze fixes; false = report-only
//   max_fix — max confirmed findings to fix in one run (default 4, max 6)

let meta = #{
    name: "trading-logic-audit",
    description: "Search, verify, and fix logical errors in trading and backtesting critical for serious trading",
    when_to_use: "Before serious trading/research, after execution/risk/backtest changes, or periodic correctness audits",
    phases: [
        #{ title: "Scan", detail: "parallel domain hunters for trading + backtest logic bugs" },
        #{ title: "Verify", detail: "adversarial verification of each finding" },
        #{ title: "Prioritize", detail: "rank confirmed bugs by trading/research impact" },
        #{ title: "Fix", detail: "apply safe non-freeze fixes" },
        #{ title: "Retest", detail: "build/tests/gates for applied fixes" },
        #{ title: "Report", detail: "structured audit report" },
    ],
};

// --- schemas ----------------------------------------------------------------

let findings_schema = #{
    "type": "object",
    "required": ["findings"],
    "properties": #{
        "findings": #{
            "type": "array",
            "maxItems": 6,
            "items": #{
                "type": "object",
                "required": ["id", "file", "severity", "category", "issue", "evidence", "why_trading_critical", "suggested_fix"],
                "properties": #{
                    "id": #{ "type": "string" },
                    "file": #{ "type": "string" },
                    "line_hint": #{ "type": "string" },
                    "severity": #{ "type": "string" },
                    "category": #{ "type": "string" },
                    "issue": #{ "type": "string" },
                    "evidence": #{ "type": "string" },
                    "why_trading_critical": #{ "type": "string" },
                    "suggested_fix": #{ "type": "string" },
                    "freeze_surface": #{ "type": "boolean" },
                    "test_gap": #{ "type": "string" },
                },
            },
        },
        "domain_summary": #{ "type": "string" },
        "files_read": #{ "type": "array", "items": #{ "type": "string" }, "maxItems": 40 },
    },
};

let verdict_schema = #{
    "type": "object",
    "required": ["real", "severity", "reason", "evidence", "freeze_surface", "fixable_now"],
    "properties": #{
        "real": #{ "type": "boolean" },
        "severity": #{ "type": "string" },
        "reason": #{ "type": "string" },
        "evidence": #{ "type": "string" },
        "freeze_surface": #{ "type": "boolean" },
        "fixable_now": #{ "type": "boolean" },
        "corrected_issue": #{ "type": "string" },
        "minimal_fix": #{ "type": "string" },
    },
};

let prioritize_schema = #{
    "type": "object",
    "required": ["ordered_ids", "rationale", "do_not_auto_fix"],
    "properties": #{
        "ordered_ids": #{ "type": "array", "items": #{ "type": "string" }, "maxItems": 12 },
        "rationale": #{ "type": "string" },
        "do_not_auto_fix": #{ "type": "array", "items": #{ "type": "string" }, "maxItems": 12 },
        "human_ccb_required": #{ "type": "array", "items": #{ "type": "string" }, "maxItems": 12 },
    },
};

let fix_schema = #{
    "type": "object",
    "required": ["fixed", "files_changed", "summary", "tests_added_or_updated", "residual_risk"],
    "properties": #{
        "fixed": #{ "type": "boolean" },
        "files_changed": #{ "type": "array", "items": #{ "type": "string" }, "maxItems": 20 },
        "summary": #{ "type": "string" },
        "tests_added_or_updated": #{ "type": "array", "items": #{ "type": "string" }, "maxItems": 12 },
        "residual_risk": #{ "type": "string" },
        "blocked_reason": #{ "type": "string" },
    },
};

let retest_schema = #{
    "type": "object",
    "required": ["verdict", "suite_relevant_green", "gates_green", "summary", "commands_run", "blockers"],
    "properties": #{
        "verdict": #{ "type": "string" },
        "suite_relevant_green": #{ "type": "boolean" },
        "gates_green": #{ "type": "boolean" },
        "summary": #{ "type": "string" },
        "commands_run": #{ "type": "array", "items": #{ "type": "string" }, "maxItems": 24 },
        "blockers": #{ "type": "array", "items": #{ "type": "string" }, "maxItems": 16 },
    },
};

// --- args -------------------------------------------------------------------

let scope = "full";
let do_fix = true;
let max_fix = 4;
if args != () {
    if args.scope != () && type_of(args.scope) == "string" && args.scope != "" {
        scope = args.scope;
    }
    if args.fix != () && type_of(args.fix) == "bool" {
        do_fix = args.fix;
    }
    if args.max_fix != () {
        // Rhai may present numbers as int/float; clamp defensively via string path later
        max_fix = 4;
        if type_of(args.max_fix) == "i64" || type_of(args.max_fix) == "int" {
            if args.max_fix >= 1 && args.max_fix <= 6 {
                max_fix = args.max_fix;
            }
        }
    }
}

// Freeze list — never auto-edit these without human CCB
let freeze_prefixes = [
    "src/core/tt_target.h",
    "src/engine/engine.cpp",
    "src/providers/binance/binance_futures_provider.h",
    "src/providers/binance/binance_futures_dead_mans_switch.h",
    "src/providers/binance/binance_futures_kill_switch.h",
    "src/providers/binance/binance_futures_reconciler.h",
    "src/providers/bitget/bitget_futures_provider.h",
    "src/providers/bitget/bitget_futures_dead_mans_switch.h",
    "src/providers/bitget/bitget_futures_kill_switch.h",
    "src/providers/bitget/bitget_futures_reconciler.h",
    "src/risk/risk_manager.h",
    "src/risk/futures_risk_check.h",
    "src/execution/live_safety.h",
    "src/threading/worker_watchdog.h",
];

// Rhai fns do not capture outer lets — pass the freeze list explicitly.
// Match exact relative path, absolute path ending, or exact basename of a
// freeze entry. Do NOT treat risk_manager.cpp as freeze (only risk_manager.h).
fn is_freeze_file(path, freeze_list) {
    for p in freeze_list {
        if path == p { return true; }
        // absolute or nested: ends with /p
        let needle = "/" + p;
        if path.len() >= needle.len() {
            let start = path.len() - needle.len();
            if path.sub_string(start, needle.len()) == needle { return true; }
        }
        // basename-only agent report: "futures_risk_check.h"
        let slash = -1;
        let i = 0;
        while i < p.len() {
            // find last '/'
            if p.sub_string(i, 1) == "/" { slash = i; }
            i += 1;
        }
        let base = p;
        if slash >= 0 {
            base = p.sub_string(slash + 1, p.len() - slash - 1);
        }
        if path == base { return true; }
    }
    false
}

fn severity_rank(s) {
    if s == "critical" { return 0; }
    if s == "high" { return 1; }
    if s == "medium" { return 2; }
    if s == "low" { return 3; }
    4
}

// --- domain catalog ---------------------------------------------------------
// Each domain is a self-contained hunting brief for a cold subagent.

let domains = [];

if scope == "full" || scope == "portfolio" {
    domains.push(#{
        name: "portfolio-pnl",
        paths: "src/execution/portfolio.{h,cpp}, src/analytics/, tests/test_portfolio.cpp, tests/test_analytics.cpp",
        focus: "Position accounting, average price, realized/unrealized PnL, funding fee application, multi-fill average, short vs long, reduce-only semantics, cash/equity updates, double-counting fees or fills, sign errors on shorts, mark vs last price for uPnL.",
    });
}
if scope == "full" || scope == "risk" {
    domains.push(#{
        name: "pretrade-risk",
        paths: "src/risk/risk_manager.{h,cpp}, src/risk/futures_risk_check.h, src/risk/maintenance_margin_table.*, tests/test_risk_manager.cpp, tests/test_futures_risk_check.cpp, tests/test_engine_venue_risk_check.cpp",
        focus: "Pre-trade checks that fail open, inverted comparisons, leverage/notional caps ignored, liquidation distance wrong sign, max position not enforced, risk limits only on open not increase, missing order of venue FuturesRiskCheck before RiskManager, circuit breakers never consulted.",
    });
}
if scope == "full" || scope == "execution" || scope == "backtest" {
    domains.push(#{
        name: "order-lifecycle",
        paths: "src/execution/order_tracker.h, src/execution/execution_bridge.h, src/execution/execution_adapter.h, src/engine/execution_router.*, src/execution/fill_parser.h, tests/test_execution_*.cpp, tests/test_cancel_race.cpp, tests/test_bridge_unknown_fill.cpp",
        focus: "Order state machine bugs: double fills, fills after cancel accepted, unknown fill handling, client_order_id collisions, partial fill qty accounting, cancel races, reject not clearing working qty, ownership of order objects, adapter vs tracker divergence.",
    });
    domains.push(#{
        name: "fills-realism",
        paths: "src/orderbook/, src/execution/*model*, src/execution/queue_*, src/execution/trade_tape_shadow_adapter.h, src/execution/fee_model.h, src/execution/latency_model.h, src/execution/impact_model.h, tests/test_fill_model.cpp, tests/test_realistic_fills.cpp, tests/test_queue_*.cpp, tests/test_engine_lookahead.cpp, tests/test_walked_book_impact.cpp, tests/test_latency_model.cpp, tests/test_fee_model.cpp",
        focus: "BACKTEST/SHADOW fill honesty: look-ahead bias, optimistic maker fills, fee sign errors, impact applied twice or never, latency not delaying eligibility, queue position never consumed, bar stop fills at close after high/low trigger without path, LocalBook discarding resting limits, live path accidentally using sim fill models.",
    });
}
if scope == "full" || scope == "backtest" {
    domains.push(#{
        name: "backtest-lookahead",
        paths: "src/engine/engine.cpp, src/engine/execution_router.*, src/orderbook/fill_model.h, src/execution/queue_aware_book_adapter.h, src/strategy/, tests/test_engine_lookahead.cpp, tests/test_engine.cpp, tests/test_engine_integration.cpp, tests/test_golden_regression.cpp",
        focus: "Look-ahead and bar-path fidelity crucial for serious research: strategy sees future bar fields before decision, stops/exits use high/low then fill at close without model, same-bar open+close of position with future knowledge, pending latency order eligible using future mid, indicators updated after signal with wrong bar ordering, OHLCV bar mode using close as both signal and fill without spread.",
    });
    domains.push(#{
        name: "backtest-determinism",
        paths: "src/simulation/, src/providers/synthetic/, src/engine/engine.cpp (reset_for_next_trial), src/bin/main.inc, tests/test_monte_carlo_*.cpp, tests/test_golden_regression.cpp, tests/providers/",
        focus: "MC and reproducibility: trials share mutable engine/strategy/RNG state, seed not applied per trial, parallel MC races, non-deterministic iteration order in maps affecting fills, synthetic provider ignoring seed, report missing seed_used, golden path drift, object reuse between trials leaving positions/orders/marks.",
    });
}
if scope == "full" || scope == "safety" {
    domains.push(#{
        name: "safety-paths",
        paths: "src/execution/live_safety.h, src/providers/*/*kill_switch*, src/providers/*/*dead_mans*, src/providers/*/*reconciler*, src/threading/worker_watchdog.h, src/core/tt_target.h, tests/test_live_safety.cpp, tests/test_worker_watchdog.cpp, tests/test_tick_to_trade_safety.cpp",
        focus: "Logical safety failures: halt not terminal, kill-switch retries, DMS adaptive lengthening, reconciler default-accept, live orders possible outside engine_live, watchdog that soft-fails, fail-open on parse errors for critical user-data. Report only; freeze files must NOT be auto-fixed.",
    });
}
if scope == "full" || scope == "provider" {
    domains.push(#{
        name: "provider-contracts",
        paths: "src/providers/provider.h, src/providers/provider_event.h, src/providers/data_bridge.h, src/providers/binance/, src/providers/bitget/, src/providers/synthetic/, tests/providers/, tests/test_provider_*.cpp, tests/test_data_bridge.cpp",
        focus: "Event contract bugs: wrong side/qty/price scaling, symbol map misses, timestamp units (ms vs us), trade vs book ticker confusion, user-data fill qty in contracts not coins, sequence gaps ignored, synthetic provider non-determinism under seed.",
    });
}
if scope == "full" {
    domains.push(#{
        name: "exits-brackets",
        paths: "src/exits/, src/execution/position_sizing.h, tests/test_exit_manager.cpp, tests/test_bracket_adapter.cpp, tests/test_engine_brackets.cpp, tests/test_position_sizing.cpp",
        focus: "Exit/bracket logic: stop loss on wrong side of market, OCO not cancelling sibling, trailing stop ratchets wrong direction, reduce-only exit increasing position, position sizing using wrong equity, brackets not re-armed after partial fill.",
    });
    domains.push(#{
        name: "engine-wiring",
        paths: "src/engine/engine.h, src/engine/engine_config.h, src/engine/execution_router.*, src/bin/main.inc, tests/test_engine*.cpp, tests/test_provider_engine_wiring.cpp",
        focus: "Composition-root logic errors: risk check not wired, wrong adapter for mode (backtest vs shadow vs live), strategy sees future events, portfolio update order after fill, MC trials sharing mutable state, live gate bypass via config flags.",
    });
}

if domains.len() == 0 {
    pause("verification", "Unknown args.scope — use full|risk|execution|safety|portfolio|provider|backtest");
}

// --- Phase 1: Scan ----------------------------------------------------------

phase("Scan");
log("Scanning " + domains.len().to_string() + " trading domains (scope=" + scope + ")");

let scan_jobs = [];
for d in domains {
    let p = "";
    p += "You are auditing the TrueTest C++ trading/backtesting engine for LOGICAL ERRORS that matter for serious trading and honest research.\n";
    p += "Domain: " + d.name + "\n";
    p += "Primary paths: " + d.paths + "\n";
    p += "Focus: " + d.focus + "\n\n";
    p += "ALREADY FIXED (do NOT re-report unless still broken in current code):\n";
    p += "- Analytics last_equity_ seeded and updated on market/tick/fill\n";
    p += "- RiskManager rejects zero-price non-reducing opens\n";
    p += "- FuturesRiskCheck uses mark equity (cash+qty*mark) for leverage/liq\n";
    p += "- engine funding → analytics_.on_event; per-symbol last_mids_by_symbol_\n";
    p += "- portfolio get_equity(marks_by_symbol) multi-symbol API\n\n";
    p += "MANDATORY METHOD:\n";
    p += "1. Use grep and read_file extensively — never answer from memory.\n";
    p += "2. Read implementation AND matching tests. A bug with no test is still a finding if code is wrong.\n";
    p += "3. Prefer falsifiable issues: wrong comparison, missing state transition, double apply, fail-open, sign error, look-ahead, non-determinism.\n";
    p += "4. Ignore style, naming, missing docs, and pure performance unless it changes trading or backtest outcomes.\n";
    p += "5. Severity: critical = wrong money/position/safety in live or research; high = can lose money or bias backtests under common paths; medium = edge cases; low = cosmetic.\n";
    p += "6. freeze_surface=true ONLY for exact freeze files: tt_target.h, engine.cpp, *kill_switch*, *dead_mans*, *reconciler*, risk_manager.h (NOT .cpp), futures_risk_check.h, live_safety.h, worker_watchdog.h.\n";
    p += "7. At most 6 findings. Empty findings is valid ONLY after reading real code and stating domain_summary of what you checked.\n";
    p += "8. id format: domain-shortslug e.g. backtest-lookahead-same-bar-close.\n";
    p += "9. evidence must cite file + function/symbol + what the code does wrong.\n";
    p += "10. Prefer NEW bugs not already fixed above. Backtest look-ahead and MC isolation are high value.\n";
    p += "Workspace root is the truetest core repo. Stay under src/ and tests/.\n";

    scan_jobs.push(#{
        prompt: p,
        label: "scan:" + d.name,
        capability_mode: "read-only",
        output_schema: findings_schema,
    });
}

let scan_results = parallel(scan_jobs);

let all_findings = [];
let scan_notes = [];
let di = 0;
for r in scan_results {
    let dname = domains[di].name;
    if r == () || !r.success {
        scan_notes.push(dname + ": scan failed/empty");
    } else {
        if r.output.domain_summary != () {
            scan_notes.push(dname + ": " + r.output.domain_summary);
        }
        if r.output.findings != () {
            for f in r.output.findings {
                // scope: must look like a repo path
                if f.file != () && f.file != "" && f.issue != () && f.issue != "" {
                    // stamp domain if id missing domain prefix
                    if f.id == () || f.id == "" {
                        f.id = dname + "-anon";
                    }
                    // force freeze flag if path matches freeze list
                    if is_freeze_file(f.file, freeze_prefixes) {
                        f.freeze_surface = true;
                    }
                    all_findings.push(f);
                }
            }
        }
    }
    di += 1;
}

log("Raw findings after scan: " + all_findings.len().to_string());

if all_findings.len() == 0 {
    let empty_report = "# Trading Logic Audit — No Findings\n\n";
    empty_report += "Scope: `" + scope + "`\n\n";
    empty_report += "All domain scanners returned empty after code inspection (or failed).\n\n";
    empty_report += "## Scan notes\n";
    for n in scan_notes {
        empty_report += "- " + n + "\n";
    }
    empty_report += "\n**Note:** Absence of findings is not a guarantee of correctness. Re-run with `scope=full` periodically.\n";
    let pth = write_scratch_file("trading-logic-audit.md", empty_report);
    complete(#{
        path: pth,
        summary: "No findings in scope " + scope,
        confirmed: [],
        fixed: [],
        blocked_freeze: [],
    });
}

// Cap verification load
let MAX_VERIFY = 12;
if all_findings.len() > MAX_VERIFY {
    log("Truncating findings for verify: " + all_findings.len().to_string() + " -> " + MAX_VERIFY.to_string());
    let trimmed = [];
    let ti = 0;
    for f in all_findings {
        if ti < MAX_VERIFY {
            trimmed.push(f);
        }
        ti += 1;
    }
    all_findings = trimmed;
}

// --- Phase 2: Verify --------------------------------------------------------

phase("Verify");
log("Adversarially verifying " + all_findings.len().to_string() + " findings");

let vjobs = [];
for f in all_findings {
    let issue = f.issue;
    let file = f.file;
    let evidence = if f.evidence != () { f.evidence } else { "" };
    let cat = if f.category != () { f.category } else { "" };
    let sev = if f.severity != () { f.severity } else { "medium" };
    let fid = if f.id != () { f.id } else { "unknown" };

    let vp = "";
    vp += "You are an adversarial verifier for a trading-engine bug report. Your job is to REFUTE weak claims.\n";
    vp += "Claim id: " + fid + "\n";
    vp += "File: " + file + "\n";
    vp += "Category: " + cat + " | alleged severity: " + sev + "\n";
    vp += "Issue: " + issue + "\n";
    vp += "Claimed evidence: " + evidence + "\n\n";
    vp += "METHOD (mandatory):\n";
    vp += "1. Read the actual file(s) with read_file/grep. Do not trust the claim.\n";
    vp += "2. Check tests that exercise the path — a deliberate design + tested behavior is often not a bug.\n";
    vp += "3. Set real=true ONLY if you independently confirm a logical error that can affect money, position, risk, fills, or safety.\n";
    vp += "4. Default real=false when uncertain, stylistic, speculative, or already correctly handled.\n";
    vp += "5. freeze_surface=true if the file is on the live-safety freeze list.\n";
    vp += "6. fixable_now=true only if a small, non-freeze, non-design-rewrite fix is clear.\n";
    vp += "7. evidence must quote concrete code behavior you observed.\n";
    vp += "8. If severity should change, set severity to critical|high|medium|low.\n";

    vjobs.push(#{
        prompt: vp,
        label: "verify:" + fid,
        capability_mode: "read-only",
        output_schema: verdict_schema,
    });
}

let verdicts = parallel(vjobs);

let confirmed = [];
let rejected = [];
let vi = 0;
for v in verdicts {
    let base = all_findings[vi];
    if v != () && v.success && v.output.real == true
        && v.output.evidence != () && v.output.evidence != "" {
        let item = #{
            id: base.id,
            file: base.file,
            issue: if v.output.corrected_issue != () && v.output.corrected_issue != "" { v.output.corrected_issue } else { base.issue },
            severity: if v.output.severity != () { v.output.severity } else { base.severity },
            category: base.category,
            evidence: v.output.evidence,
            why_trading_critical: base.why_trading_critical,
            suggested_fix: if v.output.minimal_fix != () && v.output.minimal_fix != "" { v.output.minimal_fix } else { base.suggested_fix },
            freeze_surface: v.output.freeze_surface == true || base.freeze_surface == true || is_freeze_file(base.file, freeze_prefixes),
            fixable_now: v.output.fixable_now == true,
            verify_reason: v.output.reason,
        };
        confirmed.push(item);
    } else {
        let why = "unverified";
        if v != () && v.success && v.output.reason != () {
            why = v.output.reason;
        }
        rejected.push(#{ id: base.id, file: base.file, reason: why });
    }
    vi += 1;
}

log(confirmed.len().to_string() + "/" + all_findings.len().to_string() + " findings survived verification");

if confirmed.len() == 0 {
    let rep = "# Trading Logic Audit — No Confirmed Findings\n\n";
    rep += "Scope: `" + scope + "`\n\n";
    rep += "Scanners proposed " + all_findings.len().to_string() + " issues; adversarial verification confirmed **0**.\n\n";
    rep += "## Rejected claims\n";
    for r in rejected {
        rep += "- `" + r.id + "` (" + r.file + "): " + r.reason + "\n";
    }
    rep += "\n## Scan notes\n";
    for n in scan_notes {
        rep += "- " + n + "\n";
    }
    let pth = write_scratch_file("trading-logic-audit.md", rep);
    complete(#{
        path: pth,
        summary: "0 confirmed findings after verification",
        confirmed: [],
        fixed: [],
        blocked_freeze: [],
        rejected_count: rejected.len(),
    });
}

// --- Phase 3: Prioritize ----------------------------------------------------

phase("Prioritize");

let conf_json = json_encode(confirmed);
let prio_prompt = "";
prio_prompt += "You prioritize confirmed trading/backtest-engine bugs for a fix wave.\n";
prio_prompt += "Confirmed findings JSON:\n" + conf_json + "\n\n";
prio_prompt += "Return ordered_ids by trading AND backtest-research impact (critical money/safety/look-ahead first).\n";
prio_prompt += "do_not_auto_fix: design rewrites, ambiguous product decisions, or need more research.\n";
prio_prompt += "human_ccb_required: ONLY exact freeze files (engine.cpp, futures_risk_check.h, risk_manager.h not .cpp, kill/dms/reconciler, live_safety.h, tt_target.h, worker_watchdog.h).\n";
prio_prompt += "Non-freeze files (analytics.cpp, portfolio, risk_manager.cpp, orderbook, exits, simulation, strategies) may be auto-fixed if fixable_now.\n";
prio_prompt += "Prefer fixable backtest honesty bugs (look-ahead, wrong fill price, MC state leak) when clear.\n";

let prio = agent(prio_prompt, #{
    label: "prioritize",
    capability_mode: "read-only",
    output_schema: prioritize_schema,
});

let ordered = [];
let no_auto = [];
let ccb = [];
if prio != () && prio.success && prio.output.ordered_ids != () {
    for id in prio.output.ordered_ids {
        // only keep ids that exist in confirmed
        for c in confirmed {
            if c.id == id {
                ordered.push(c);
            }
        }
    }
    if prio.output.do_not_auto_fix != () {
        no_auto = prio.output.do_not_auto_fix;
    }
    if prio.output.human_ccb_required != () {
        ccb = prio.output.human_ccb_required;
    }
}

// Fallback: severity sort if prioritizer failed
if ordered.len() == 0 {
    // simple insertion by severity_rank
    for c in confirmed {
        ordered.push(c);
    }
    // bubble sort by severity
    let n = ordered.len();
    let a = 0;
    while a < n {
        let b = 0;
        while b + 1 < n - a {
            if severity_rank(ordered[b].severity) > severity_rank(ordered[b + 1].severity) {
                let tmp = ordered[b];
                ordered[b] = ordered[b + 1];
                ordered[b + 1] = tmp;
            }
            b += 1;
        }
        a += 1;
    }
}

// Partition: auto-fix candidates vs blocked
let fix_queue = [];
let blocked_freeze = [];
let blocked_other = [];
for c in ordered {
    let blocked = false;
    if c.freeze_surface == true || is_freeze_file(c.file, freeze_prefixes) {
        blocked_freeze.push(c);
        blocked = true;
    }
    if !blocked {
        for id in no_auto {
            if id == c.id { blocked_other.push(c); blocked = true; }
        }
    }
    if !blocked {
        for id in ccb {
            if id == c.id {
                blocked_freeze.push(c);
                blocked = true;
            }
        }
    }
    if !blocked {
        if c.fixable_now == false {
            blocked_other.push(c);
        } else {
            fix_queue.push(c);
        }
    }
}

// --- Phase 4: Fix -----------------------------------------------------------

phase("Fix");

let fixed_results = [];
let fix_count = 0;

if !do_fix {
    log("Report-only mode (fix=false); skipping fixes");
} else if fix_queue.len() == 0 {
    log("No auto-fixable findings (freeze/CCB/design blocks)");
} else {
    log("Fix queue size " + fix_queue.len().to_string() + ", max_fix=" + max_fix.to_string());
    for c in fix_queue {
        if fix_count >= max_fix {
            log("max_fix reached; remaining findings deferred");
            break;
        }
        // refuse freeze paths even if prioritizer erred
        if is_freeze_file(c.file, freeze_prefixes) || c.freeze_surface == true {
            blocked_freeze.push(c);
        } else {
            let fp = "";
            fp += "You are fixing a CONFIRMED logical bug in the TrueTest C++ trading engine.\n";
            fp += "Finding id: " + c.id + "\n";
            fp += "File: " + c.file + "\n";
            fp += "Severity: " + c.severity + "\n";
            fp += "Issue: " + c.issue + "\n";
            fp += "Evidence: " + c.evidence + "\n";
            fp += "Suggested fix: " + c.suggested_fix + "\n";
            fp += "Why trading-critical: " + c.why_trading_critical + "\n\n";
            fp += "RULES:\n";
            fp += "1. Minimal correct fix only — no drive-by refactors.\n";
            fp += "2. NEVER edit freeze-list files: tt_target.h, engine.cpp, *kill_switch*, *dead_mans*, *reconciler*, risk_manager.h, futures_risk_check.h, live_safety.h, worker_watchdog.h.\n";
            fp += "3. If the only correct fix touches freeze files, set fixed=false and blocked_reason.\n";
            fp += "4. Prefer adding/updating a focused unit test proving the bug and fix.\n";
            fp += "5. Follow AGENTS.md: C++23, zero-alloc hot path, no new deps, register sources in cmake/Sources.cmake if new files.\n";
            fp += "6. After editing, run relevant tests if build tree exists (./out/build/linux-tests or ./build).\n";
            fp += "7. Report files_changed with repo-relative paths.\n";

            let fr = agent(fp, #{
                label: "fix:" + c.id,
                capability_mode: "all",
                output_schema: fix_schema,
            });

            if fr != () && fr.success && fr.output.fixed == true {
                fixed_results.push(#{
                    id: c.id,
                    summary: fr.output.summary,
                    files: fr.output.files_changed,
                    tests: fr.output.tests_added_or_updated,
                    residual: fr.output.residual_risk,
                });
                fix_count += 1;
            } else {
                let reason = "fix agent failed";
                if fr != () && fr.success && fr.output.blocked_reason != () {
                    reason = fr.output.blocked_reason;
                } else if fr != () && fr.success && fr.output.summary != () {
                    reason = fr.output.summary;
                }
                blocked_other.push(#{
                    id: c.id,
                    file: c.file,
                    issue: c.issue,
                    severity: c.severity,
                    category: c.category,
                    evidence: c.evidence,
                    why_trading_critical: c.why_trading_critical,
                    suggested_fix: c.suggested_fix,
                    freeze_surface: false,
                    fixable_now: false,
                    verify_reason: reason,
                });
            }
        }
    }
}

// --- Phase 5: Retest --------------------------------------------------------

phase("Retest");

let retest_verdict = "N/A";
let retest_summary = "No fixes applied; retest skipped";
let retest_blockers = [];
let retest_commands = [];

if fixed_results.len() > 0 {
    let changed = [];
    for fr in fixed_results {
        if fr.files != () {
            for f in fr.files { changed.push(f); }
        }
    }
    let tp = "";
    tp += "Verify the trading-logic fixes just applied in this workspace.\n";
    tp += "Fixed items JSON:\n" + json_encode(fixed_results) + "\n";
    tp += "Changed files (claimed): " + json_encode(changed) + "\n\n";
    tp += "MANDATORY:\n";
    tp += "1. Run gate scripts from repo root: ./scripts/check-hotpath-json.sh, ./scripts/check-layer-deps.sh, ./scripts/check-live-safety-freeze.sh\n";
    tp += "2. Build/tests if possible: prefer existing out/build/linux-tests or build; cmake --build if needed; ctest focused on affected tests then broader if time allows.\n";
    tp += "3. verdict PASS only if gates green AND relevant tests green.\n";
    tp += "4. FAIL if you cannot run tests or any fail.\n";
    tp += "5. List exact commands_run and blockers.\n";

    let tr = agent(tp, #{
        label: "retest",
        capability_mode: "all",
        output_schema: retest_schema,
    });

    if tr != () && tr.success {
        retest_verdict = tr.output.verdict;
        retest_summary = tr.output.summary;
        if tr.output.blockers != () { retest_blockers = tr.output.blockers; }
        if tr.output.commands_run != () { retest_commands = tr.output.commands_run; }
        if tr.output.suite_relevant_green != true || tr.output.gates_green != true {
            if retest_verdict != "FAIL" { retest_verdict = "FAIL"; }
        }
    } else {
        retest_verdict = "FAIL";
        retest_summary = "Retest agent failed";
        retest_blockers.push("retest agent unsuccessful");
    }
}

// --- Phase 6: Report --------------------------------------------------------

phase("Report");

let report = "";
report += "# Trading Logic Audit Report\n\n";
report += "- **Scope**: `" + scope + "`\n";
report += "- **Fix mode**: " + if do_fix { "on" } else { "report-only" } + "\n";
report += "- **Raw findings**: " + all_findings.len().to_string() + "\n";
report += "- **Confirmed**: " + confirmed.len().to_string() + "\n";
report += "- **Fixed**: " + fixed_results.len().to_string() + "\n";
report += "- **Blocked (freeze/CCB)**: " + blocked_freeze.len().to_string() + "\n";
report += "- **Blocked (other)**: " + blocked_other.len().to_string() + "\n";
report += "- **Retest**: " + retest_verdict + "\n\n";

report += "## Executive summary\n\n";
if confirmed.len() == 0 {
    report += "No confirmed logical errors after adversarial verification.\n\n";
} else {
    report += "Confirmed trading-logic issues that can affect money, positions, risk, fills, or safety. ";
    report += "Freeze-surface items require human CCB (`LIVE_SAFETY_CCB_APPROVED`) and were not auto-edited.\n\n";
}

report += "## Confirmed findings\n\n";
for c in confirmed {
    report += "### " + c.id + " (" + c.severity + ")\n\n";
    report += "- **File**: `" + c.file + "`\n";
    report += "- **Issue**: " + c.issue + "\n";
    report += "- **Evidence**: " + c.evidence + "\n";
    report += "- **Why critical**: " + c.why_trading_critical + "\n";
    report += "- **Suggested fix**: " + c.suggested_fix + "\n";
    report += "- **Freeze surface**: " + if c.freeze_surface { "yes" } else { "no" } + "\n\n";
}

if fixed_results.len() > 0 {
    report += "## Applied fixes\n\n";
    for fr in fixed_results {
        report += "### " + fr.id + "\n\n";
        report += fr.summary + "\n\n";
        if fr.files != () {
            report += "Files: ";
            for f in fr.files { report += "`" + f + "` "; }
            report += "\n\n";
        }
        if fr.residual != () && fr.residual != "" {
            report += "Residual risk: " + fr.residual + "\n\n";
        }
    }
}

if blocked_freeze.len() > 0 {
    report += "## Requires human CCB (freeze / live-safety)\n\n";
    for c in blocked_freeze {
        report += "- **" + c.id + "** (`" + c.file + "`): " + c.issue + "\n";
        report += "  - Fix sketch: " + c.suggested_fix + "\n";
    }
    report += "\nDo not auto-merge. Follow `docs/governance/02-prerequisites.md` and T3 multi-agent protocol.\n\n";
}

if blocked_other.len() > 0 {
    report += "## Deferred (not auto-fixed)\n\n";
    for c in blocked_other {
        let id = if c.id != () { c.id } else { "?" };
        let issue = if c.issue != () { c.issue } else { if c.verify_reason != () { c.verify_reason } else { "deferred" } };
        report += "- **" + id + "**: " + issue + "\n";
    }
    report += "\n";
}

if rejected.len() > 0 {
    report += "## Rejected after verification\n\n";
    for r in rejected {
        report += "- `" + r.id + "`: " + r.reason + "\n";
    }
    report += "\n";
}

report += "## Retest\n\n";
report += "- Verdict: **" + retest_verdict + "**\n";
report += "- Summary: " + retest_summary + "\n";
if retest_commands.len() > 0 {
    report += "- Commands:\n";
    for cmd in retest_commands { report += "  - `" + cmd + "`\n"; }
}
if retest_blockers.len() > 0 {
    report += "- Blockers:\n";
    for b in retest_blockers { report += "  - " + b + "\n"; }
}
report += "\n";

report += "## Scan notes\n\n";
for n in scan_notes {
    report += "- " + n + "\n";
}
report += "\n";

report += "## How to re-run\n\n";
report += "```text\n";
report += "/workflow trading-logic-audit\n";
report += "/workflow trading-logic-audit {\"scope\":\"risk\",\"fix\":false}\n";
report += "/workflow trading-logic-audit {\"scope\":\"full\",\"fix\":true,\"max_fix\":3}\n";
report += "```\n";

let out_path = write_scratch_file("trading-logic-audit.md", report);
log("Report written; confirmed=" + confirmed.len().to_string() + " fixed=" + fixed_results.len().to_string());

complete(#{
    path: out_path,
    summary: "confirmed=" + confirmed.len().to_string() + " fixed=" + fixed_results.len().to_string() + " retest=" + retest_verdict,
    confirmed_count: confirmed.len(),
    fixed_count: fixed_results.len(),
    blocked_freeze_count: blocked_freeze.len(),
    retest: retest_verdict,
    report: report,
});
