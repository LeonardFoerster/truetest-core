let meta = #{
    name: "strategy-dev-docs",
    description: "Write technical strategy-development docs from code: IStrategy, indicators, entry/exit, TP/SL, registration — not alpha/edge research",
    when_to_use: "When strategy authoring docs are missing/stale, or after strategy/exit/indicator API changes",
    phases: [
        #{ title: "Research", detail: "parallel code readers for strategy surface" },
        #{ title: "Outline", detail: "doc structure + claim inventory" },
        #{ title: "Draft", detail: "full markdown guide from verified code facts" },
        #{ title: "Verify", detail: "adversarial fact-check of draft claims" },
        #{ title: "Write", detail: "publish docs/reference strategy guide + index link" },
    ],
};

// ---------------------------------------------------------------------------
// Schemas
// ---------------------------------------------------------------------------
let research_schema = #{
    "type": "object",
    "required": ["area", "facts", "file_refs", "code_snippets", "gotchas"],
    "properties": #{
        "area": #{ "type": "string" },
        "summary": #{ "type": "string" },
        "facts": #{
            "type": "array", "maxItems": 40,
            "items": #{
                "type": "object",
                "required": ["id", "claim", "evidence"],
                "properties": #{
                    "id": #{ "type": "string" },
                    "claim": #{ "type": "string" },
                    "evidence": #{ "type": "string" },
                    "severity": #{ "type": "string" },
                },
            },
        },
        "file_refs": #{
            "type": "array", "maxItems": 30,
            "items": #{ "type": "string" },
        },
        "code_snippets": #{
            "type": "array", "maxItems": 12,
            "items": #{
                "type": "object",
                "required": ["title", "path", "snippet", "why"],
                "properties": #{
                    "title": #{ "type": "string" },
                    "path": #{ "type": "string" },
                    "snippet": #{ "type": "string" },
                    "why": #{ "type": "string" },
                },
            },
        },
        "gotchas": #{
            "type": "array", "maxItems": 15,
            "items": #{ "type": "string" },
        },
        "undocumented": #{
            "type": "array", "maxItems": 15,
            "items": #{ "type": "string" },
        },
    },
};

let outline_schema = #{
    "type": "object",
    "required": ["title", "target_path", "sections", "audience", "out_of_scope"],
    "properties": #{
        "title": #{ "type": "string" },
        "target_path": #{ "type": "string" },
        "audience": #{ "type": "string" },
        "out_of_scope": #{ "type": "array", "maxItems": 12, "items": #{ "type": "string" } },
        "sections": #{
            "type": "array", "maxItems": 20,
            "items": #{
                "type": "object",
                "required": ["heading", "purpose", "must_cover"],
                "properties": #{
                    "heading": #{ "type": "string" },
                    "purpose": #{ "type": "string" },
                    "must_cover": #{
                        "type": "array", "maxItems": 20,
                        "items": #{ "type": "string" },
                    },
                },
            },
        },
        "index_updates": #{
            "type": "array", "maxItems": 8,
            "items": #{ "type": "string" },
        },
    },
};

let draft_schema = #{
    "type": "object",
    "required": ["markdown", "target_path", "claim_checklist"],
    "properties": #{
        "target_path": #{ "type": "string" },
        "markdown": #{ "type": "string" },
        "claim_checklist": #{
            "type": "array", "maxItems": 40,
            "items": #{
                "type": "object",
                "required": ["claim", "source_path"],
                "properties": #{
                    "claim": #{ "type": "string" },
                    "source_path": #{ "type": "string" },
                },
            },
        },
        "open_questions": #{
            "type": "array", "maxItems": 10,
            "items": #{ "type": "string" },
        },
    },
};

