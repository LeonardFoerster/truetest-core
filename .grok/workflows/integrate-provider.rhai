// integrate-provider — phased multi-venue provider integration for TrueTest core.
//
// Orchestrates scaffold → market data → REST/instrument → safety → execution →
// verify for one venue from docs/upcoming_platform/<venue>.md, using Bitget as
// the modern golden reference and Binance futures as the secondary golden path.
//
// PARALLEL SAFETY: other agents may implement other venues on the same branch.
// Every implementer MUST only create/edit:
//   - src/providers/<codepath>/
//   - tests/providers/<codepath>/
//   - tests/fixtures/<codepath>/ (optional)
//   - CMakeLists.txt / cmake/Dependencies.cmake / cmake/Sources.cmake ONLY the
//     ENABLE_<FLAG> / HAS_<FLAG> / test list lines for THIS venue (append-only)
//   - CLI (main.inc) ONLY additive helpers for this venue (prefer provider_config)
// NEVER touch: other venues' trees, freeze-list Binance/Bitget safety files,
// engine.cpp, risk/threading core, or live_safety.h interfaces.
//
// Args:
//   venue   — required: "gate" | "bybit" | "okx" | "kraken" (or full name)
//   max_phase — optional int 0..4 (default 4). Stop after this phase DoD.
//   verify  — optional bool (default true). Run build/tests/gate scripts.

let meta = #{
    name: "integrate-provider",
    description: "Integrate a new CEX futures provider into TrueTest core from upcoming_platform guides",
    when_to_use: "When adding gate/bybit/okx/kraken (or similar) as IProvider with phased safety stack",
    phases: [
        #{ title: "Resolve", detail: "map venue args → guide path, codepath, flag, registry names" },
        #{ title: "Inventory", detail: "read guide + golden Bitget/Binance surfaces" },
        #{ title: "Phase0", detail: "scaffold endpoints, auth, registry stub, CMake, unit tests" },
        #{ title: "Phase1", detail: "public market WS + parsers + paper/shadow open path" },
        #{ title: "Phase2", detail: "REST client, time sync, instrument/contract probe" },
        #{ title: "Phase3", detail: "reconciler, kill-switch, DMS, dual-mode refuse" },
        #{ title: "Phase4", detail: "order encoder, order transport, user-data, ExecutionBridge" },
        #{ title: "Verify", detail: "build, ctest venue, three gate scripts" },
        #{ title: "Report", detail: "structured DoD report" },
    ],
};

let inventory_schema = #{
    "type": "object",
    "required": ["venue", "codepath", "flag", "has_macro", "registry_names", "guide_path", "phase_plan", "golden_files", "risks"],
    "properties": #{
        "venue": #{ "type": "string" },
        "codepath": #{ "type": "string" },
        "flag": #{ "type": "string" },
        "has_macro": #{ "type": "string" },
        "registry_names": #{ "type": "array", "items": #{ "type": "string" }, "maxItems": 4 },
        "guide_path": #{ "type": "string" },
        "phase_plan": #{ "type": "array", "maxItems": 6, "items": #{
            "type": "object",
            "required": ["phase", "deliverables", "test_files"],
            "properties": #{
                "phase": #{ "type": "integer" },
                "deliverables": #{ "type": "string" },
                "test_files": #{ "type": "array", "items": #{ "type": "string" }, "maxItems": 20 },
            },
        }},
        "golden_files": #{ "type": "array", "items": #{ "type": "string" }, "maxItems": 30 },
        "risks": #{ "type": "array", "items": #{ "type": "string" }, "maxItems": 12 },
        "notes": #{ "type": "string" },
    },
};

let phase_result_schema = #{
    "type": "object",
    "required": ["phase", "success", "files_written", "summary", "blockers"],
    "properties": #{
        "phase": #{ "type": "integer" },
        "success": #{ "type": "boolean" },
        "files_written": #{ "type": "array", "items": #{ "type": "string" }, "maxItems": 40 },
        "summary": #{ "type": "string" },
        "blockers": #{ "type": "array", "items": #{ "type": "string" }, "maxItems": 12 },
        "tests_added": #{ "type": "array", "items": #{ "type": "string" }, "maxItems": 20 },
        "commands_run": #{ "type": "array", "items": #{ "type": "string" }, "maxItems": 12 },
    },
};

