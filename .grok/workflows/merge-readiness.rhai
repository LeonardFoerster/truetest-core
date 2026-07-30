let meta = #{
    name: "merge-readiness",
    description: "Investigate what a feature branch needs to merge cleanly into master",
    phases: [
        #{ title: "Survey", detail: "parallel lenses on conflicts, intent, master drift" },
        #{ title: "Synthesize", detail: "ordered fix plan for merge-ready state" },
    ],
};

let report_schema = #{
    "type": "object",
    "required": ["blockers", "actions", "risk_notes", "summary"],
    "properties": #{
        "blockers": #{
            "type": "array",
            "maxItems": 12,
            "items": #{
                "type": "object",
                "required": ["severity", "detail"],
                "properties": #{
                    "severity": #{ "type": "string" },
                    "detail": #{ "type": "string" },
                },
            },
        },
        "actions": #{
            "type": "array",
            "maxItems": 15,
            "items": #{
                "type": "object",
                "required": ["order", "action", "files"],
                "properties": #{
                    "order": #{ "type": "integer" },
                    "action": #{ "type": "string" },
                    "files": #{ "type": "string" },
                },
            },
        },
        "risk_notes": #{
            "type": "array",
            "maxItems": 8,
            "items": #{ "type": "string" },
        },
        "summary": #{ "type": "string" },
    },
};

let branch = if args == () { () } else { args.branch };
let base = if args == () { () } else { args.base };
if branch == () { pause("verification", "Pass args.branch (feature branch name)."); }
if base == () { base = "master"; }

phase("Survey");

let p1 = "";
p1 += "You are investigating git merge readiness for branch '" + branch + "' into '" + base + "'. ";
p1 += "READ-ONLY. Use shell via run_terminal_command only for git/read tools: git log, git diff, git merge-tree, git show, read_file, grep, list_dir. ";
p1 += "Do NOT checkout, merge, or edit. ";
p1 += "Focus: content CONFLICT files when merging " + base + " into " + branch + ". ";
p1 += "Run: git merge-tree $(git merge-base " + base + " " + branch + ") " + base + " " + branch + " and extract conflict markers / conflicted paths. ";
p1 += "For each conflict file, summarize BOTH sides (what master changed vs what the feature branch changed) and propose a resolution strategy that preserves feature branch intent (backtest accuracy / fill realism) while keeping master safety/engine improvements. ";
p1 += "Return blockers, ordered actions with files, risk_notes, summary.";

let p2 = "";
p2 += "You are investigating git merge readiness for branch '" + branch + "' into '" + base + "'. ";
p2 += "READ-ONLY. Use git log/diff/show and read_file/grep only. No edits. ";
p2 += "Focus: FEATURE INTENT of " + branch + ". List the 7ish unique commits, what realism/determinism/PnL features they add, which APIs/types they introduce, and which tests cover them. ";
p2 += "Identify symbols/APIs that may have been renamed or removed on " + base + " so the feature may not compile after merge. ";
p2 += "Return blockers, ordered actions with files, risk_notes, summary.";

let p3 = "";
p3 += "You are investigating git merge readiness for branch '" + branch + "' into '" + base + "'. ";
p3 += "READ-ONLY. Use git log/diff and read_file/grep only. No edits. ";
p3 += "Focus: MASTER DRIFT since the branch diverged. Summarize major themes on " + base + " not in " + branch + " ";
p3 += "(engine decomposition, live safety, tick-to-trade, providers, risk halt, pools, cmake Sources.cmake, freeze files). ";
p3 += "Flag files on the live-safety freeze list that the feature branch also touches. ";
p3 += "Return blockers, ordered actions with files, risk_notes, summary.";

let p4 = "";
p4 += "You are investigating git merge readiness for branch '" + branch + "' into '" + base + "'. ";
p4 += "READ-ONLY. Use git show/diff and read_file on tests and CMake. No edits. ";
p4 += "Focus: BUILD & TEST readiness after merge. Check CMakeLists/Sources.cmake differences, new test files on the feature branch (e.g. test_stop_fill_pricing.cpp), whether they are registered on master-style Sources.cmake, golden files, and gate scripts expectations. ";
p4 += "Propose a post-merge verification sequence (build, ctest, gate scripts). ";
p4 += "Return blockers, ordered actions with files, risk_notes, summary.";

let survey = parallel([
    #{ prompt: p1, label: "conflicts", capability_mode: "read-only", output_schema: report_schema },
    #{ prompt: p2, label: "feature-intent", capability_mode: "read-only", output_schema: report_schema },
    #{ prompt: p3, label: "master-drift", capability_mode: "read-only", output_schema: report_schema },
    #{ prompt: p4, label: "build-test", capability_mode: "read-only", output_schema: report_schema },
]);

phase("Synthesize");

let bundle = "";
let i = 0;
let labels = ["conflicts", "feature-intent", "master-drift", "build-test"];
for r in survey {
    let lab = labels[i];
    if r == () || !r.success {
        bundle += "PANEL " + lab + ": FAILED or empty\n";
    } else {
        bundle += "PANEL " + lab + " summary: " + r.output.summary + "\n";
        bundle += "blockers: " + json_encode(r.output.blockers) + "\n";
        bundle += "actions: " + json_encode(r.output.actions) + "\n";
        bundle += "risks: " + json_encode(r.output.risk_notes) + "\n\n";
    }
    i += 1;
}

let syn = "";
syn += "Synthesize a single merge-ready action plan for branch '" + branch + "' into '" + base + "'. ";
syn += "You have four investigation panels below. Deduplicate, order by dependency (checkout → merge → conflict resolve → compile → tests → gates → freeze/safety), ";
syn += "and produce the definitive blockers/actions/risk_notes/summary an implementer should follow. ";
syn += "Prefer concrete file paths and resolution rules (keep feature X, take master Y). ";
syn += "Do not invent files; only use evidence from the panels. Panels:\n\n" + bundle;

let final = agent(syn, #{
    label: "synthesize",
    capability_mode: "read-only",
    output_schema: report_schema,
});

if final == () || !final.success {
    complete(#{
        summary: "Synthesis failed; raw panel bundle only",
        panels: bundle,
        success: false,
    });
}

let report = "# Merge readiness: " + branch + " → " + base + "\n\n";
if final.output.summary != () {
    report += final.output.summary + "\n\n";
}
report += "## Blockers (JSON)\n" + json_encode(final.output.blockers) + "\n\n";
report += "## Actions (JSON)\n" + json_encode(final.output.actions) + "\n\n";
report += "## Risk notes (JSON)\n" + json_encode(final.output.risk_notes) + "\n";
let path = write_scratch_file("merge-readiness-report.md", report);
complete(#{
    path: path,
    summary: final.output.summary,
    blockers: final.output.blockers,
    actions: final.output.actions,
    risk_notes: final.output.risk_notes,
    success: true,
});
