/* =========================================================================
   TrueTest — live feed hook.

   Subscribes to the engine's WS /stream, parses each SnapshotFrame, and runs
   it through adaptSnapshot. Auto-reconnects with backoff. If no engine is
   reachable (e.g. `npm run dev` with no backend), it falls back to the bundled
   fixture after a short grace period so the cockpit is still usable offline.
   ========================================================================= */
import { useEffect, useRef, useState } from "react";
import { adaptSnapshot, type LiveData } from "../adapters/snapshot";
import { parseSnapshotFrame } from "../wire";
import { fixtureSnapshot } from "../fixtures";
import { authToken } from "./store";
import {
  LIVE_FRAME_STALE_MS,
  LiveFeedFreshness,
  type FreshnessStatus,
} from "./liveFeedFreshness";

export type FeedStatus = FreshnessStatus | "disconnected";

export interface LiveFeed {
  status: FeedStatus;
  data: LiveData | null;
  offline: boolean; // true when showing the bundled fixture (no engine)
}

const FALLBACK_MS = 2500;
const MAX_BACKOFF_MS = 5000;

export function useLiveFeed(): LiveFeed {
  const [status, setStatus] = useState<FeedStatus>("connecting");
  const [data, setData] = useState<LiveData | null>(null);
  const [offline, setOffline] = useState(false);

  // Refs so the WS handlers always see current values without re-subscribing.
  const gotLive = useRef(false);
  const sawBackend = useRef(false);
  const showingOffline = useRef(false);
  const retries = useRef(0);

  useEffect(() => {
    let closed = false;
    let ws: WebSocket | null = null;
    let reconnectTimer: number | undefined;
    let fallbackTimer: number | undefined;
    let freshnessTimer: number | undefined;
    let transportOpen = false;
    const freshness = new LiveFeedFreshness();

    const wsUrl = () => {
      const proto = window.location.protocol === "https:" ? "wss" : "ws";
      const tok = authToken();
      return `${proto}://${window.location.host}/stream${tok ? "?token=" + encodeURIComponent(tok) : ""}`;
    };

    const scheduleReconnect = () => {
      if (closed) return;
      retries.current += 1;
      const delay = Math.min(MAX_BACKOFF_MS, 500 * retries.current);
      reconnectTimer = window.setTimeout(connect, delay);
    };

    const connect = () => {
      if (closed) return;
      try {
        ws = new WebSocket(wsUrl());
      } catch {
        scheduleReconnect();
        return;
      }
      ws.onopen = () => {
        transportOpen = true;
        retries.current = 0;
        sawBackend.current = true;
        const openedAt = Date.now();
        freshness.markOpen(openedAt);
        if (showingOffline.current) {
          showingOffline.current = false;
          setOffline(false);
          setData(null);
        }
        setStatus(freshness.statusAt(openedAt));
      };
      ws.onmessage = (ev) => {
        try {
          const frame = parseSnapshotFrame(JSON.parse(ev.data as string));
          const receivedAt = Date.now();
          freshness.markAccepted(frame, receivedAt);
          gotLive.current = true;
          showingOffline.current = false;
          setData(adaptSnapshot(frame));
          setOffline(false);
          setStatus(freshness.statusAt(receivedAt));
        } catch (error) {
          sawBackend.current = true;
          freshness.markMalformed();
          showingOffline.current = false;
          setOffline(false);
          setStatus("degraded");
          console.error(error instanceof Error ? error.message : "Rejected malformed SnapshotFrame");
        }
      };
      ws.onerror = () => {
        try {
          ws?.close();
        } catch {
          /* noop */
        }
      };
      ws.onclose = () => {
        if (closed) return;
        transportOpen = false;
        if (gotLive.current || sawBackend.current) setStatus("disconnected");
        scheduleReconnect();
      };
    };

    connect();

    // A WebSocket can remain OPEN while its producer is dead. Re-evaluate
    // freshness independently of callbacks so silence, unavailable source
    // timestamps, and replayed snapshots cannot leave the UI green.
    freshnessTimer = window.setInterval(() => {
      if (closed || showingOffline.current || !transportOpen) return;
      const freshnessStatus = freshness.statusAt(Date.now());
      if (freshnessStatus !== "connecting" || sawBackend.current)
        setStatus(freshnessStatus);
    }, Math.min(500, LIVE_FRAME_STALE_MS / 4));

    // Offline grace: if no live frame arrived, render the fixture so the design
    // is usable without an engine. A later live frame transparently takes over.
    fallbackTimer = window.setTimeout(() => {
      if (!gotLive.current && !sawBackend.current) {
        setData(fixtureSnapshot);
        showingOffline.current = true;
        setOffline(true);
        setStatus("live");
      }
    }, FALLBACK_MS);

    return () => {
      closed = true;
      if (reconnectTimer) window.clearTimeout(reconnectTimer);
      if (fallbackTimer) window.clearTimeout(fallbackTimer);
      if (freshnessTimer) window.clearInterval(freshnessTimer);
      try {
        ws?.close();
      } catch {
        /* noop */
      }
    };
  }, []);

  return { status, data, offline };
}