let verify_schema = #{
    "type": "object",
    "required": ["build_ok", "tests_ok", "hotpath_ok", "layers_ok", "freeze_ok", "summary", "failures"],
    "properties": #{
        "build_ok": #{ "type": "boolean" },
        "tests_ok": #{ "type": "boolean" },
        "hotpath_ok": #{ "type": "boolean" },
        "layers_ok": #{ "type": "boolean" },
        "freeze_ok": #{ "type": "boolean" },
        "summary": #{ "type": "string" },
        "failures": #{ "type": "array", "items": #{ "type": "string" }, "maxItems": 20 },
        "test_filter": #{ "type": "string" },
        "ctest_output_tail": #{ "type": "string" },
    },
};

// --- args -------------------------------------------------------------------

let venue_raw = if args == () { () } else { args.venue };
if venue_raw == () {
    pause("verification", "Pass args.venue — one of: gate, bybit, okx, kraken (or gateio).");
}

let max_phase = 4;
if args != () && args.max_phase != () {
    max_phase = args.max_phase;
}
if max_phase < 0 { max_phase = 0; }
if max_phase > 4 { max_phase = 4; }

let do_verify = true;
if args != () && args.verify != () {
    do_verify = args.verify;
}

// Normalize aliases
let venue = venue_raw;
if venue == "gateio" || venue == "gate.io" || venue == "gate-futures" {
    venue = "gate";
}
if venue == "bitget" {
    pause("verification", "Bitget is already landed under src/providers/bitget/. Pick another venue.");
}

// Canonical map (script-enforced; agents re-confirm from guide)
let codepath = venue;
let flag = "";
let has_macro = "";
let guide = "";
let reg_names = "";
let test_filter = venue;

if venue == "gate" {
    codepath = "gate";
    flag = "ENABLE_GATE";
    has_macro = "HAS_GATE";
    guide = "docs/upcoming_platform/gateio.md";
    reg_names = "gate-futures, gate";
    test_filter = "gate|Gate";
} else if venue == "bybit" {
    codepath = "bybit";
    flag = "ENABLE_BYBIT";
    has_macro = "HAS_BYBIT";
    guide = "docs/upcoming_platform/bybit.md";
    reg_names = "bybit-futures, bybit";
    test_filter = "bybit|Bybit";
} else if venue == "okx" {
    codepath = "okx";
    flag = "ENABLE_OKX";
    has_macro = "HAS_OKX";
    guide = "docs/upcoming_platform/okx.md";
    reg_names = "okx-futures, okx";
    test_filter = "okx|Okx|OKX";
} else if venue == "kraken" {
    codepath = "kraken";
    flag = "ENABLE_KRAKEN";
    has_macro = "HAS_KRAKEN";
    guide = "docs/upcoming_platform/kraken.md";
    reg_names = "kraken-futures, kraken";
    test_filter = "kraken|Kraken";
} else {
    pause("verification", "Unknown venue '" + venue + "'. Use gate|bybit|okx|kraken.");
}

let shared_constraints = "";
shared_constraints += "PARALLEL-AGENT RULES (mandatory):\n";
shared_constraints += "- ONLY edit src/providers/" + codepath + "/, tests/providers/" + codepath + "/, fixtures for this venue.\n";
shared_constraints += "- CMake: append-only ENABLE_" + venue + " / " + has_macro + " blocks; do not rewrite other venues' blocks.\n";
shared_constraints += "- NEVER edit src/providers/bitget/, binance freeze files, engine.cpp, live_safety.h, risk_manager.h.\n";
shared_constraints += "- No nlohmann/json on hot path. All headers wrap in #ifdef " + has_macro + ".\n";
shared_constraints += "- After any src/ edit: ./scripts/check-hotpath-json.sh && ./scripts/check-layer-deps.sh && ./scripts/check-live-safety-freeze.sh\n";
shared_constraints += "- Golden references: src/providers/bitget/* (modern) and src/providers/binance/*futures* (safety).\n";
shared_constraints += "- Registry names: " + reg_names + ". Flag: " + flag + " → " + has_macro + ".\n";
shared_constraints += "- Workspace root is the TrueTest core/ repo. Use relative paths from core/.\n";
shared_constraints += "CRITICAL WORKTREE: All file reads/writes MUST use absolute paths under\n";
shared_constraints += "/home/leonard/work/projects/truetest/core-provider-gate/\n";
shared_constraints += "That checkout is branch `provider` with Bitget golden + Bybit/Gate Phase 0 already landed.\n";
shared_constraints += "Do NOT edit /home/leonard/work/projects/truetest/core/ (master) for this work.\n";
shared_constraints += "Phase 0 is ALREADY COMPLETE for bybit and gate — verify and skip re-scaffolding; implement max_phase remaining phases.\n";
shared_constraints += "Build dir: /home/leonard/work/projects/truetest/core-provider-gate/out/build/linux-bybit-gate (ENABLE_BYBIT+ENABLE_GATE already configured) or create out/build/linux-" + venue + ".\n";


