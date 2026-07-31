let meta = #{
    name: "ma-crossover-open-close",
    description: "Audit MA-crossover backtest open/close path: signal field, next-bar fill, OHLC mapping, independent oracle",
    when_to_use: "When MA crossover / SMA backtests look wrong and open vs close fill scenarios are suspected",
    phases: [
        #{ title: "Map", detail: "code path for signal price, bar OHLC, and fill anchor" },
        #{ title: "Oracle", detail: "independent crossover recomputation vs strategy unit tests" },
        #{ title: "Backtest", detail: "run delay=0 vs delay=1 and synthetic vs CSV" },
        #{ title: "Diagnose", detail: "reconcile trades with open/close hypotheses" },
        #{ title: "Verify", detail: "adversarial check of top findings" },
    ],
};

// ---------------------------------------------------------------------------
// Schemas
// ---------------------------------------------------------------------------
let path_schema = #{
    "type": "object",
    "required": ["findings", "signal_price_field", "fill_anchor_default", "order_type_emitted"],
    "properties": #{
        "signal_price_field": #{ "type": "string" },
        "fill_anchor_default": #{ "type": "string" },
        "order_type_emitted": #{ "type": "string" },
        "limit_vs_next_open_risk": #{ "type": "string" },
        "ohlc_mapping_notes": #{ "type": "string" },
        "findings": #{
            "type": "array", "maxItems": 10,
            "items": #{
                "type": "object",
                "required": ["id", "area", "issue", "severity", "evidence"],
                "properties": #{
                    "id": #{ "type": "string" },
                    "area": #{ "type": "string" },
                    "issue": #{ "type": "string" },
                    "severity": #{ "type": "string" },
                    "evidence": #{ "type": "string" },
                },
            },
        },
    },
};

let oracle_schema = #{
    "type": "object",
    "required": ["warmup_ok", "golden_cross_ok", "death_cross_ok", "notes", "mismatches"],
    "properties": #{
        "warmup_ok": #{ "type": "boolean" },
        "golden_cross_ok": #{ "type": "boolean" },
        "death_cross_ok": #{ "type": "boolean" },
        "notes": #{ "type": "string" },
        "mismatches": #{
            "type": "array", "maxItems": 8,
            "items": #{
                "type": "object",
                "required": ["scenario", "expected", "observed", "evidence"],
                "properties": #{
                    "scenario": #{ "type": "string" },
                    "expected": #{ "type": "string" },
                    "observed": #{ "type": "string" },
                    "evidence": #{ "type": "string" },
                },
            },
        },
    },
};

let run_schema = #{
    "type": "object",
    "required": ["runs", "comparison_summary"],
    "properties": #{
        "comparison_summary": #{ "type": "string" },
        "runs": #{
            "type": "array", "maxItems": 8,
            "items": #{
                "type": "object",
                "required": ["label", "command", "exit_code", "n_trades", "notes"],
                "properties": #{
                    "label": #{ "type": "string" },
                    "command": #{ "type": "string" },
                    "exit_code": #{ "type": "integer" },
                    "n_trades": #{ "type": "integer" },
                    "first_fill_price": #{ "type": "number" },
                    "first_fill_ts_hint": #{ "type": "string" },
                    "artifact": #{ "type": "string" },
                    "notes": #{ "type": "string" },
                },
            },
        },
    },
};

let diagnose_schema = #{
    "type": "object",
    "required": ["root_causes", "open_close_verdict", "recommended_fixes"],
    "properties": #{
        "open_close_verdict": #{ "type": "string" },
        "root_causes": #{
            "type": "array", "maxItems": 8,
            "items": #{
                "type": "object",
                "required": ["id", "hypothesis", "supported", "evidence", "impact"],
                "properties": #{
                    "id": #{ "type": "string" },
                    "hypothesis": #{ "type": "string" },
                    "supported": #{ "type": "boolean" },
                    "evidence": #{ "type": "string" },
                    "impact": #{ "type": "string" },
                },
            },
        },
        "recommended_fixes": #{
            "type": "array", "maxItems": 6,
            "items": #{
                "type": "object",
                "required": ["priority", "change", "why"],
                "properties": #{
                    "priority": #{ "type": "string" },
                    "change": #{ "type": "string" },
                    "why": #{ "type": "string" },
                },
            },
        },
    },
};

let verdict_schema = #{
    "type": "object",
    "required": ["real", "reason", "evidence"],
    "properties": #{
        "real": #{ "type": "boolean" },
        "reason": #{ "type": "string" },
        "evidence": #{ "type": "string" },
    },
};

