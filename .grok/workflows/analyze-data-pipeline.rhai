let meta = #{
    name: "analyze-data-pipeline",
    description: "Analyze, understand, and summarize the core data pipeline and wrappers",
    when_to_use: "When you need a structured map of src/data, IDataSource wrappers, DataBridge, and QuestDB flow",
    phases: [
        #{ title: "Survey", detail: "parallel lenses: sources, handler, wrappers, QuestDB, engine wiring" },
        #{ title: "Synthesize", detail: "unified architecture summary report" },
    ],
};

let lens_schema = #{
    "type": "object",
    "required": ["title", "components", "flow", "invariants", "open_questions", "summary"],
    "properties": #{
        "title": #{ "type": "string" },
        "components": #{
            "type": "array",
            "maxItems": 12,
            "items": #{
                "type": "object",
                "required": ["name", "path", "role"],
                "properties": #{
                    "name": #{ "type": "string" },
                    "path": #{ "type": "string" },
                    "role": #{ "type": "string" },
                },
            },
        },
        "flow": #{
            "type": "array",
            "maxItems": 10,
            "items": #{ "type": "string" },
        },
        "invariants": #{
            "type": "array",
            "maxItems": 8,
            "items": #{ "type": "string" },
        },
        "open_questions": #{
            "type": "array",
            "maxItems": 6,
            "items": #{ "type": "string" },
        },
        "summary": #{ "type": "string" },
    },
};

let root = if args == () { () } else { args.root };
if root == () { root = "src/data"; }

phase("Survey");

let common = "";
common += "You are analyzing TrueTest core's data pipeline / wrappers under workspace root. ";
common += "READ-ONLY. Use read_file, grep, list_dir only — no edits, no builds. ";
common += "Scope starts at " + root + " but you MUST also follow includes/callers into ";
common += "src/providers (DataBridge, local CSV), src/engine, src/simulation, src/api, src/bin as needed. ";
common += "Do not invent APIs. Quote real type/method names from code you read. ";
common += "Empty lists are valid only after you actually inspected the files. ";

let p1 = common;
p1 += "LENS: INGRESS SOURCES. Map IDataSource and every concrete source: ";
p1 += "CsvDataSource, TickCsvDataSource, BinaryCacheSource, WebSocketDataSource, any others. ";
p1 += "For each: path, load_data signature, what it writes into data_handler (bars vs ticks), ";
p1 += "error handling, and whether it is hot-path or cold startup. ";
p1 += "Return title, components, flow steps, invariants, open_questions, summary.";

let p2 = common;
p2 += "LENS: data_handler CORE. Read data_handler.h and data_loader.cpp thoroughly. ";
p2 += "Document bar vs tick storage layout (SoA vectors vs tick_record), load_from_csv, ";
p2 += "load_into_queue, add_tick, sort_by_date, reserve, reset, validation_errors. ";
p2 += "Note MC reuse (reset) and performance notes (reserve). ";
p2 += "Who owns the handler after load? Return title, components, flow, invariants, open_questions, summary.";

let p3 = common;
p3 += "LENS: WRAPPERS & ADAPTERS. Focus on BinaryCacheSource (cache-over-fallback wrapper) ";
p3 += "and providers/data_bridge.h DataBridge (IDataSource adapter). Also date_parse helpers ";
p3 += "and any FileTransport/local provider path that feeds the handler. ";
p3 += "Explain the decorator/adapter pattern: who wraps whom, cache format (BinaryCacheHeader), ";
p3 += "sink_fn pattern, and how provider parse lands in data_handler. ";
p3 += "Return title, components, flow, invariants, open_questions, summary.";

let p4 = common;
p4 += "LENS: QuestDB SINK. Map src/data/questdb/*: schema, store, ilp_writer, tcp_client, ";
p4 += "http_client, run_tag. Is this market-data ingress or audit/persist egress? ";
p4 += "Document write path, failure modes, and relationship to the IDataSource load pipeline. ";
p4 += "Return title, components, flow, invariants, open_questions, summary.";

let p5 = common;
p5 += "LENS: ENGINE & CALLERS. Find where data_handler / IDataSource / CsvDataSource / DataBridge ";
p5 += "are constructed and consumed: engine ctor, main/bin, truetest_api, monte_carlo_controller, ";
p5 += "provider_convert. Document the end-to-end flow from CLI/API flag to engine event loop. ";
p5 += "Distinguish historical batch load vs live/stream paths. ";
p5 += "Return title, components, flow, invariants, open_questions, summary.";

