import type { SnapshotFrame } from "../wire";

// The server normally publishes at ~10 Hz. Two seconds permits transient
// scheduling jitter while making a silent or replayed stream visibly unsafe.
export const LIVE_FRAME_STALE_MS = 2_000;

export type FreshnessStatus = "connecting" | "live" | "degraded";

export interface AcceptedFrameClock {
  generatedAtMs: number | null;
  receivedAtMs: number;
}

/**
 * Age is deliberately the maximum of source age and receipt age. Receiving
 * the same old frame repeatedly therefore cannot make an engine snapshot look
 * fresh, while silence after a genuinely fresh frame still ages normally.
 */
export function acceptedFrameAgeMs(
  accepted: AcceptedFrameClock,
  nowMs: number,
): number | null {
  if (accepted.generatedAtMs === null || !Number.isFinite(accepted.generatedAtMs))
    return null;

  const sourceAge = Math.max(0, nowMs - accepted.generatedAtMs);
  const receiptAge = Math.max(0, nowMs - accepted.receivedAtMs);
  return Math.max(sourceAge, receiptAge);
}

/** Small state machine shared by the hook and deterministic timer tests. */
export class LiveFeedFreshness {
  private openedAtMs: number | null = null;
  private accepted: AcceptedFrameClock | null = null;
  private malformedSinceAccepted = false;

  markOpen(nowMs: number): void {
    this.openedAtMs = nowMs;
  }

  markAccepted(frame: SnapshotFrame, receivedAtMs: number): void {
    this.accepted = {
      generatedAtMs: frame.generated_at_available
        ? frame.generated_at_ms
        : null,
      receivedAtMs,
    };
    this.malformedSinceAccepted = false;
  }

  markMalformed(): void {
    this.malformedSinceAccepted = true;
  }

  statusAt(nowMs: number): FreshnessStatus {
    if (this.malformedSinceAccepted) return "degraded";
    if (this.accepted) {
      const age = acceptedFrameAgeMs(this.accepted, nowMs);
      return age !== null && age <= LIVE_FRAME_STALE_MS
        ? "live"
        : "degraded";
    }
    if (
      this.openedAtMs !== null &&
      nowMs - this.openedAtMs > LIVE_FRAME_STALE_MS
    ) {
      return "degraded";
    }
    return "connecting";
  }
}
