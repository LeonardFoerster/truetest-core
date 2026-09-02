import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import test from "node:test";

import {
  SNAPSHOT_SCHEMA_VERSION,
  SNAPSHOT_FUTURE_TOLERANCE_MS,
  SnapshotContractError,
  parseSnapshotFrame,
} from "../src/wire.ts";
import {
  LIVE_FRAME_STALE_MS,
  LiveFeedFreshness,
  acceptedFrameAgeMs,
} from "../src/data/liveFeedFreshness.ts";

const readSnapshot = () => JSON.parse(
  readFileSync(new URL("../src/fixtures/snapshot.json", import.meta.url), "utf8"),
);

test("accepts the current snapshot schema", () => {
  const frame = readSnapshot();
  assert.equal(frame.schema_version, SNAPSHOT_SCHEMA_VERSION);
  assert.equal(parseSnapshotFrame(frame), frame);
});

test("rejects an older snapshot schema with an actionable message", () => {
  assert.throws(
    () => parseSnapshotFrame({ schema_version: 2 }),
    (error) =>
      error instanceof SnapshotContractError &&
      error.message === "Rejected SnapshotFrame schema_version 2; expected 3",
  );
});

test("rejects missing or non-object snapshot payloads", () => {
  assert.throws(
    () => parseSnapshotFrame({}),
    /Rejected SnapshotFrame schema_version undefined; expected 3/,
  );
  assert.throws(
    () => parseSnapshotFrame(null),
    /Rejected SnapshotFrame: expected a JSON object/,
  );
});

test("rejects a v3 frame whose availability pair is contradictory", () => {
  const frame = readSnapshot();
  frame.account.equity = null;
  frame.account.equity_available = true;
  assert.throws(
    () => parseSnapshotFrame(frame),
    /account\.equity\/equity_available must be a consistent number\/true or null\/false pair/,
  );
});

test("rejects contradictory or materially future generation timestamps", () => {
  const contradictory = readSnapshot();
  contradictory.generated_at_ms = null;
  contradictory.generated_at_available = true;
  assert.throws(
    () => parseSnapshotFrame(contradictory),
    /snapshot\.generated_at_ms\/generated_at_available must be a consistent number\/true or null\/false pair/,
  );

  const future = readSnapshot();
  future.generated_at_ms = Date.now() + SNAPSHOT_FUTURE_TOLERANCE_MS + 10_000;
  future.generated_at_available = true;
  assert.throws(
    () => parseSnapshotFrame(future),
    /snapshot\.generated_at_ms must be no more than 5000 ms in the future/,
  );
});

test("freshness age is bounded by both source age and receipt silence", () => {
  assert.equal(
    acceptedFrameAgeMs({ generatedAtMs: 10_000, receivedAtMs: 11_500 }, 12_000),
    2_000,
  );
  assert.equal(
    acceptedFrameAgeMs({ generatedAtMs: 11_900, receivedAtMs: 10_000 }, 12_500),
    2_500,
  );
  assert.equal(
    acceptedFrameAgeMs({ generatedAtMs: null, receivedAtMs: 10_000 }, 12_500),
    null,
  );
});

test("replayed, silent, unavailable, and malformed-only streams degrade", () => {
  const base = Date.now() - 60_000;
  const frame = readSnapshot();
  frame.generated_at_ms = base;
  frame.generated_at_available = true;
  const parsed = parseSnapshotFrame(frame);

  const replayed = new LiveFeedFreshness();
  replayed.markOpen(base);
  replayed.markAccepted(parsed, base);
  assert.equal(replayed.statusAt(base + LIVE_FRAME_STALE_MS), "live");
  // Receipt of the same payload at this later time must not reset source age.
  replayed.markAccepted(parsed, base + LIVE_FRAME_STALE_MS + 1);
  assert.equal(replayed.statusAt(base + LIVE_FRAME_STALE_MS + 1), "degraded");

  const silent = new LiveFeedFreshness();
  silent.markOpen(base);
  assert.equal(silent.statusAt(base + LIVE_FRAME_STALE_MS), "connecting");
  assert.equal(silent.statusAt(base + LIVE_FRAME_STALE_MS + 1), "degraded");

  const unavailable = readSnapshot();
  const unavailableClock = new LiveFeedFreshness();
  unavailableClock.markOpen(base);
  unavailableClock.markAccepted(parseSnapshotFrame(unavailable), base);
  assert.equal(unavailableClock.statusAt(base), "degraded");

  const malformedOnly = new LiveFeedFreshness();
  malformedOnly.markOpen(base);
  malformedOnly.markMalformed();
  assert.equal(malformedOnly.statusAt(base), "degraded");
});
