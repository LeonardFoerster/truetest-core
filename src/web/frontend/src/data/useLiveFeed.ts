/* =========================================================================
   TrueTest — live feed hook.

   Subscribes to the engine's WS /stream, parses each SnapshotFrame, and runs
   it through adaptSnapshot. Auto-reconnects with backoff. If no engine is
   reachable (e.g. `npm run dev` with no backend), it falls back to the bundled
   fixture after a short grace period so the cockpit is still usable offline.
   ========================================================================= */
import { useEffect, useRef, useState } from "react";
import { adaptSnapshot, type LiveData } from "../adapters/snapshot";
import type { SnapshotFrame } from "../wire";
import { fixtureSnapshot } from "../fixtures";
import { authToken } from "./store";

export type FeedStatus = "connecting" | "live" | "disconnected";

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
  const retries = useRef(0);

  useEffect(() => {
    let closed = false;
    let ws: WebSocket | null = null;
    let reconnectTimer: number | undefined;
    let fallbackTimer: number | undefined;

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
        retries.current = 0;
      };
      ws.onmessage = (ev) => {
        try {
          const frame = JSON.parse(ev.data as string) as SnapshotFrame;
          gotLive.current = true;
          setData(adaptSnapshot(frame));
          setOffline(false);
          setStatus("live");
        } catch {
          /* ignore malformed frame */
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
        if (gotLive.current) setStatus("disconnected");
        scheduleReconnect();
      };
    };

    connect();

    // Offline grace: if no live frame arrived, render the fixture so the design
    // is usable without an engine. A later live frame transparently takes over.
    fallbackTimer = window.setTimeout(() => {
      if (!gotLive.current) {
        setData(fixtureSnapshot);
        setOffline(true);
        setStatus("live");
      }
    }, FALLBACK_MS);

    return () => {
      closed = true;
      if (reconnectTimer) window.clearTimeout(reconnectTimer);
      if (fallbackTimer) window.clearTimeout(fallbackTimer);
      try {
        ws?.close();
      } catch {
        /* noop */
      }
    };
  }, []);

  return { status, data, offline };
}