// ---------------------------------------------------------------------------
// Args
// ---------------------------------------------------------------------------
let core_root = if args == () { () } else { args.core_root };
if core_root == () { core_root = "/home/leonard/work/projects/truetest/core"; }

let seed = if args == () { () } else { args.seed };
if seed == () { seed = 424242; }

let strategy = if args == () { () } else { args.strategy };
if strategy == () { strategy = "ma-crossover"; }

let binary = if args == () { () } else { args.binary };
if binary == () { binary = core_root + "/build/engine_backtest"; }

let n_steps = if args == () { () } else { args.n_steps };
if n_steps == () { n_steps = 2000; }

let csv_path = if args == () { () } else { args.csv_path };
if csv_path == () { csv_path = core_root + "/tests/golden/sma_basic.csv"; }

let artifacts = if args == () { () } else { args.artifacts };
if artifacts == () { artifacts = "/tmp/tt-ma-crossover-audit"; }

log("ma-crossover-open-close: core=" + core_root + " strategy=" + strategy + " seed=" + seed.to_string());

// ===========================================================================
// Phase 1 — Map open/close code path (read-only, parallel lenses)
// ===========================================================================
phase("Map");

let map_jobs = [];

let p_strat = "";
p_strat += "Workspace root: " + core_root + ". READ-ONLY. ";
p_strat += "Audit ma-crossover / sma strategy open-close signal logic. ";
p_strat += "MUST read with read_file/grep (do not answer from memory): ";
p_strat += "src/strategy/ma_crossover_strategy.cpp, src/strategy/sma_strategy.cpp, ";
p_strat += "src/strategy/ma_crossover_strategy.h, tests/test_strategies.cpp (MACrossover* tests). ";
p_strat += "Determine: (1) which OHLC field feeds SMA update, (2) order_type and limit price field, ";
p_strat += "(3) how position_open gates re-entry, (4) whether signals can fire before fill lands under next-bar delay. ";
p_strat += "Return signal_price_field (e.g. close), fill_anchor_default as 'strategy_limit_at_close', ";
p_strat += "order_type_emitted, limit_vs_next_open_risk (short paragraph), ohlc_mapping_notes, ";
p_strat += "and findings[{id,area,issue,severity,evidence}] severity in {critical,high,medium,low}. ";
p_strat += "Empty findings only after reading the files.";
map_jobs.push(#{
    prompt: p_strat,
    label: "map:strategy",
    capability_mode: "read-only",
    output_schema: path_schema,
});

let p_eng = "";
p_eng += "Workspace root: " + core_root + ". READ-ONLY. ";
p_eng += "Audit engine bar-path open/close fill anchors for strategy orders. ";
p_eng += "MUST read: src/engine/engine.cpp (process_single_bar / history bar loop / route_order / execution_bar_delay), ";
p_eng += "src/engine/engine_config.h (execution_bar_delay default), tests/test_engine_lookahead.cpp, ";
p_eng += "docs/reference/04-flags.md (--exec-bar-delay). ";
p_eng += "Trace exact sequence per bar: set mid=open → drain pending → set mid=close → strategy on_market → park order. ";
p_eng += "Explain what price a LIMIT buy at bar-N close sees when drained at bar N+1 open (book replenish). ";
p_eng += "Return signal_price_field=close (if strategy uses close), fill_anchor_default describing delay=1 behavior, ";
p_eng += "order_type_emitted for engine path, limit_vs_next_open_risk, ohlc_mapping_notes, findings[].";
map_jobs.push(#{
    prompt: p_eng,
    label: "map:engine",
    capability_mode: "read-only",
    output_schema: path_schema,
});

let p_data = "";
p_data += "Workspace root: " + core_root + ". READ-ONLY. ";
p_data += "Audit whether open/close columns can be swapped or mis-mapped for local CSV and Binance kline. ";
p_data += "MUST read: src/providers/local/csv_parser.h, src/providers/data_bridge.h, ";
p_data += "src/providers/provider_convert.h, src/data/csv_data_source.cpp, ";
p_data += "tests/fixtures/binance_kline_sample.csv (header+2 rows), tests/fixtures/sample_ohlcv.csv, ";
p_data += "tests/test_binance_kline_csv.cpp if present. ";
p_data += "Confirm open/high/low/close field assignment and open_time timestamp path. ";
p_data += "Return findings if any column swap / volume scale / ts issues; else empty findings with evidence. ";
p_data += "signal_price_field=n/a, fill_anchor_default=n/a, order_type_emitted=n/a, ";
p_data += "limit_vs_next_open_risk=n/a, ohlc_mapping_notes=summary.";
map_jobs.push(#{
    prompt: p_data,
    label: "map:data",
    capability_mode: "read-only",
    output_schema: path_schema,
});

