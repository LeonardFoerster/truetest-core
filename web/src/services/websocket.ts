export type ConnectionState = 'connected' | 'disconnected' | 'reconnecting';
export type MessageCallback = (data: Record<string, unknown>) => void;
export type StateCallback = (state: ConnectionState) => void;

export class WebSocketService {
  private ws: WebSocket | null = null;
  private url: string;
  private messageListeners: MessageCallback[] = [];
  private stateListeners: StateCallback[] = [];
  private state: ConnectionState = 'disconnected';
  private reconnectTimer: ReturnType<typeof setTimeout> | null = null;
  private reconnectDelay = 1000;
  private maxReconnectDelay = 10000;
  private shouldReconnect = true;

  constructor(url?: string) {
    this.url = url ?? `ws://${window.location.hostname || 'localhost'}:8765`;
  }

  connect(): void {
    this.shouldReconnect = true;
    this.attemptConnect();
  }

  disconnect(): void {
    this.shouldReconnect = false;
    if (this.reconnectTimer) {
      clearTimeout(this.reconnectTimer);
      this.reconnectTimer = null;
    }
    if (this.ws) {
      this.ws.close();
      this.ws = null;
    }
    this.setState('disconnected');
  }

  send(command: Record<string, unknown>): void {
    if (this.ws?.readyState === WebSocket.OPEN) {
      this.ws.send(JSON.stringify(command));
    }
  }

  onMessage(callback: MessageCallback): () => void {
    this.messageListeners.push(callback);
    return () => {
      this.messageListeners = this.messageListeners.filter((cb) => cb !== callback);
    };
  }

  onStateChange(callback: StateCallback): () => void {
    this.stateListeners.push(callback);
    callback(this.state);
    return () => {
      this.stateListeners = this.stateListeners.filter((cb) => cb !== callback);
    };
  }

  getState(): ConnectionState {
    return this.state;
  }

  private attemptConnect(): void {
    try {
      this.ws = new WebSocket(this.url);
    } catch {
      this.scheduleReconnect();
      return;
    }

    this.ws.onopen = () => {
      this.reconnectDelay = 1000;
      this.setState('connected');
    };

    this.ws.onclose = () => {
      this.ws = null;
      if (this.shouldReconnect) {
        this.setState('reconnecting');
        this.scheduleReconnect();
      } else {
        this.setState('disconnected');
      }
    };

    this.ws.onerror = () => {
      this.ws?.close();
    };

    this.ws.onmessage = (event) => {
      try {
        const data = JSON.parse(event.data);
        for (const listener of this.messageListeners) {
          listener(data);
        }
      } catch {
        // ignore parse errors
      }
    };
  }

  private scheduleReconnect(): void {
    if (this.reconnectTimer) return;
    this.reconnectTimer = setTimeout(() => {
      this.reconnectTimer = null;
      this.reconnectDelay = Math.min(this.reconnectDelay * 1.5, this.maxReconnectDelay);
      this.attemptConnect();
    }, this.reconnectDelay);
  }

  private setState(newState: ConnectionState): void {
    if (this.state === newState) return;
    this.state = newState;
    for (const listener of this.stateListeners) {
      listener(newState);
    }
  }
}