// =============================================================================
phase("Resolve");
// =============================================================================
log("Integrating venue=" + venue + " codepath=" + codepath + " max_phase=" + max_phase.to_string());

// =============================================================================
phase("Inventory");
// =============================================================================

let inv_prompt = "";
inv_prompt += "You are inventorying TrueTest core for a new futures provider integration.\n";
inv_prompt += "Venue: " + venue + " | Guide: " + guide + " | Codepath: src/providers/" + codepath + "/\n";
inv_prompt += shared_constraints;
inv_prompt += "\nREQUIRED ACTIONS (use tools — do not invent):\n";
inv_prompt += "1. Read " + guide + " fully (or enough to extract phase plan).\n";
inv_prompt += "2. Read docs/upcoming_platform/README.md status table.\n";
inv_prompt += "3. Inspect existing golden files: bitget_endpoints/auth/register/provider open(),\n";
inv_prompt += "   binance_futures_dead_mans_switch, binance_futures_kill_switch, binance_futures_reconciler,\n";
inv_prompt += "   cmake/Dependencies.cmake ENABLE_BITGET block, cmake/Sources.cmake bitget tests, CMakeLists options.\n";
inv_prompt += "4. Confirm src/providers/" + codepath + "/ does NOT already exist (or note partial work).\n";
inv_prompt += "5. Produce phase_plan phases 0.." + max_phase.to_string() + " with concrete deliverable files and test files from the guide.\n";
inv_prompt += "Return structured inventory only. Do not write code yet.\n";

let inv = agent(inv_prompt, #{
    label: "inventory:" + venue,
    capability_mode: "read-only",
    output_schema: inventory_schema,
});

if inv == () || inv.success != true || inv.output == () {
    complete(#{
        ok: false,
        venue: venue,
        error: "inventory agent failed",
    });
}

let inv_json = json_encode(inv.output);
log("Inventory complete for " + venue);

// Helper to build phase implementer prompt
fn phase_prompt(phase_n, venue, codepath, flag, has_macro, guide, shared_constraints, inv_json, focus) {
    let p = "";
    p += "Implement Phase " + phase_n.to_string() + " of the " + venue + " futures provider for TrueTest core.\n";
    p += shared_constraints;
    p += "\nGuide: " + guide + "\n";
    p += "Inventory (JSON, trust as plan but re-read guide for wire formats):\n" + inv_json + "\n";
    p += "\nFOCUS THIS PHASE:\n" + focus + "\n";
    p += "\nIMPLEMENTATION RULES:\n";
    p += "- Write real compilable C++23 header-only code (register .cpp only where needed).\n";
    p += "- Mirror Bitget structure under src/providers/" + codepath + "/.\n";
    p += "- Add matching gtests under tests/providers/" + codepath + "/ with #ifdef " + has_macro + ".\n";
    p += "- Wire " + flag + " in CMakeLists.txt option, Dependencies.cmake target_sources register.cpp,\n";
    p += "  Sources.cmake TEST_SOURCES list (append gate/venue tests before CLI section).\n";
    p += "- Prefer pure unit-testable helpers + fake REST post_fn patterns from Bitget/Binance tests.\n";
    p += "- Do NOT require network for unit tests. Env-gated live tests optional and skipped by default.\n";
    p += "- If a prior phase file is missing, implement the minimal prerequisite first.\n";
    p += "- After writing, try: cmake -B build-gate -D" + flag + "=ON -DBUILD_TESTS=ON -DENABLE_BINANCE=ON (if exists) or reconfigure existing build dir.\n";
    p += "  Prefer out/build or build-gate dedicated dir to reduce conflict with other agents.\n";
    p += "- Run focused ctest -R for this venue if build works; fix compile errors before finishing.\n";
    p += "- Return files_written, tests_added, success=true only if code compiles or you document exact blockers.\n";
    p += "Use write/search_replace tools. Be thorough but stay in-scope for this phase.\n";
    return p;
}

