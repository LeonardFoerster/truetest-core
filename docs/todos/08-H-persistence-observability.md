# H: Persistence, Observability & Hardening (later phases)

**See**: docs/todos/00-OVERVIEW.md for full rules, reference format, maintenance, and mapping. High-level also in thin `docs/governance/03-todo.md`. All QuestDB / persistence work happens on the `database` branch.

Primary reference: `docs/reference/03-db.md` (current authoritative schema + queries + reliability + ritual) and `docs/archive/questdb-multi-week-hardening-guide.md` (historical phased log; most phases landed).

- **H-01** Add `--persist-strict` / hard-fail mode + automatic retry + local checkpoint fallback for QuestDB. (See the multi-week hardening guide — Phase 2. Current: soft-fail default (warning to stderr, persistence disabled, run continues); `--persist-strict` + fallback .ilp wired in engine/main/QuestdbStore/IlpWriter + health surface (strict_mode/fallback_lines); "refuse to start when `--persist` is set but QuestDB is down" documented as follow-up.)
- **H-02** Make structured binary event logging + integrity verification mandatory in live mode.
- **H-03** Implement reliable crash recovery (position + open-order replay from binary logs). (Richer checkpoints; crash-replay golden tests.)
- **H-04** Add Prometheus / metrics export endpoint + structured alerting hooks (halt, large loss, DMS trigger, etc.). (IAlertSink; alerting drill — Go-Live row 6.)
- **H-05** Encrypted credential store + key rotation support. (Demonstrated on ≥10 sessions — Go-Live row 5.)
- **H-06** Automatic tape rotation + offsite / cloud backup helpers.
- Additional (from code/gaps/db + Go-Live): Funding table + `record_funding` present; MC campaigns use only lightweight campaign summary (per-trial full is MC-06); scripts/questdb_* + 45-min soak guide + verify_reconciliation (binary side is manual/placeholder); health surface (pending/dropped/fallback/age); time-based flush (150ms default + `--questdb-flush-ms`).

**Last updated**: 2026-07-03 (split from governance/03-todo.md per TODOS-SPLIT-SPEC; verbatim + branch note + refs to db.md + archive; see 00-OVERVIEW.md).