let verify_schema = #{
    "type": "object",
    "required": ["verdict", "confirmed", "false_claims", "missing", "suggested_edits"],
    "properties": #{
        "verdict": #{ "type": "string" },
        "confirmed": #{
            "type": "array", "maxItems": 30,
            "items": #{ "type": "string" },
        },
        "false_claims": #{
            "type": "array", "maxItems": 20,
            "items": #{
                "type": "object",
                "required": ["claim", "actual", "evidence"],
                "properties": #{
                    "claim": #{ "type": "string" },
                    "actual": #{ "type": "string" },
                    "evidence": #{ "type": "string" },
                },
            },
        },
        "missing": #{
            "type": "array", "maxItems": 20,
            "items": #{ "type": "string" },
        },
        "suggested_edits": #{
            "type": "array", "maxItems": 20,
            "items": #{ "type": "string" },
        },
        "severity_notes": #{ "type": "string" },
    },
};

let write_schema = #{
    "type": "object",
    "required": ["files_written", "summary", "index_updated"],
    "properties": #{
        "files_written": #{
            "type": "array", "maxItems": 10,
            "items": #{ "type": "string" },
        },
        "index_updated": #{ "type": "boolean" },
        "summary": #{ "type": "string" },
        "bytes_approx": #{ "type": "integer" },
        "warnings": #{
            "type": "array", "maxItems": 10,
            "items": #{ "type": "string" },
        },
    },
};

// ---------------------------------------------------------------------------
// Args
// ---------------------------------------------------------------------------
let target_path = "docs/reference/07-strategy-development.md";
let lang = "de";
if args != () {
    if args.target_path != () { target_path = args.target_path; }
    if args.lang != () { lang = args.lang; }
}

let repo_root = ".";
// Prefer workspace-relative paths; agents run from project root.

// ---------------------------------------------------------------------------
// Phase 1 — Research (parallel code readers)
// ---------------------------------------------------------------------------
phase("Research");
log("Researching strategy surface from code (not alpha/edge)");

let common_rules = "";
common_rules += "READ ONLY. Do not create, edit, or delete any files. ";
common_rules += "Use read_file, grep, list_dir only. Never invent APIs. ";
common_rules += "Code is truth. Quote concrete paths and symbols. ";
common_rules += "Scope is TECHNICAL engine integration for strategies: ";
common_rules += "IStrategy callbacks, indicators, entry orders, exit_intent, ";
common_rules += "DefaultExitPolicy / --sl --tp, registration, params, hot-path rules. ";
common_rules += "OUT OF SCOPE: trading edge, alpha research, market theory, ";
common_rules += "parameter optimization advice, venue ops SOPs. ";
common_rules += "Return structured facts with evidence (file:line or path+symbol). ";

let rjobs = [];

// 1) IStrategy + lifecycle
let p1 = "";
p1 += common_rules;
p1 += "AREA=interface. Read fully: src/strategy/strategy_interface.h, ";
p1 += "src/strategy/strategy_registry.h, src/strategy/strategy_factory.h. ";
p1 += "Document every virtual method, defaults, param_def, get_indicator_values, ";
p1 += "reset for MC, take_pending_exit_intents vs legacy single intent, ";
p1 += "on_fill vs set_position_open. List REGISTER_STRATEGY names by grepping ";
p1 += "REGISTER_STRATEGY in src/strategy/. Note factory/CLI name mapping.";
rjobs.push(#{
    prompt: p1,
    label: "research:interface",
    capability_mode: "read-only",
    agent_type: "explore",
    output_schema: research_schema,
});

// 2) Indicators
let p2 = "";
p2 += common_rules;
p2 += "AREA=indicators. Inventory every header under src/indicator/*.h. ";
p2 += "For each: class/struct name, update API (on_bar/on_tick/push?), warm-up, ";
p2 += "outputs, allocation behavior if noted, and how strategies typically use it. ";
p2 += "Also check if src/strategy/sma.h duplicates indicator/sma. ";
p2 += "Read 2-3 simple strategy .cpp files that use indicators (sma_strategy, ";
p2 += "ma_crossover_strategy, mean_reversion_strategy) and extract the pattern ";
p2 += "of indicator ownership (member fields, update in on_market).";
rjobs.push(#{
    prompt: p2,
    label: "research:indicators",
    capability_mode: "read-only",
    agent_type: "explore",
    output_schema: research_schema,
});

