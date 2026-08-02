let meta = #{
    name: "implement-data-architecture",
    description: "Execute docs/internal/data-pipeline.md phases D-00..D-11: MarketSeries, IMarketSink/Source, DataWrapper",
    when_to_use: "When implementing the market-data pipeline redesign from docs/internal/data-pipeline.md",
    phases: [
        #{ title: "Survey", detail: "D-00 inventory + clean-architecture diagnosis" },
        #{ title: "Foundation", detail: "D-01 domain types + IMarketSink + series store" },
        #{ title: "Consumers", detail: "D-02/D-03 engine + privatize + rename" },
        #{ title: "Sources", detail: "D-04/D-05 IMarketSource + DataWrapper + CLI" },
        #{ title: "Hardening", detail: "D-06 stream retain, D-07 dead code, D-10 timestamps" },
        #{ title: "Verify", detail: "D-11 cleanup, gates, tests" },
    ],
};

let phase_schema = #{
    "type": "object",
    "required": ["phase", "status", "summary", "files_changed", "risks"],
    "properties": #{
        "phase": #{ "type": "string" },
        "status": #{ "type": "string" },
        "summary": #{ "type": "string" },
        "files_changed": #{
            "type": "array",
            "maxItems": 40,
            "items": #{ "type": "string" },
        },
        "risks": #{
            "type": "array",
            "maxItems": 10,
            "items": #{ "type": "string" },
        },
        "next": #{ "type": "string" },
    },
};

let verdict_schema = #{
    "type": "object",
    "required": ["pass", "summary", "blockers"],
    "properties": #{
        "pass": #{ "type": "boolean" },
        "summary": #{ "type": "string" },
        "blockers": #{
            "type": "array",
            "maxItems": 12,
            "items": #{ "type": "string" },
        },
    },
};

// ── args ──────────────────────────────────────────────────────────────────
let skip_parquet = true;
let skip_http = true;
if args != () {
    if args.skip_parquet != () { skip_parquet = args.skip_parquet; }
    if args.skip_http != () { skip_http = args.skip_http; }
}

// ═══════════════════════════════════════════════════════════════════════════
phase("Survey");
// ═══════════════════════════════════════════════════════════════════════════

let common = "";
common += "Workspace: TrueTest core C++23 trading engine. Plan: docs/internal/data-pipeline.md. ";
common += "AGENTS.md rules: zero-alloc hot path, layer deps (data must not include engine/providers venue), ";
common += "engine.cpp is LIVE-SAFETY freeze surface — minimal edits only. ";
common += "Register sources in cmake/Sources.cmake. After src/ edits run: ";
common += "./scripts/check-hotpath-json.sh && ./scripts/check-layer-deps.sh && ./scripts/check-live-safety-freeze.sh. ";
common += "QuestDB stays IOrderAuditSink only — never market ingress. ";

let s1 = common;
s1 += "READ-ONLY D-00 INVENTORY. Map every construction/mutation of data_handler, IDataSource, ";
s1 += "CsvDataSource, TickCsvDataSource, BinaryCacheSource, WebSocketDataSource, DataBridge, ";
s1 += "load_from_csv, load_into_queue, db_data_* peeks in engine/main/api/MC/tests. ";
s1 += "Use grep+read_file. Decision: DELETE BinaryCacheSource and WebSocketDataSource (unwired). ";
s1 += "Return phase=D-00, status=done, summary of inventory, files_changed=[], risks, next.";

let s2 = common;
s2 += "READ-ONLY CLEAN ARCHITECTURE. Apply dependency rule: domain (Bar/Tick) independent of I/O; ";
s2 += "MarketSeries store no formats; IMarketSource adapters at edge; DataWrapper composition root; ";
s2 += "engine depends only on read views. Check scripts/check-layer-deps.sh ALLOWED map for data/providers/engine. ";
s2 += "Report target placement and any layer risks for DataWrapper::from_provider. ";
s2 += "Return phase=arch, status=done, summary, files_changed=[], risks, next.";

let survey = parallel([
    #{ prompt: s1, label: "d00-inventory", capability_mode: "read-only", agent_type: "explore", output_schema: phase_schema },
    #{ prompt: s2, label: "clean-arch", capability_mode: "read-only", agent_type: "explore", output_schema: phase_schema },
]);

log("Survey complete");

// ═══════════════════════════════════════════════════════════════════════════
phase("Foundation");
// ═══════════════════════════════════════════════════════════════════════════