let results = [];
let any_fail = false;

// =============================================================================
// Phase 0 — always
// =============================================================================
phase("Phase0");

let p0_focus = "";
p0_focus += "Phase 0 Scaffold (offline unit tests):\n";
p0_focus += "1. " + codepath + "_endpoints.h — mainnet/testnet hosts, settle paths, path helpers\n";
p0_focus += "2. " + codepath + "_auth.h — venue-correct HMAC (Gate: SHA512 hex REST + WS sign)\n";
p0_focus += "3. Minimal parser/json util if needed for later (can be stub)\n";
p0_focus += "4. " + codepath + "_futures_provider.h — stub IProvider: name, has_data/exec, open/close stubs or minimal paper path\n";
p0_focus += "5. " + codepath + "_futures_register.cpp — register both names; require symbol\n";
p0_focus += "6. CMake ENABLE + HAS + Sources tests\n";
p0_focus += "7. tests: auth golden vectors, endpoints, register\n";
p0_focus += "For Gate specifically: SHA512 not SHA256; timestamp seconds; empty body SHA512 known digest;\n";
p0_focus += "symbol BTC_USDT underscore; dual_mode refuse deferred to later phase.\n";

let p0 = agent(
    phase_prompt(0, venue, codepath, flag, has_macro, guide, shared_constraints, inv_json, p0_focus),
    #{
        label: "phase0:" + venue,
        capability_mode: "all",
        output_schema: phase_result_schema,
    }
);

if p0 == () || p0.success != true || p0.output == () || p0.output.success != true {
    any_fail = true;
    log("Phase 0 failed or incomplete");
    if max_phase == 0 {
        // fall through to report
    } else {
        // Continue only if partial files may exist — still try later phases carefully
        log("Continuing despite Phase 0 issues (later agents may repair scaffold)");
    }
}
if p0 != () && p0.success == true && p0.output != () {
    results.push(p0.output);
}

// =============================================================================
if max_phase >= 1 {
    phase("Phase1");
    let p1_focus = "";
    p1_focus += "Phase 1 Market Data (shadow/paper):\n";
    p1_focus += "- Public WS transport + combined transport (trades + order book update)\n";
    p1_focus += "- Hand-rolled combined parser → provider::event (tick, l2_*)\n";
    p1_focus += "- Optional backfill candlesticks + PrependTransport\n";
    p1_focus += "- Provider open() non-live: HybridExecutor or TradeTapeShadowAdapter + transport open\n";
    p1_focus += "- Parser unit tests with fixtures (no network)\n";
    p1_focus += "Gate: futures.trades, futures.order_book_update, signed size, is_internal filter.\n";
    p1_focus += "Copy WS patterns from bitget_transport.h / binance_combined_transport.h (Boost.Beast).\n";

    let p1 = agent(
        phase_prompt(1, venue, codepath, flag, has_macro, guide, shared_constraints, inv_json, p1_focus),
        #{
            label: "phase1:" + venue,
            capability_mode: "all",
            output_schema: phase_result_schema,
        }
    );
    if p1 == () || p1.success != true || p1.output == () || p1.output.success != true {
        any_fail = true;
        log("Phase 1 failed or incomplete");
    }
    if p1 != () && p1.success == true && p1.output != () {
        results.push(p1.output);
    }
}