// 3) Exits / TP / SL
let p3 = "";
p3 += common_rules;
p3 += "AREA=exits. Read fully: src/exits/exit_intent.h, exit_manager.h/.cpp, ";
p3 += "default_exit_policy.h/.cpp, bracket_adapter.h. ";
p3 += "Document exit_intent fields, how SL/TP/trailing/time work, multi-intent ";
p3 += "scale-outs, DefaultExitPolicy modes (floor, strategy_only, engine_only, union), ";
p3 += "CLI --exit-policy / --sl / --tp (also check docs/reference/04-flags.md and ";
p3 += "src/bin/main.inc or engine_config). Show how strategy-declared intents ";
p3 += "interact with platform defaults. Note position-reducing closes vs openers. ";
p3 += "Find an example strategy that emits exit_intent (breakout or similar).";
rjobs.push(#{
    prompt: p3,
    label: "research:exits",
    capability_mode: "read-only",
    agent_type: "explore",
    output_schema: research_schema,
});

// 4) Engine dispatch / entry path
let p4 = "";
p4 += common_rules;
p4 += "AREA=engine-dispatch. Grep and read relevant sections in ";
p4 += "src/engine/engine.cpp and engine.h for: on_market/on_tick/on_l2 strategy ";
p4 += "dispatch, order emission, register_strategy_exit_intent, ";
p4 += "execution_bar_delay, fill attribution, dispatch_fill_to_strategy, ";
p4 += "halt gating of strategies, multi-strategy. Also engine_config fields that ";
p4 += "affect strategy authors (bar delay, fill style if any). ";
p4 += "Document the entry order lifecycle a strategy author must understand: ";
p4 += "signal bar vs fill bar, order_event fields needed, side/qty/type.";
rjobs.push(#{
    prompt: p4,
    label: "research:engine",
    capability_mode: "read-only",
    agent_type: "explore",
    output_schema: research_schema,
});

// 5) Minimal strategy pattern + build
let p5 = "";
p5 += common_rules;
p5 += "AREA=authoring-pattern. Read a SIMPLE strategy end-to-end as the ";
p5 += "canonical template (prefer sma_strategy.h/.cpp or ma_crossover). ";
p5 += "Document: includes, class shape, REGISTER_STRATEGY, param schema, ";
p5 += "how orders are built, CMake registration (cmake/Sources.cmake pattern), ";
p5 += "how to run: --strategy name, --param key=value from main.inc / flags docs. ";
p5 += "List existing registered strategy names and which are demos/placeholders. ";
p5 += "Hot-path rules for strategies from AGENTS.md: zero heap on hot path, ";
p5 += "no JSON, no exceptions in tight loops if avoidable, pools already owned by engine.";
rjobs.push(#{
    prompt: p5,
    label: "research:authoring",
    capability_mode: "read-only",
    agent_type: "explore",
    output_schema: research_schema,
});

// 6) Existing docs gap
let p6 = "";
p6 += common_rules;
p6 += "AREA=docs-gap. Read docs/00-INDEX.md, docs/README.md, ";
p6 += "docs/reference/01-instructions.md (strategy sections only), ";
p6 += "docs/reference/02-user-manual.md strategy sections, ";
p6 += "docs/reference/04-flags.md exit/strategy flags, ";
p6 += "docs/reference/06-adaptive-hybrid-strategy.md (note it is strategy-specific), ";
p6 += "docs/architecture/01-target-architecture.md strategy bullets. ";
p6 += "Report what a NEW strategy author still cannot learn from existing docs. ";
p6 += "facts[] should list missing topics (entry API, indicator catalog, exit intents, etc.).";
rjobs.push(#{
    prompt: p6,
    label: "research:docs-gap",
    capability_mode: "read-only",
    agent_type: "explore",
    output_schema: research_schema,
});

let research = parallel(rjobs);

// Collect successful research blobs as JSON text for later prompts
let research_blobs = [];
let research_ok = 0;
let ri = 0;
for r in research {
    if r != () && r.success && r.output != () {
        research_ok += 1;
        research_blobs.push(json_encode(r.output));
    } else {
        log("research slot " + ri.to_string() + " failed or empty");
    }
    ri += 1;
}
if research_ok < 3 {
    pause("verification", "Fewer than 3 research agents succeeded; resume after checking logs or re-run.");
}