let survey = parallel([
    #{ prompt: p1, label: "ingress-sources", capability_mode: "read-only", output_schema: lens_schema, agent_type: "explore" },
    #{ prompt: p2, label: "data-handler", capability_mode: "read-only", output_schema: lens_schema, agent_type: "explore" },
    #{ prompt: p3, label: "wrappers", capability_mode: "read-only", output_schema: lens_schema, agent_type: "explore" },
    #{ prompt: p4, label: "questdb-sink", capability_mode: "read-only", output_schema: lens_schema, agent_type: "explore" },
    #{ prompt: p5, label: "engine-callers", capability_mode: "read-only", output_schema: lens_schema, agent_type: "explore" },
]);

phase("Synthesize");

let bundle = "";
let labels = ["ingress-sources", "data-handler", "wrappers", "questdb-sink", "engine-callers"];
let i = 0;
let ok_count = 0;
for r in survey {
    let lab = labels[i];
    if r == () || !r.success {
        bundle += "### " + lab + "\nFAILED or empty — treat as uncovered.\n\n";
    } else {
        ok_count += 1;
        bundle += "### " + lab + "\n";
        bundle += "title: " + r.output.title + "\n";
        bundle += "summary: " + r.output.summary + "\n";
        bundle += "flow:\n";
        if r.output.flow != () {
            for step in r.output.flow {
                bundle += "- " + step + "\n";
            }
        }
        bundle += "components:\n";
        if r.output.components != () {
            for c in r.output.components {
                bundle += "- " + c.name + " @ " + c.path + " — " + c.role + "\n";
            }
        }
        bundle += "invariants:\n";
        if r.output.invariants != () {
            for inv in r.output.invariants {
                bundle += "- " + inv + "\n";
            }
        }
        bundle += "open_questions:\n";
        if r.output.open_questions != () {
            for q in r.output.open_questions {
                bundle += "- " + q + "\n";
            }
        }
        bundle += "\n";
    }
    i += 1;
}

log(ok_count.to_string() + "/5 survey lenses succeeded");

let synth = "";
synth += "You are the architecture synthesizer for TrueTest core's data pipeline / wrappers. ";
synth += "READ-ONLY: you may re-read key headers with read_file/grep to resolve contradictions, ";
synth += "but prefer the survey bundle below. Produce a single markdown report for humans.\n\n";
synth += "Required sections (use these exact headings):\n";
synth += "1. Executive summary (what the pipeline is, in 5-8 sentences)\n";
synth += "2. Architecture diagram (ASCII or mermaid of load path vs live path vs QuestDB)\n";
synth += "3. Component catalog (table: name | path | role | hot/cold)\n";
synth += "4. Wrapper / adapter pattern (BinaryCacheSource, DataBridge, and any others)\n";
synth += "5. data_handler memory model (bars SoA vs ticks, lifecycle, MC reset)\n";
synth += "6. End-to-end flows (CSV backtest; binary cache; provider/DataBridge; MC synthetic; live WS if any)\n";
synth += "7. QuestDB role (ingress vs egress; what tables)\n";
synth += "8. Invariants & red lines (determinism, zero-alloc boundaries, fail-closed)\n";
synth += "9. Gaps / tech debt / open questions\n";
synth += "10. Suggested next reads for a new engineer\n\n";
synth += "Rules: only claim what the survey or re-read code supports. Flag uncertainty. ";
synth += "Be concrete with file paths and type names. Keep under ~250 lines.\n\n";
synth += "SURVEY BUNDLE:\n";
synth += bundle;

let final_r = agent(synth, #{
    label: "synthesize-report",
    capability_mode: "read-only",
    agent_type: "explore",
});

if final_r == () || !final_r.success {
    let fallback = "# Data pipeline analysis (partial)\n\nSurvey lenses OK: " + ok_count.to_string() + "/5\n\n" + bundle;
    let path = write_scratch_file("data-pipeline-summary.md", fallback);
    complete(#{
        path: path,
        summary: "Partial: synthesizer failed; raw survey written",
        lenses_ok: ok_count,
    });
}

let report_text = final_r.output;
if type_of(report_text) != "string" {
    report_text = json_encode(report_text);
}

let path = write_scratch_file("data-pipeline-summary.md", report_text);
log("Wrote summary to " + path);
complete(#{
    path: path,
    summary: "Data pipeline / wrapper analysis complete",
    lenses_ok: ok_count,
    report: report_text,
});
