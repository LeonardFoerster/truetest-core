let meta = #{
    name: "fix-cmake-build",
    description: "Diagnose broken CMake configure for launch-default config, fix it, verify",
    phases: [
        #{ title: "Diagnose", detail: "reproduce failure and map root cause" },
        #{ title: "Fix", detail: "patch cmake/Dependencies.cmake for modern Boost" },
        #{ title: "Verify", detail: "reconfigure with LIVE_DATA and confirm build" },
    ],
};

let diagnosis_schema = #{
    "type": "object",
    "required": ["root_cause", "failing_file", "failing_line_hint", "fix_plan", "notes"],
    "properties": #{
        "root_cause": #{ "type": "string" },
        "failing_file": #{ "type": "string" },
        "failing_line_hint": #{ "type": "string" },
        "fix_plan": #{ "type": "string" },
        "notes": #{ "type": "array", "maxItems": 8, "items": #{ "type": "string" } },
    },
};

let fix_schema = #{
    "type": "object",
    "required": ["changed_files", "what_changed", "success", "evidence"],
    "properties": #{
        "changed_files": #{ "type": "array", "maxItems": 8, "items": #{ "type": "string" } },
        "what_changed": #{ "type": "string" },
        "success": #{ "type": "boolean" },
        "evidence": #{ "type": "string" },
    },
};

let verify_schema = #{
    "type": "object",
    "required": ["configure_ok", "build_ok", "evidence", "residual_risks"],
    "properties": #{
        "configure_ok": #{ "type": "boolean" },
        "build_ok": #{ "type": "boolean" },
        "evidence": #{ "type": "string" },
        "residual_risks": #{ "type": "array", "maxItems": 6, "items": #{ "type": "string" } },
    },
};

// ── Phase 1: Diagnose ──────────────────────────────────────────────────────
phase("Diagnose");

let dprompt = "";
dprompt += "You are diagnosing a broken CMake configuration in the truetest core repo at the workspace root. ";
dprompt += "Context: launch-default.sh configures with ENABLE_IMGUI=ON, ENABLE_LIVE_DATA=ON, ENABLE_BINANCE=ON, ";
dprompt += "ENABLE_BITGET=ON, ENABLE_BITUNIX=ON, BUILD_TESTS=ON and fails at configure time. ";
dprompt += "User says the script is fine — CMake is the problem. ";
dprompt += "REQUIRED steps (use tools, do not guess): ";
dprompt += "1) Read launch-default.sh (may be gitignored — use shell cat if read_file fails). ";
dprompt += "2) Read cmake/Dependencies.cmake around ENABLE_LIVE_DATA and venue Boost wiring. ";
dprompt += "3) Run a clean configure that reproduces the failure (use a temp binary dir like /tmp/tt-cmake-diag-$$). ";
dprompt += "4) Check installed Boost packages under /usr/lib/cmake/ for boost_system vs boost_headers. ";
dprompt += "5) Confirm whether ENABLE_BINANCE alone works without Boost::system. ";
dprompt += "Return structured diagnosis: root_cause, failing_file, failing_line_hint, fix_plan, notes. ";
dprompt += "fix_plan must be concrete (what to change in Dependencies.cmake). Prefer fixing CMake to accept ";
dprompt += "header-only Boost.System / Boost::headers rather than removing the flag from the script.";

let d = agent(dprompt, #{
    label: "diagnose-cmake",
    capability_mode: "execute",
    output_schema: diagnosis_schema,
});

if (d == () || !d.success || d.output == ()) {
    complete(#{
        summary: "Diagnosis agent failed; cannot safely fix.",
        configure_ok: false,
        build_ok: false,
    });
}

log("Root cause: " + d.output.root_cause);
log("Fix plan: " + d.output.fix_plan);

// ── Phase 2: Fix ───────────────────────────────────────────────────────────
phase("Fix");