log("Research complete: " + research_ok.to_string() + "/6 agents OK");
write_scratch_file("research.json", json_encode(research_blobs));

// ---------------------------------------------------------------------------
// Phase 2 — Outline
// ---------------------------------------------------------------------------
phase("Outline");

let outline_prompt = "";
outline_prompt += "You are the documentation architect for TrueTest core. ";
outline_prompt += "Design the outline for a TECHNICAL strategy development guide. ";
outline_prompt += "Language for the final doc: " + lang + " (use German if lang=de, English if en). ";
outline_prompt += "Target path: " + target_path + ". ";
outline_prompt += "Audience: C++ developers implementing a new IStrategy against this engine. ";
outline_prompt += "NOT for quant edge research. ";
outline_prompt += "Must cover: IStrategy surface, indicators (src/indicator), entry (order_event), ";
outline_prompt += "exit_intent + DefaultExitPolicy + --sl/--tp, registration, params, ";
outline_prompt += "engine dispatch gotchas (bar delay, on_fill), minimal worked example, ";
outline_prompt += "hot-path constraints, how to run a backtest of the strategy. ";
outline_prompt += "Use ONLY facts from the research JSON below. If research conflicts, prefer ";
outline_prompt += "interface/exits/engine areas over docs-gap. ";
outline_prompt += "READ ONLY — do not write files. ";
outline_prompt += "Research payloads (JSON array of area reports):\n";
// Cap research payload size by taking first N blobs if needed
let bi = 0;
for blob in research_blobs {
    if bi < 6 {
        outline_prompt += blob;
        outline_prompt += "\n---\n";
    }
    bi += 1;
}

let outline_res = agent(outline_prompt, #{
    label: "outline",
    capability_mode: "read-only",
    output_schema: outline_schema,
});

if outline_res == () || !outline_res.success || outline_res.output == () {
    pause("verification", "Outline agent failed; resume to retry or inspect.");
}

let outline = outline_res.output;
if outline.target_path != () && outline.target_path != "" {
    target_path = outline.target_path;
}
write_scratch_file("outline.json", json_encode(outline));
log("Outline ready: " + outline.title);

// ---------------------------------------------------------------------------
// Phase 3 — Draft
// ---------------------------------------------------------------------------
phase("Draft");

let draft_prompt = "";
draft_prompt += "Write the FULL markdown document for TrueTest strategy development. ";
draft_prompt += "Language: " + lang + ". ";
draft_prompt += "target_path: " + target_path + ". ";
draft_prompt += "Follow the outline exactly (section order). ";
draft_prompt += "RULES:\n";
draft_prompt += "1. Code is truth — only document what research + these files prove.\n";
draft_prompt += "2. No alpha/edge advice. No 'how to find profitable setups'.\n";
draft_prompt += "3. Include concrete C++ snippets (abbreviated, accurate) for: ";
draft_prompt += "class skeleton, REGISTER_STRATEGY, on_market entry, exit_intent emission, ";
draft_prompt += "indicator member update, get_param_schema/set_param, reset for MC.\n";
draft_prompt += "4. Tables for: IStrategy methods, indicator inventory, exit-policy modes, ";
draft_prompt += "CLI flags relevant to strategies.\n";
draft_prompt += "5. Explicit platform vs strategy SL/TP interaction.\n";
draft_prompt += "6. Link to existing docs (04-flags, 01-instructions, AGENTS.md hot-path) ";
draft_prompt += "instead of duplicating freeze/safety content.\n";
draft_prompt += "7. Mark unknowns as open questions, never invent.\n";
draft_prompt += "8. Start with a short purpose + out-of-scope box.\n";
draft_prompt += "9. End with a minimal checklist: new strategy author definition of done.\n";
draft_prompt += "10. Prefer German technical prose if lang=de; keep identifiers in English as in code.\n";
draft_prompt += "READ ONLY for files — put the full markdown in the schema field 'markdown'. ";
draft_prompt += "You MAY re-read source files with tools to verify snippets before writing.\n";
draft_prompt += "OUTLINE:\n";
draft_prompt += json_encode(outline);
draft_prompt += "\nRESEARCH (use for facts):\n";
let bi2 = 0;
for blob in research_blobs {
    if bi2 < 6 {
        draft_prompt += blob;
        draft_prompt += "\n---\n";
    }
    bi2 += 1;
}

