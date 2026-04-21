# 09 — Typed, versioned config schema

## Goal

Replace the current ad-hoc JSON-config loader with a **typed, versioned,
validated** configuration schema. Today the engine silently accepts
configs with missing or wrong-typed fields, fills in defaults, and keeps
running. That is unacceptable for a SaaS control plane.

## Context

- `engine_config.h` is the target struct: ~40 fields spanning modes, fees,
  threading, risk, WS settings, logging, persistence, execution tuning.
- `main.cpp::load_config_file()` uses optional field extraction
  (`if (j.contains(key) && j[key].is_number()) out = j[key].get<double>();`).
  This is permissive by design but masks tenant errors.
- `CLAUDE.md` explicitly states "No external JSON library — snprintf for
  serialization, hand-rolled extraction for parsing." That rule applies
  to event JSON on the hot path; using `nlohmann/json` for **config**
  is already a dep and is fine.

## Instructions

1. **Define the schema in JSON Schema Draft 2020-12** at
   `BacktestEngine/src/config/schema_v1.json`. Every field typed with
   `"type"`, `"enum"`, `"minimum"`, `"maximum"`. Mark required fields
   explicitly. Add `"$id": "tt://config/v1"` and a `version: 1` top-level
   field on the config payload.

2. **Embed the schema into the binary** via a CMake step that converts
   `schema_v1.json` into a C++ `constexpr std::string_view` header
   (`config_schema_embedded.h`). No runtime file IO needed to validate.

3. **Integrate a validator**. Candidates:
   - `nlohmann/json-schema-validator` (single-header, uses nlohmann-json already present). Preferred.
   - Write a minimal validator by hand (only if adding the dep is rejected).

4. **Config loader** — new file `config/config_loader.h/.cpp`:

   ```cpp
   struct ConfigLoadResult {
       bool ok;
       engine_config cfg;
       std::vector<std::string> errors;  // per-field, machine-readable
   };

   ConfigLoadResult load_config(std::string_view json_text);
   ```

   Validation steps:
   1. Parse JSON (fail with a diagnostic if malformed).
   2. Check `version` field. Supported: 1. Unknown = hard fail.
   3. Apply JSON Schema validation. Collect all errors; do not stop at the
      first one.
   4. If valid, populate `engine_config` with typed getters.

5. **Remove the silent-fallback loader** in `main.cpp`. The new loader is
   the only path. A malformed config exits with a non-zero code and prints
   every error.

6. **Versioning policy**:
   - Bump `version` when you add a required field, remove a field, or change
     a field's semantics. Adding optional fields is a compatible change.
   - Maintain `schema_v1.json`, `schema_v2.json`, etc. side by side. The
     loader dispatches on `version`.
   - Provide a `config/migrations/v1_to_v2.cpp` translator when bumping.

7. **CLI integration**:
   - `--config path.json` loads and validates.
   - `--dump-config` emits the resolved config in canonical form (ordered
     keys, defaults materialised).
   - `--print-schema` prints the embedded schema to stdout (for tenant docs).
   - `--validate-only path.json` returns 0 on valid, 1 on invalid, prints
     the error list to stderr as JSON.

8. **Tests** — `tests/test_config_loader.cpp`:
   - Valid minimal config parses.
   - Missing required field fails with the right error code.
   - Wrong-typed field fails (e.g. `"seed": "not-a-number"`).
   - Out-of-range field fails (e.g. `"ws_port": 99999`).
   - Unknown `version` fails.
   - Unknown top-level key fails (strict mode) — use `additionalProperties: false`
     in the schema.

## Acceptance criteria

- `./truetest --validate-only testdata/config_good.json` returns 0.
- `./truetest --validate-only testdata/config_bad.json` returns 1 and prints
  a structured error list.
- `--dump-config` output re-validates cleanly (round-trip).
- The existing CLI behaviour on valid configs is preserved.

## Out of scope

- Secrets management (API keys, DB URLs). Keep in env vars / separate
  secret store.
- GUI config builder.
- Per-field telemetry / usage tracking.