let map_results = parallel(map_jobs);

let all_findings = [];
let map_summaries = [];
for r in map_results {
    if r == () || !r.success || r.output == () {
        log("map agent failed or empty");
        continue;
    }
    let o = r.output;
    let sum = "";
    sum += "signal=" + o.signal_price_field + "; fill=" + o.fill_anchor_default;
    sum += "; order=" + o.order_type_emitted + "; risk=" + o.limit_vs_next_open_risk;
    map_summaries.push(sum);
    if o.findings != () {
        for f in o.findings { all_findings.push(f); }
    }
}
log("map findings so far: " + all_findings.len().to_string());

// ===========================================================================
// Phase 2 — Independent oracle / unit-test logic check
// ===========================================================================
phase("Oracle");

let p_oracle = "";
p_oracle += "Workspace: " + core_root + ". Capability: execute (may run tests). ";
p_oracle += "Independently verify ma-crossover signal math and unit tests. Steps: ";
p_oracle += "1) Read src/strategy/ma_crossover_strategy.cpp and tests/test_strategies.cpp MACrossover* cases. ";
p_oracle += "2) Hand-compute SMAs for the BuyOnCrossover and DeathCross price sequences; confirm expected sides. ";
p_oracle += "3) Run focused tests if binary exists: cd " + core_root + " && ";
p_oracle += "./build/truetest_tests --gtest_filter='MACrossoverStrategy.*:EngineLookahead.*' ";
p_oracle += "or ctest --test-dir build -R 'MACrossover|Lookahead' --output-on-failure. ";
p_oracle += "If binary missing, build with: cmake --build build -j --target truetest_tests (execute allowed). ";
p_oracle += "4) Explicitly check: does the strategy price LIMIT at close while engine fills at next open? ";
p_oracle += "Set warmup_ok/golden_cross_ok/death_cross_ok from evidence. mismatches[{scenario,expected,observed,evidence}] for any discrepancy. notes=summary.";
let oracle_r = agent(p_oracle, #{
    label: "oracle:signals",
    capability_mode: "execute",
    output_schema: oracle_schema,
});

let oracle_notes = "oracle unavailable";
if oracle_r != () && oracle_r.success && oracle_r.output != () {
    oracle_notes = oracle_r.output.notes;
    if oracle_r.output.mismatches != () {
        for m in oracle_r.output.mismatches {
            all_findings.push(#{
                id: "oracle-" + m.scenario,
                area: "oracle",
                issue: "mismatch expected=" + m.expected + " observed=" + m.observed,
                severity: "high",
                evidence: m.evidence,
            });
        }
    }
}

// ===========================================================================
// Phase 3 — Repeat backtests (delay 0 vs 1, synthetic + CSV)
// ===========================================================================
phase("Backtest");

let p_bt = "";
p_bt += "Workspace: " + core_root + ". EXECUTE backtests and compare open/close fill scenarios. ";
p_bt += "Binary: " + binary + ". Artifacts dir: " + artifacts + " (mkdir -p). ";
p_bt += "Always use headless flags: --no-pin --status-format off --no-tui --output-format json. ";
p_bt += "Seed: " + seed.to_string() + ". Strategy: " + strategy + ". ";
p_bt += "\n\nRun these four experiments (rebuild engine_backtest first if needed):\n";
p_bt += "A) synthetic delay=1 (default): ";
p_bt += binary + " --provider synthetic --strategy " + strategy;
p_bt += " --seed " + seed.to_string();
p_bt += " --mc-params symbol=BTCUSDT,initial_price=65000,n_steps=" + n_steps.to_string() + ",mu=0.0,sigma=0.65";
p_bt += " --exec-bar-delay 1 --balance 100000 --no-pin --status-format off --no-tui";
p_bt += " --output " + artifacts + "/syn_delay1.json\n";
p_bt += "B) synthetic delay=0: same as A but --exec-bar-delay 0 --output " + artifacts + "/syn_delay0.json\n";
p_bt += "C) local CSV delay=1: ";
p_bt += binary + " --provider local --path " + csv_path + " --strategy " + strategy;
p_bt += " --seed " + seed.to_string() + " --exec-bar-delay 1 --balance 100000";
p_bt += " --no-pin --status-format off --no-tui --output " + artifacts + "/csv_delay1.json\n";
p_bt += "D) local CSV delay=0: same as C with --exec-bar-delay 0 --output " + artifacts + "/csv_delay0.json\n";
p_bt += "\nFor each run record: label, full command, exit_code, n_trades (from JSON trades[] or summary), ";
p_bt += "first_fill_price if present, first_fill_ts_hint, artifact path, notes. ";
p_bt += "Use jq if available: '.trades | length', first trade fill_price/price fields. ";
p_bt += "comparison_summary must explain whether delay0 vs delay1 trade counts/prices differ in the way ";
p_bt += "expected for close-signal vs next-open-fill (and whether LIMIT@close fails to fill after gaps). ";
p_bt += "Also run sma strategy once on synthetic delay=1 for baseline contrast if time allows (optional 5th run).";