// =============================================================================
if max_phase >= 2 {
    phase("Phase2");
    let p2_focus = "";
    p2_focus += "Phase 2 REST + instrument:\n";
    p2_focus += "- RestClient (TLS, signed/unsigned, clock offset)\n";
    p2_focus += "- time_sync skew refuse helpers\n";
    p2_focus += "- Contract/instrument probe → instrument_spec (tick, lot, min_qty, quanto)\n";
    p2_focus += "- Unit tests for time parse / path signing helpers without network where possible\n";
    p2_focus += "Gate: GET /spot/time ms; contracts BTC_USDT; HMAC path includes /api/v4.\n";

    let p2 = agent(
        phase_prompt(2, venue, codepath, flag, has_macro, guide, shared_constraints, inv_json, p2_focus),
        #{
            label: "phase2:" + venue,
            capability_mode: "all",
            output_schema: phase_result_schema,
        }
    );
    if p2 == () || p2.success != true || p2.output == () || p2.output.success != true {
        any_fail = true;
        log("Phase 2 failed or incomplete");
    }
    if p2 != () && p2.success == true && p2.output != () {
        results.push(p2.output);
    }
}

// =============================================================================
if max_phase >= 3 {
    phase("Phase3");
    // Safety stack: three parallel specialists that touch disjoint files
    let s_jobs = [];

    let rec_focus = "";
    rec_focus += "Implement ONLY reconciler + unit tests for " + venue + ".\n";
    rec_focus += shared_constraints;
    rec_focus += "Guide: " + guide + "\nInventory: " + inv_json + "\n";
    rec_focus += "File: src/providers/" + codepath + "/" + codepath + "_futures_reconciler.h\n";
    rec_focus += "Test: tests/providers/" + codepath + "/test_" + codepath + "_futures_reconciler.cpp\n";
    rec_focus += "Mirror bitget_futures_reconciler.h: default-refuse on drift/network fail; canned JSON tests.\n";
    rec_focus += "Also add dual_mode/safety helpers if missing (gate_futures_safety.h).\n";
    rec_focus += "Wire test into Sources.cmake if not present. Build and run focused test if possible.\n";
    rec_focus += "Return phase=3 success/files_written schema.\n";

    s_jobs.push(#{
        prompt: rec_focus,
        label: "phase3-reconciler:" + venue,
        capability_mode: "all",
        output_schema: phase_result_schema,
    });

    let kill_focus = "";
    kill_focus += "Implement ONLY kill-switch + unit tests for " + venue + ".\n";
    kill_focus += shared_constraints;
    kill_focus += "Guide: " + guide + "\nInventory: " + inv_json + "\n";
    kill_focus += "File: src/providers/" + codepath + "/" + codepath + "_futures_kill_switch.h\n";
    kill_focus += "Test: tests/providers/" + codepath + "/test_" + codepath + "_futures_kill_switch.cpp\n";
    kill_focus += "Loud, non-retrying, fail-closed. Fake REST sequence tests like Bitget/Binance.\n";
    kill_focus += "Gate: DELETE orders?contract= then reduce-only market opposite signed size.\n";
    kill_focus += "Wire test into Sources.cmake if needed. Build/run focused test if possible.\n";
    kill_focus += "Return phase=3 success/files_written schema.\n";

    s_jobs.push(#{
        prompt: kill_focus,
        label: "phase3-kill:" + venue,
        capability_mode: "all",
        output_schema: phase_result_schema,
    });

    let dms_focus = "";
    dms_focus += "Implement ONLY dead-man's switch + unit tests for " + venue + ".\n";
    dms_focus += shared_constraints;
    dms_focus += "Guide: " + guide + "\nInventory: " + inv_json + "\n";
    dms_focus += "File: src/providers/" + codepath + "/" + codepath + "_futures_dead_mans_switch.h\n";
    dms_focus += "Test: tests/providers/" + codepath + "/test_" + codepath + "_futures_dead_mans_switch.cpp\n";
    dms_focus += "Fixed conservative timeout; arm fail refuses live; no adaptive lengthening.\n";
    dms_focus += "Gate: countdown_cancel_all timeout in SECONDS min 5; CLI ms → max(5, ms/1000).\n";
    dms_focus += "Wire test into Sources.cmake if needed. Build/run focused test if possible.\n";
    dms_focus += "Return phase=3 success/files_written schema.\n";

    s_jobs.push(#{
        prompt: dms_focus,
        label: "phase3-dms:" + venue,
        capability_mode: "all",
        output_schema: phase_result_schema,
    });

    let s_results = parallel(s_jobs);
    for sr in s_results {
        if sr == () || sr.success != true || sr.output == () || sr.output.success != true {
            any_fail = true;
            log("Phase 3 parallel piece failed");
        }
        if sr != () && sr.success == true && sr.output != () {
            results.push(sr.output);
        }
    }

    // Wire safety into provider open() live path
    let wire3 = "";
    wire3 += "Wire Phase 3 safety components into " + venue + " provider open()/close()/get_* hooks.\n";
    wire3 += shared_constraints;
    wire3 += "Guide open() checklist. Read existing " + codepath + "_futures_provider.h and Bitget open_live_path.\n";
    wire3 += "Ensure get_reconciler/get_kill_switch/get_liveness_sources work; dual_mode refuse on live open.\n";
    wire3 += "Do not implement full ExecutionBridge yet if Phase 4 not started — but live open should refuse cleanly\n";
    wire3 += "when keys present without complete bridge OR complete the refuse paths only.\n";
    wire3 += "If max work allows, stub live refuse with clear error until Phase 4 lands.\n";
    wire3 += "Return phase=3 success schema.\n";

    let p3w = agent(wire3, #{
        label: "phase3-wire:" + venue,
        capability_mode: "all",
        output_schema: phase_result_schema,
    });
    if p3w == () || p3w.success != true || p3w.output == () || p3w.output.success != true {
        any_fail = true;
        log("Phase 3 wire failed");
    }
    if p3w != () && p3w.success == true && p3w.output != () {
        results.push(p3w.output);
    }
}