let fprompt = "";
fprompt += "Apply a minimal CMake fix for the diagnosed ENABLE_LIVE_DATA / Boost::system failure. ";
fprompt += "Diagnosis root_cause: " + d.output.root_cause + " ";
fprompt += "Failing file: " + d.output.failing_file + " ";
fprompt += "Hint: " + d.output.failing_line_hint + " ";
fprompt += "Plan: " + d.output.fix_plan + " ";
fprompt += "Constraints: ";
fprompt += "- Edit cmake/Dependencies.cmake (and only related CMake if strictly needed). ";
fprompt += "- ENABLE_LIVE_DATA must still define HAS_LIVE_DATA when ON. ";
fprompt += "- Do NOT require Boost COMPONENTS system when Boost 1.69+ ships system header-only and ";
fprompt += "  BoostConfig has no boost_system package (this machine has Boost 1.91 with boost_headers only). ";
fprompt += "- Prefer: find_package(Boost REQUIRED) then link Boost::headers if present, else Boost::system if present. ";
fprompt += "- Keep venue blocks (BINANCE/BITGET/BITUNIX) using Boost::headers as they already do. ";
fprompt += "- Do not change launch-default.sh unless CMake alone cannot fix it. ";
fprompt += "- Do not touch freeze-surface source files. ";
fprompt += "After editing, show a short diff via `git diff -- cmake/Dependencies.cmake`. ";
fprompt += "Return changed_files, what_changed, success, evidence.";

let f = agent(fprompt, #{
    label: "fix-cmake",
    capability_mode: "all",
    output_schema: fix_schema,
});

if (f == () || !f.success || f.output == () || f.output.success != true) {
    let ev = "no evidence";
    if (f != () && f.success && f.output != () && f.output.evidence != ()) {
        ev = f.output.evidence;
    }
    complete(#{
        summary: "Fix agent did not report success.",
        root_cause: d.output.root_cause,
        configure_ok: false,
        build_ok: false,
        evidence: ev,
    });
}

log("Fix applied: " + f.output.what_changed);

// ── Phase 3: Verify ────────────────────────────────────────────────────────
phase("Verify");

let vprompt = "";
vprompt += "Verify the CMake fix for launch-default-style configuration. ";
vprompt += "Use a clean binary dir under /tmp/tt-cmake-verify-$$ (do not delete the user's build-dev if present). ";
vprompt += "1) Configure exactly: ";
vprompt += "cmake -S . -B <tmpdir> -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTS=ON -DENABLE_IMGUI=ON ";
vprompt += "-DENABLE_LIVE_DATA=ON -DENABLE_BINANCE=ON -DENABLE_BITGET=ON -DENABLE_BITUNIX=ON ";
vprompt += "-DENABLE_DEBUG=OFF -DENABLE_BENCHMARKS=OFF -DENABLE_NATIVE_OPT=OFF ";
vprompt += "2) If configure succeeds, build targets: engine_shadow (and truetest_tests if time allows) with -j. ";
vprompt += "3) Report configure_ok, build_ok, evidence (last ~30 lines of success or the first error), residual_risks. ";
vprompt += "configure_ok=true only if cmake exit 0. build_ok=true only if the build exit 0. ";
vprompt += "Clean up the temp dir at the end if you want.";

let v = agent(vprompt, #{
    label: "verify-cmake",
    capability_mode: "execute",
    output_schema: verify_schema,
});

let configure_ok = false;
let build_ok = false;
let evidence = "";
let residual = [];
if (v != () && v.success && v.output != ()) {
    configure_ok = v.output.configure_ok == true;
    build_ok = v.output.build_ok == true;
    if (v.output.evidence != ()) { evidence = v.output.evidence; }
    if (v.output.residual_risks != ()) { residual = v.output.residual_risks; }
}

let summary = "";
if (configure_ok && build_ok) {
    summary = "CMake ENABLE_LIVE_DATA fixed for modern Boost; launch-default config configures and builds.";
} else if (configure_ok) {
    summary = "Configure fixed but build still fails — see evidence.";
} else {
    summary = "Configure still broken after fix attempt — see evidence.";
}

// Persist a short report for the parent session.
let report = "";
report += "# CMake build fix report\n\n";
report += "## Root cause\n" + d.output.root_cause + "\n\n";
report += "## Fix\n" + f.output.what_changed + "\n\n";
report += "## Verification\n";
report += "- configure_ok: " + configure_ok.to_string() + "\n";
report += "- build_ok: " + build_ok.to_string() + "\n";
report += "- evidence: " + evidence + "\n\n";
report += "## Residual risks\n";
for r in residual {
    report += "- " + r + "\n";
}
let path = write_scratch_file("cmake-fix-report.md", report);

complete(#{
    summary: summary,
    root_cause: d.output.root_cause,
    fix: f.output.what_changed,
    changed_files: f.output.changed_files,
    configure_ok: configure_ok,
    build_ok: build_ok,
    evidence: evidence,
    residual_risks: residual,
    report_path: path,
});