let bt_r = agent(p_bt, #{
    label: "backtest:matrix",
    capability_mode: "execute",
    output_schema: run_schema,
});

let bt_summary = "backtests unavailable";
let runs = [];
if bt_r != () && bt_r.success && bt_r.output != () {
    bt_summary = bt_r.output.comparison_summary;
    if bt_r.output.runs != () { runs = bt_r.output.runs; }
}
log("backtest summary: " + bt_summary);

// ===========================================================================
// Phase 4 — Diagnose open/close hypotheses against evidence
// ===========================================================================
phase("Diagnose");

let findings_json = json_encode(all_findings);
let runs_json = json_encode(runs);
let map_json = json_encode(map_summaries);

let p_diag = "";
p_diag += "You are diagnosing why MA-crossover backtests may look wrong due to open/close scenarios. ";
p_diag += "Workspace: " + core_root + ". READ-ONLY except you may re-read artifacts under " + artifacts + ". ";
p_diag += "\nCode-map summaries: " + map_json;
p_diag += "\nPrior findings JSON: " + findings_json;
p_diag += "\nOracle notes: " + oracle_notes;
p_diag += "\nBacktest comparison: " + bt_summary;
p_diag += "\nRuns JSON: " + runs_json;
p_diag += "\n\nEvaluate these hypotheses with supported true/false + evidence:\n";
p_diag += "H1: Strategy updates SMA on close and emits LIMIT priced at same-bar close (looks correct for EOD signal).\n";
p_diag += "H2: Default execution_bar_delay=1 parks order to next bar; book reseeded at OPEN → fill tracks next open, not signal close.\n";
p_diag += "H3: LIMIT@previous_close may miss fill when next open gaps through the limit (buy gap-up / sell gap-down).\n";
p_diag += "H4: CSV/kline open/close columns are swapped or mis-mapped (falsify with fixture headers).\n";
p_diag += "H5: position_open not updated until fill → strategy may re-signal or skip incorrectly across delay bars.\n";
p_diag += "H6: Using market orders at next open would match classical 'signal on close, trade next open' better than limit@close.\n";
p_diag += "Return open_close_verdict (one paragraph), root_causes[{id,hypothesis,supported,evidence,impact}], ";
p_diag += "recommended_fixes[{priority,change,why}]. Prefer concrete code-level fixes over vague advice. ";
p_diag += "Do NOT edit freeze files; only recommend.";

let diag_r = agent(p_diag, #{
    label: "diagnose:hypotheses",
    capability_mode: "read-only",
    output_schema: diagnose_schema,
});

let root_causes = [];
let fixes = [];
let verdict = "diagnosis unavailable";
if diag_r != () && diag_r.success && diag_r.output != () {
    verdict = diag_r.output.open_close_verdict;
    if diag_r.output.root_causes != () { root_causes = diag_r.output.root_causes; }
    if diag_r.output.recommended_fixes != () { fixes = diag_r.output.recommended_fixes; }
}

// ===========================================================================
// Phase 5 — Adversarial verify top supported causes
// ===========================================================================
phase("Verify");

let to_verify = [];
for c in root_causes {
    if c.supported == true {
        to_verify.push(c);
    }
}
// Cap verification panel
let max_v = 4;
if to_verify.len() > max_v {
    log("capping verification from " + to_verify.len().to_string() + " to " + max_v.to_string());
    let trimmed = [];
    let i = 0;
    for c in to_verify {
        if i < max_v { trimmed.push(c); }
        i += 1;
    }
    to_verify = trimmed;
}