let f1 = common;
f1 += "IMPLEMENT D-01 + store foundation (write code). Create: ";
f1 += "src/data/market_types.h (Bar, Tick, tick_side; alias data_tick_side=tick_side; tick_record=Tick with fields timestamp/symbol/price/quantity/side). ";
f1 += "src/data/market_sink.h (IMarketSink::on_bar/on_tick, LoadStats). ";
f1 += "src/data/market_source.h (IMarketSource::load_into/stream_into). ";
f1 += "Evolve data_handler into MarketSeries implementing IMarketSink with PRIVATE SoA: ";
f1 += "bar_ts_, bar_date_, bar_symbol_, open/high/low/close/volume, ticks_. ";
f1 += "Public API: on_bar, on_tick, load_into_queue (parse date→ts via date_parse), add_tick, ";
f1 += "bar_count/tick_count/empty/has_bar_data/has_tick_data, bar_at→BarView (string_view symbol, no alloc), ";
f1 += "tick_at, first_symbol, set_all_bar_symbols, clear(=reset), reserve/reserve_bars/reserve_ticks, ";
f1 += "sort_by_date + sort_bars_by_time (primary ts, secondary symbol), validation_errors. ";
f1 += "using data_handler = MarketSeries; keep data_handler.h including market_series. ";
f1 += "MOVE load_from_csv OUT of store into CsvDataSource or sources/csv_bar_source. ";
f1 += "Update data_loader.cpp→market_series.cpp. Register cmake/Sources.cmake. ";
f1 += "Update tests/test_data_handler.cpp for new API (no public db_data_*). ";
f1 += "Add tests for on_bar/on_tick mapping parity. Build tests if possible. ";
f1 += "Do NOT expand scope to engine yet if blocked — leave engine for next phase. ";
f1 += "Return phase=D-01, status, summary, files_changed, risks, next.";

let foundation = agent(f1, #{
    label: "d01-foundation",
    capability_mode: "all",
    agent_type: "general-purpose",
    output_schema: phase_schema,
});

if foundation == () || !foundation.success {
    complete(#{ summary: "Foundation agent failed", survey: survey, foundation: foundation });
}

// ═══════════════════════════════════════════════════════════════════════════
phase("Consumers");
// ═══════════════════════════════════════════════════════════════════════════

let c1 = common;
c1 += "IMPLEMENT D-02/D-03 consumers. MarketSeries read API should already exist. ";
c1 += "Migrate engine::run / run_tick_data / switch_symbol / strategy symbol peek to bar_count/bar_at/tick_at/first_symbol/set_all_bar_symbols. ";
c1 += "BarView must not allocate. Prefer bar_at(i).ts when seed==0 instead of re-parsing date strings. ";
c1 += "engine.cpp is FREEZE surface — change ONLY data access lines, keep event loop identical. ";
c1 += "Update MC controller, main.inc db_data_symbol.size peeks, truetest_api, benchmarks, ALL tests that poke db_data_*/tick_data public. ";
c1 += "Keep using data_handler = MarketSeries typedef. ";
c1 += "Run gate scripts after. Return phase=D-02-03, status, summary, files_changed, risks, next.";

let consumers = agent(c1, #{
    label: "d02-d03-consumers",
    capability_mode: "all",
    agent_type: "general-purpose",
    output_schema: phase_schema,
});

// ═══════════════════════════════════════════════════════════════════════════
phase("Sources");
// ═══════════════════════════════════════════════════════════════════════════

let src_p = common;
src_p += "IMPLEMENT D-04/D-05. IMarketSource replaces IDataSource (keep IDataSource shim: load_data → load_into(*handler)). ";
src_p += "Migrate DataBridge to load_into(IMarketSink&) and stream_into; sink_fn should use IMarketSink not shared_ptr only. ";
src_p += "CsvDataSource/TickCsvDataSource implement IMarketSource (under src/data/sources/ optional). ";
src_p += "provider_sink/provider_convert map to domain Bar/Tick → on_bar/on_tick. ";
src_p += "Implement DataWrapper: from_paths, from_uri (csv:/csv+tick:/plain path), from_source, from_provider if layer-safe ";
src_p += "(if data→providers forbidden, put provider factory under providers/ or main composition). ";
src_p += "DataLoadOptions: sort_after_load, fail_if_empty, retain_streamed, reserve_hint. ";
src_p += "Map main.inc + truetest_api batch load through DataWrapper where easy; keep CLI flags stable. ";
src_p += "Return phase=D-04-05, status, summary, files_changed, risks, next.";

let sources = agent(src_p, #{
    label: "d04-d05-sources",
    capability_mode: "all",
    agent_type: "general-purpose",
    output_schema: phase_schema,
});