let draft_res = agent(draft_prompt, #{
    label: "draft",
    capability_mode: "read-only",
    output_schema: draft_schema,
});

if draft_res == () || !draft_res.success || draft_res.output == () {
    pause("verification", "Draft agent failed.");
}
if draft_res.output.markdown == () || draft_res.output.markdown == "" {
    pause("verification", "Draft produced empty markdown.");
}

let draft_md = draft_res.output.markdown;
if draft_res.output.target_path != () && draft_res.output.target_path != "" {
    target_path = draft_res.output.target_path;
}
write_scratch_file("draft.md", draft_md);
write_scratch_file("draft_meta.json", json_encode(#{
    target_path: target_path,
    claim_checklist: draft_res.output.claim_checklist,
    open_questions: draft_res.output.open_questions,
}));
log("Draft written to scratch (" + draft_md.len().to_string() + " chars)");

// ---------------------------------------------------------------------------
// Phase 4 — Adversarial verify (parallel skeptics)
// ---------------------------------------------------------------------------
phase("Verify");

let vjobs = [];

let v1 = "";
v1 += "Adversarially fact-check a strategy development doc draft against CODE. ";
v1 += "READ ONLY. Re-read src/strategy/strategy_interface.h, strategy_registry.h, ";
v1 += "and at least one REGISTER_STRATEGY site. ";
v1 += "Focus: IStrategy methods, defaults, registration names, param API. ";
v1 += "Set false_claims for any invented or wrong API. verdict=pass only if no false claims. ";
v1 += "DRAFT:\n";
v1 += draft_md;
vjobs.push(#{
    prompt: v1,
    label: "verify:interface",
    capability_mode: "read-only",
    agent_type: "explore",
    output_schema: verify_schema,
});

let v2 = "";
v2 += "Adversarially fact-check exits/TP/SL section of a strategy doc against CODE. ";
v2 += "READ ONLY. Re-read src/exits/exit_intent.h, default_exit_policy.h/.cpp, ";
v2 += "and engine register_strategy_exit_intent usage. Cross-check --exit-policy modes ";
v2 += "against docs/reference/04-flags.md AND code enums/strings. ";
v2 += "Reject wrong mode names or wrong default SL/TP fractions. ";
v2 += "DRAFT:\n";
v2 += draft_md;
vjobs.push(#{
    prompt: v2,
    label: "verify:exits",
    capability_mode: "read-only",
    agent_type: "explore",
    output_schema: verify_schema,
});

let v3 = "";
v3 += "Adversarially fact-check indicator inventory + entry/fill/bar-delay claims. ";
v3 += "READ ONLY. List actual src/indicator headers; compare to draft inventory. ";
v3 += "Check engine execution_bar_delay and order_event construction patterns ";
v3 += "in a real strategy .cpp. Flag missing indicators or wrong fill semantics. ";
v3 += "DRAFT:\n";
v3 += draft_md;
vjobs.push(#{
    prompt: v3,
    label: "verify:indicators-entry",
    capability_mode: "read-only",
    agent_type: "explore",
    output_schema: verify_schema,
});

let verdicts = parallel(vjobs);

let all_false = [];
let all_missing = [];
let all_edits = [];
let all_confirmed = [];
let verify_ok = 0;
let vi = 0;
for v in verdicts {
    if v != () && v.success && v.output != () {
        verify_ok += 1;
        if v.output.false_claims != () {
            for fc in v.output.false_claims { all_false.push(fc); }
        }
        if v.output.missing != () {
            for m in v.output.missing { all_missing.push(m); }
        }
        if v.output.suggested_edits != () {
            for e in v.output.suggested_edits { all_edits.push(e); }
        }
        if v.output.confirmed != () {
            for c in v.output.confirmed { all_confirmed.push(c); }
        }
    } else {
        log("verify slot " + vi.to_string() + " failed — treat as unverified");
    }
    vi += 1;
}