let confirmed = [];
if to_verify.len() == 0 {
    log("no supported root causes to verify — verifying absence of H4 (OHLC swap) as negative control");
    let p_neg = "";
    p_neg += "Adversarially check claim: 'CSV open/close columns are correctly mapped (not swapped)'. ";
    p_neg += "Workspace " + core_root + ". Read csv_parser.h field assignment and binance_kline_sample.csv header. ";
    p_neg += "Set real=true only if mapping is correct with line-level evidence; else real=false.";
    let vr = agent(p_neg, #{
        label: "verify:h4-neg",
        capability_mode: "read-only",
        output_schema: verdict_schema,
    });
    if vr != () && vr.success && vr.output != () && vr.output.real == true {
        confirmed.push(#{
            id: "H4-neg",
            hypothesis: "OHLC columns correctly mapped (no swap)",
            evidence: vr.output.evidence,
        });
    }
} else {
    let vjobs = [];
    for c in to_verify {
        let pp = "";
        pp += "Adversarially verify this diagnosis (try to REFUTE it). Workspace: " + core_root + ". ";
        pp += "Hypothesis: " + c.hypothesis + " | Prior evidence: " + c.evidence + " | Impact: " + c.impact + ". ";
        pp += "Independently re-read the cited source files / artifacts. ";
        pp += "Set real=true ONLY with concrete code or run evidence you inspected; default real=false if uncertain.";
        vjobs.push(#{
            prompt: pp,
            label: "verify:" + c.id,
            capability_mode: "read-only",
            output_schema: verdict_schema,
        });
    }
    let vresults = parallel(vjobs);
    let i = 0;
    for v in vresults {
        if v != () && v.success && v.output != () && v.output.real == true
            && v.output.evidence != () && v.output.evidence != "" {
            confirmed.push(#{
                id: to_verify[i].id,
                hypothesis: to_verify[i].hypothesis,
                evidence: v.output.evidence,
                reason: v.output.reason,
            });
        }
        i += 1;
    }
}

log("confirmed causes: " + confirmed.len().to_string() + "/" + to_verify.len().to_string());

// ===========================================================================
// Report
// ===========================================================================
let report = "";
report += "# MA-Crossover Open/Close Backtest Audit\n\n";
report += "Strategy: `" + strategy + "`  \n";
report += "Seed: " + seed.to_string() + "  \n";
report += "CSV: `" + csv_path + "`  \n";
report += "Artifacts: `" + artifacts + "`\n\n";

report += "## Verdict\n\n";
report += verdict + "\n\n";

report += "## Confirmed root causes (" + confirmed.len().to_string() + ")\n\n";
if confirmed.len() == 0 {
    report += "_None survived adversarial verification._\n\n";
} else {
    for c in confirmed {
        report += "### " + c.id + "\n";
        report += "- **Hypothesis:** " + c.hypothesis + "\n";
        report += "- **Evidence:** " + c.evidence + "\n";
        if c.reason != () { report += "- **Verifier:** " + c.reason + "\n"; }
        report += "\n";
    }
}

report += "## Recommended fixes\n\n";
if fixes.len() == 0 {
    report += "_None proposed._\n\n";
} else {
    for f in fixes {
        report += "- **" + f.priority + "**: " + f.change + " — " + f.why + "\n";
    }
    report += "\n";
}

report += "## Backtest comparison\n\n";
report += bt_summary + "\n\n";
if runs.len() > 0 {
    report += "| Label | Exit | Trades | First fill | Notes |\n";
    report += "|-------|------|--------|------------|-------|\n";
    for r in runs {
        let fp = if r.first_fill_price != () { r.first_fill_price.to_string() } else { "-" };
        report += "| " + r.label + " | " + r.exit_code.to_string() + " | " + r.n_trades.to_string();
        report += " | " + fp + " | " + r.notes + " |\n";
    }
    report += "\n";
}

report += "## Oracle\n\n";
report += oracle_notes + "\n\n";

report += "## Map findings (pre-verify)\n\n";
if all_findings.len() == 0 {
    report += "_None._\n";
} else {
    for f in all_findings {
        report += "- **[" + f.severity + "]** `" + f.area + "`: " + f.issue + " — " + f.evidence + "\n";
    }
}

let path = write_scratch_file("ma-crossover-open-close-report.md", report);

complete(#{
    path: path,
    summary: "MA-crossover open/close audit: " + confirmed.len().to_string() + " confirmed causes",
    open_close_verdict: verdict,
    confirmed: confirmed,
    recommended_fixes: fixes,
    backtest_summary: bt_summary,
    n_map_findings: all_findings.len(),
});