// ═══════════════════════════════════════════════════════════════════════════
phase("Hardening");
// ═══════════════════════════════════════════════════════════════════════════

let h1 = common;
h1 += "IMPLEMENT D-06/D-07/D-10. ";
h1 += "D-06: DataBridge/DataWrapper stream path — retain_streamed=false default means do not append to series during stream ";
h1 += "(only on_record/engine callback); retain_streamed=true keeps old append behaviour. Wire option. ";
h1 += "D-07: DELETE BinaryCacheSource and WebSocketDataSource (files + cmake Sources + Dependencies ENABLE_LIVE_DATA websocket). ";
h1 += "Confirm no callers via grep. ";
h1 += "D-10: sort_bars_by_time primary; engine uses stored ts; multi-symbol secondary key. ";
if skip_parquet {
    h1 += "D-08 Parquet: DO NOT add Arrow dependency — document deferred in docs/internal/data-pipeline.md Open Decisions. ";
}
if skip_http {
    h1 += "D-09 Historical API: DO NOT add network client — document deferred. ";
}
h1 += "Return phase=D-06-10, status, summary, files_changed, risks, next.";

let hard = agent(h1, #{
    label: "d06-d10-harden",
    capability_mode: "all",
    agent_type: "general-purpose",
    output_schema: phase_schema,
});

// ═══════════════════════════════════════════════════════════════════════════
phase("Verify");
// ═══════════════════════════════════════════════════════════════════════════

let v1 = common;
v1 += "D-11 CLEANUP + VERIFY. Update docs/internal/data-pipeline.md status lines for completed phases. ";
v1 += "Short pointer in docs if needed. Ensure cmake lists only live TUs. ";
v1 += "Run: ./scripts/check-hotpath-json.sh ; ./scripts/check-layer-deps.sh ; ./scripts/check-live-safety-freeze.sh ; ";
v1 += "cmake --build build -j (or configure with linux-tests if needed); ctest --test-dir build --output-on-failure ";
v1 += "or focused: truetest_tests filters for data|Data|golden|hotpath. ";
v1 += "Fix any build/test failures you introduced. ";
v1 += "Return phase=D-11, status, summary, files_changed, risks, next.";

let cleanup = agent(v1, #{
    label: "d11-verify",
    capability_mode: "all",
    agent_type: "general-purpose",
    output_schema: phase_schema,
});

// Parallel adversarial verify
let vjobs = [];
let vp = common;
vp += "ADVERSARIAL VERIFY of data pipeline redesign. Grep for remaining public db_data_ writes outside market_series, ";
vp += "IDataSource without shim, load_from_csv on store, BinaryCache/WebSocket leftovers, layer violations data→engine. ";
vp += "Read key headers. pass=true only if architecture goals 1-7 from docs/internal/data-pipeline.md §1 substantially met for non-deferred phases. ";
vp += "List blockers concretely.";
vjobs.push(#{ prompt: vp, label: "adv-arch", capability_mode: "read-only", agent_type: "explore", output_schema: verdict_schema });

let vp2 = common;
vp2 += "PERF/HOT-PATH VERIFY. Confirm BarView is non-allocating, engine batch loop does not parse CSV/json, ";
vp2 += "MarketSeries clear keeps capacity (reserve then clear). Read market_series and engine run loops. pass=true if OK.";
vjobs.push(#{ prompt: vp2, label: "adv-perf", capability_mode: "read-only", agent_type: "explore", output_schema: verdict_schema });

let verdicts = parallel(vjobs);

let report = "";
report += "# Data Architecture Implementation Report\n\n";
report += "Plan: docs/internal/data-pipeline.md\n\n";
if foundation != () && foundation.success { report += "## Foundation\n" + foundation.output.summary + "\n\n"; }
if consumers != () && consumers.success { report += "## Consumers\n" + consumers.output.summary + "\n\n"; }
if sources != () && sources.success { report += "## Sources\n" + sources.output.summary + "\n\n"; }
if hard != () && hard.success { report += "## Hardening\n" + hard.output.summary + "\n\n"; }
if cleanup != () && cleanup.success { report += "## Cleanup\n" + cleanup.output.summary + "\n\n"; }
report += "## Verification\n";
for v in verdicts {
    if v != () && v.success {
        report += "- pass=" + v.output.pass.to_string() + ": " + v.output.summary + "\n";
    }
}

let path = write_scratch_file("data-architecture-report.md", report);
complete(#{
    path: path,
    report: report,
    foundation: foundation,
    consumers: consumers,
    sources: sources,
    hardening: hard,
    cleanup: cleanup,
    verdicts: verdicts,
});