write_scratch_file("verify.json", json_encode(#{
    verify_ok: verify_ok,
    false_claims: all_false,
    missing: all_missing,
    suggested_edits: all_edits,
    confirmed_count: all_confirmed.len(),
}));

log("Verify: " + verify_ok.to_string() + "/3 panels; false_claims=" + all_false.len().to_string());

// If false claims exist, revise once before write
let final_md = draft_md;
if all_false.len() > 0 || all_edits.len() > 0 {
    log("Revising draft from verification feedback");
    let rev = "";
    rev += "Revise the strategy development markdown. Apply ALL false_claim corrections ";
    rev += "and high-value suggested_edits. Keep language=" + lang + ". ";
    rev += "Do not invent new APIs. Prefer deleting wrong claims over guessing. ";
    rev += "Add missing critical topics if listed. Return full revised markdown. ";
    rev += "READ ONLY tools OK to re-check code. ";
    rev += "FALSE_CLAIMS:\n";
    rev += json_encode(all_false);
    rev += "\nMISSING:\n";
    rev += json_encode(all_missing);
    rev += "\nEDITS:\n";
    rev += json_encode(all_edits);
    rev += "\nDRAFT:\n";
    rev += draft_md;

    let rev_res = agent(rev, #{
        label: "revise",
        capability_mode: "read-only",
        output_schema: draft_schema,
    });
    if rev_res != () && rev_res.success && rev_res.output != () && rev_res.output.markdown != () && rev_res.output.markdown != "" {
        final_md = rev_res.output.markdown;
        write_scratch_file("draft_revised.md", final_md);
        log("Revision applied (" + final_md.len().to_string() + " chars)");
    } else {
        log("Revision failed — publishing original draft with warnings");
    }
}

// ---------------------------------------------------------------------------
// Phase 5 — Write to repo docs
// ---------------------------------------------------------------------------
phase("Write");

let write_prompt = "";
write_prompt += "You are the docs-writer for TrueTest core. DOCS ONLY. ";
write_prompt += "You may ONLY create/edit documentation under docs/**/*.md and optionally ";
write_prompt += "touch docs/00-INDEX.md / docs/README.md / docs/reference/ cross-links. ";
write_prompt += "MUST NOT modify src/, tests/, cmake/, scripts/, AGENTS.md, or any code. ";
write_prompt += "TASK:\n";
write_prompt += "1) Write the full content below to: " + target_path + "\n";
write_prompt += "2) Add a link entry in docs/00-INDEX.md (reference section / item 18 area) ";
write_prompt += "for the new strategy development guide if missing.\n";
write_prompt += "3) If docs/README.md has a reference file list, add the path there too.\n";
write_prompt += "4) Keep existing tone; German doc is fine; identifiers stay English.\n";
write_prompt += "5) After write, verify with read_file that the file exists and starts correctly.\n";
write_prompt += "Return files_written, index_updated, short summary.\n\n";
write_prompt += "MARKDOWN TO WRITE:\n";
write_prompt += final_md;

let write_res = agent(write_prompt, #{
    label: "write-docs",
    capability_mode: "read-write",
    output_schema: write_schema,
});

if write_res == () || !write_res.success || write_res.output == () {
    // Fallback: still persist markdown to scratch for the parent session
    write_scratch_file("FINAL_strategy_dev.md", final_md);
    complete(#{
        status: "partial",
        reason: "write agent failed; full markdown left in workflow scratch FINAL_strategy_dev.md",
        target_path: target_path,
        false_claims: all_false.len(),
        verify_ok: verify_ok,
        research_ok: research_ok,
    });
}

write_scratch_file("FINAL_strategy_dev.md", final_md);

complete(#{
    status: "ok",
    target_path: target_path,
    files_written: write_res.output.files_written,
    index_updated: write_res.output.index_updated,
    summary: write_res.output.summary,
    research_ok: research_ok,
    verify_ok: verify_ok,
    false_claims_found: all_false.len(),
    missing_topics: all_missing.len(),
    lang: lang,
});