// =============================================================================
if max_phase >= 4 {
    phase("Phase4");
    let p4_focus = "";
    p4_focus += "Phase 4 Execution Bridge live path:\n";
    p4_focus += "- Order encoder (Gate: JSON body, signed size, text t- prefix, market price 0 ioc)\n";
    p4_focus += "- REST order transport place/cancel\n";
    p4_focus += "- User-data private WS transport + fill parser\n";
    p4_focus += "- Wire ExecutionBridge in provider open_live_path like Bitget/Binance futures\n";
    p4_focus += "- Optional bracket adapter stub if guide says phase live+\n";
    p4_focus += "- Unit tests: encoder, user_data_parser, rest_order_transport mapping\n";
    p4_focus += "- Rate limiter conservative; ClientOrderId minter with venue constraints\n";
    p4_focus += "CLI: only additive provider_config keys if absolutely needed; prefer registry factory.\n";

    let p4 = agent(
        phase_prompt(4, venue, codepath, flag, has_macro, guide, shared_constraints, inv_json, p4_focus),
        #{
            label: "phase4:" + venue,
            capability_mode: "all",
            output_schema: phase_result_schema,
        }
    );
    if p4 == () || p4.success != true || p4.output == () || p4.output.success != true {
        any_fail = true;
        log("Phase 4 failed or incomplete");
    }
    if p4 != () && p4.success == true && p4.output != () {
        results.push(p4.output);
    }
}

// =============================================================================
let verify_out = #{
    build_ok: false,
    tests_ok: false,
    hotpath_ok: false,
    layers_ok: false,
    freeze_ok: false,
    summary: "verify skipped",
    failures: [],
    test_filter: test_filter,
    ctest_output_tail: "",
};

