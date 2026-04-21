# 08 — REST control plane and job queue

## Goal

Expose a **REST API** that accepts backtest jobs, tracks their state, and
returns results. This is what turns the engine from a CLI into a service.

Listed in the original `todo.md` as J3. Still open.

## Context

- The current entry point is `main.cpp`: run-and-exit.
- There is a WebSocket UI server under `ENABLE_WEB_UI` (ws_worker.h),
  but it is a live-dashboard channel, not a job API.
- The C API (`api/truetest_api.h`) is synchronous and in-process; it does
  not address the "submit a job, poll later" workflow.

## Instructions

1. **Daemon mode**: add `./truetest serve [--listen 0.0.0.0:8080] [--workers N]`
   as a new top-level subcommand. Reuses Boost.Beast (already a dep) for
   HTTP.

2. **Job data model**:

   ```cpp
   enum class JobState { queued, running, done, failed, cancelled };

   struct Job {
       std::string id;               // uuid-v4
       std::string tenant_id;        // filled by caller, opaque to engine
       nlohmann::json config;        // engine_config JSON
       JobState state;
       std::chrono::system_clock::time_point created_at;
       std::chrono::system_clock::time_point started_at;
       std::chrono::system_clock::time_point finished_at;
       nlohmann::json result;        // final AnalyticsReport as JSON
       std::string error;
   };
   ```

3. **Endpoints** (all JSON over HTTP):

   ```
   POST   /v1/jobs                  body: { config: {...} }           → 201 { id, state }
   GET    /v1/jobs/{id}                                                 → 200 { state, progress?, result?, error? }
   GET    /v1/jobs?tenant=X&limit=N&since=TS                             → 200 [Job, ...]
   DELETE /v1/jobs/{id}             (cancel)                            → 200 or 409 if already done
   GET    /v1/jobs/{id}/events      (stream)   text/event-stream        → 200 SSE
   GET    /v1/health                                                    → 200 { status:"ok", uptime_s }
   GET    /v1/version                                                   → 200 { version, abi, features: [...] }
   ```

   SSE stream emits the same events that go to the WebSocket UI, filtered
   by job id.

4. **Storage**: reuse the existing `SqliteStore` (data/sqlite_store.h)
   for job persistence. Add two tables:

   ```sql
   CREATE TABLE jobs (
       id TEXT PRIMARY KEY, tenant_id TEXT, state TEXT,
       config_json TEXT, result_json TEXT, error TEXT,
       created_at INTEGER, started_at INTEGER, finished_at INTEGER
   );
   CREATE INDEX idx_jobs_tenant_created ON jobs(tenant_id, created_at DESC);
   ```

5. **Worker pool**: a fixed-size pool of threads pulls queued jobs from
   SQLite, constructs an `engine`, runs it, writes the result back. Jobs
   run serialised within a worker; workers are independent. Each worker
   owns its engine instance — **no global state shared between jobs**.

6. **Cancellation**: set `JobState::cancelled` in the DB and signal the
   engine's existing `halt_flag_`. Worker checks state before each run
   tick; on cancel, tears down gracefully.

7. **Multi-tenancy primitives** (not auth — that's out of scope):
   - Each request carries `X-Tenant-Id` header; pass through as `tenant_id`.
   - Resource limits per job: `cfg.max_runtime_s`, `cfg.max_events`,
     `cfg.max_memory_mb`. Exceeding any halts the job with a clear error.
   - Data-path restrictions: jobs cannot reference absolute filesystem
     paths outside a tenant-scoped data root (`--data-root /var/truetest/data`).

8. **OpenAPI spec**: generate `api/openapi.yaml` describing every endpoint.
   Keep it in sync with the code.

9. **Tests**:
   - `tests/test_control_plane.cpp` — spin up the server on a random port,
     POST a job, poll for completion, assert result contains expected fields.
   - Cancellation test: long-running job, DELETE mid-run, assert state.

## Acceptance criteria

- `./truetest serve` binds a port and responds to `/v1/health`.
- A job submitted via curl completes and `/v1/jobs/{id}` returns a result
  that matches the same config run via CLI (golden regression).
- Cancellation works and leaves the DB in a consistent state.
- Two jobs submitted concurrently do not interfere with each other's results.

## Out of scope

- Authentication / authorization / API keys (separate concern).
- Rate limiting, billing, metering.
- Distributed job queue (cross-process / cross-host). One daemon for now.
- Webhooks / push notifications — polling + SSE only.
