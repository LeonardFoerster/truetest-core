import { createContext, useContext, useEffect, useRef, useState, type ReactNode } from 'react';
import { WebSocketService, type ConnectionState, type MessageCallback } from '../services/websocket';

interface WebSocketContextValue {
  connectionState: ConnectionState;
  send: (command: Record<string, unknown>) => void;
  subscribe: (callback: MessageCallback) => () => void;
}

const WebSocketContext = createContext<WebSocketContextValue | null>(null);

export function WebSocketProvider({ children }: { children: ReactNode }) {
  const serviceRef = useRef<WebSocketService | null>(null);
  const [connectionState, setConnectionState] = useState<ConnectionState>('disconnected');

  if (!serviceRef.current) {
    serviceRef.current = new WebSocketService();
  }

  useEffect(() => {
    const service = serviceRef.current!;
    const unsub = service.onStateChange(setConnectionState);
    service.connect();
    return () => {
      unsub();
      service.disconnect();
    };
  }, []);

  const value: WebSocketContextValue = {
    connectionState,
    send: (cmd) => serviceRef.current?.send(cmd),
    subscribe: (cb) => serviceRef.current?.onMessage(cb) ?? (() => {}),
  };

  return (
    <WebSocketContext.Provider value={value}>
      {children}
    </WebSocketContext.Provider>
  );
}

export function useWebSocket(): WebSocketContextValue {
  const ctx = useContext(WebSocketContext);
  if (!ctx) throw new Error('useWebSocket must be used within WebSocketProvider');
  return ctx;
}