if do_verify {
    phase("Verify");
    let v_prompt = "";
    v_prompt += "Verify the " + venue + " provider integration end-to-end in TrueTest core.\n";
    v_prompt += shared_constraints;
    v_prompt += "\n1. Configure/build with -D" + flag + "=ON -DBUILD_TESTS=ON (use build-gate or out/build/linux-tests-gate).\n";
    v_prompt += "2. ctest --test-dir <build> -R '" + test_filter + "' --output-on-failure\n";
    v_prompt += "3. ./scripts/check-hotpath-json.sh\n";
    v_prompt += "4. ./scripts/check-layer-deps.sh\n";
    v_prompt += "5. ./scripts/check-live-safety-freeze.sh\n";
    v_prompt += "If tests fail with fixable compile/link errors in " + codepath + " only, fix them and re-run.\n";
    v_prompt += "Do not touch other venues. Report honest pass/fail booleans.\n";
    v_prompt += "Include brief ctest tail in ctest_output_tail.\n";

    let v = agent(v_prompt, #{
        label: "verify:" + venue,
        capability_mode: "all",
        output_schema: verify_schema,
    });
    if v != () && v.success == true && v.output != () {
        if v.output.build_ok == true { verify_out.build_ok = true; }
        if v.output.tests_ok == true { verify_out.tests_ok = true; }
        if v.output.hotpath_ok == true { verify_out.hotpath_ok = true; }
        if v.output.layers_ok == true { verify_out.layers_ok = true; }
        if v.output.freeze_ok == true { verify_out.freeze_ok = true; }
        if v.output.summary != () { verify_out.summary = v.output.summary; }
        if v.output.failures != () { verify_out.failures = v.output.failures; }
        if v.output.test_filter != () { verify_out.test_filter = v.output.test_filter; }
        if v.output.ctest_output_tail != () {
            verify_out.ctest_output_tail = v.output.ctest_output_tail;
        }
    } else {
        verify_out.summary = "verify agent failed";
        verify_out.failures = ["verify agent failed or returned invalid schema"];
        any_fail = true;
    }
    if verify_out.build_ok != true || verify_out.tests_ok != true || verify_out.hotpath_ok != true || verify_out.layers_ok != true || verify_out.freeze_ok != true {
        any_fail = true;
    }
}

// =============================================================================
phase("Report");

let report = "";
report += "# Provider integration report: " + venue + "\n\n";
report += "- codepath: `src/providers/" + codepath + "/`\n";
report += "- flag: `" + flag + "` / `" + has_macro + "`\n";
report += "- registry: " + reg_names + "\n";
report += "- max_phase: " + max_phase.to_string() + "\n";
report += "- any_phase_failure: " + if any_fail == true { "true" } else { "false" } + "\n\n";
report += "## Inventory notes\n\n";
if inv.output.notes != () {
    report += inv.output.notes + "\n\n";
}
report += "## Phase results\n\n";
for r in results {
    let ph = if r.phase == () { -1 } else { r.phase };
    let sum = if r.summary == () { "(no summary)" } else { r.summary };
    report += "### Phase " + ph.to_string() + " — " + if r.success == true { "OK" } else { "FAIL" } + "\n";
    report += sum + "\n";
    if r.files_written != () {
        report += "Files:\n";
        for f in r.files_written {
            report += "- " + f + "\n";
        }
    }
    if r.blockers != () {
        for b in r.blockers {
            report += "- blocker: " + b + "\n";
        }
    }
    report += "\n";
}
report += "## Verify\n\n";
report += "- build_ok: " + if verify_out.build_ok == true { "true" } else { "false" } + "\n";
report += "- tests_ok: " + if verify_out.tests_ok == true { "true" } else { "false" } + "\n";
report += "- hotpath_ok: " + if verify_out.hotpath_ok == true { "true" } else { "false" } + "\n";
report += "- layers_ok: " + if verify_out.layers_ok == true { "true" } else { "false" } + "\n";
report += "- freeze_ok: " + if verify_out.freeze_ok == true { "true" } else { "false" } + "\n";
let vsum = if verify_out.summary == () { "(no verify summary)" } else { verify_out.summary };
report += "\n" + vsum + "\n";

let path = write_scratch_file("provider-integration-" + venue + ".md", report);
log("Wrote report " + path);

complete(#{
    ok: any_fail == false && (do_verify == false || (verify_out.build_ok == true && verify_out.tests_ok == true && verify_out.hotpath_ok == true && verify_out.layers_ok == true && verify_out.freeze_ok == true)),
    venue: venue,
    codepath: codepath,
    flag: flag,
    has_macro: has_macro,
    max_phase: max_phase,
    report_path: path,
    verify: verify_out,
    phase_count: results.len(),
});
