import { useState, useEffect, useRef } from 'react';
import { TopBar } from './components/TopBar';
import { ChartPanel } from './components/Chart/ChartPanel';
import { OrderBook } from './components/Sidebar/OrderBook';
import { TradeEntry } from './components/Sidebar/TradeEntry';
import { BottomPanel } from './components/BottomPanel/BottomPanel';
import { useWebSocket } from './contexts/WebSocketContext';
import { useEngineState } from './store/EngineStore';
import { useToast } from './components/Toast';
import { cn } from './utils/format';

function App() {
  const [sidebarOpen, setSidebarOpen] = useState(true);
  const { send, connectionState } = useWebSocket();
  const engine = useEngineState();
  const { toast } = useToast();
  const prevConnectionRef = useRef(connectionState);

  // Toast on connection state changes
  useEffect(() => {
    const prev = prevConnectionRef.current;
    prevConnectionRef.current = connectionState;
    if (prev === connectionState) return;
    if (connectionState === 'connected') toast('Connected to engine', 'success');
    else if (connectionState === 'disconnected' && prev === 'connected') toast('Disconnected from engine', 'error');
  }, [connectionState, toast]);

  // Keyboard shortcuts
  useEffect(() => {
    function onKeyDown(e: KeyboardEvent) {
      // Don't capture when typing in inputs
      if (e.target instanceof HTMLInputElement || e.target instanceof HTMLTextAreaElement) return;

      switch (e.key) {
        case ' ':
          e.preventDefault();
          if (engine.status === 'running') send({ command: 'pause' });
          else send({ command: 'start' });
          break;
        case 'Escape':
          send({ command: 'stop' });
          break;
        case 'b':
        case 'B':
          document.getElementById('trade-qty-input')?.focus();
          break;
        case 's':
        case 'S':
          document.getElementById('trade-qty-input')?.focus();
          break;
      }
    }
    window.addEventListener('keydown', onKeyDown);
    return () => window.removeEventListener('keydown', onKeyDown);
  }, [send, engine.status]);

  return (
    <div className="h-screen flex flex-col bg-[#0f1117]">
      <TopBar />

      {/* Main area: chart + sidebar */}
      <div className="flex flex-1 min-h-0">
        {/* Chart */}
        <div className="flex-1 flex flex-col min-w-0">
          <ChartPanel />
        </div>

        {/* Sidebar toggle */}
        <button
          onClick={() => setSidebarOpen(!sidebarOpen)}
          className="w-5 bg-[#161a25] border-x border-[#2a2e39] flex items-center justify-center text-[#787b86] hover:text-[#d1d4dc] hover:bg-[#1e222d] transition-colors shrink-0"
        >
          <span className="text-xs">{sidebarOpen ? '\u25b6' : '\u25c0'}</span>
        </button>

        {/* Sidebar */}
        <div className={cn(
          'bg-[#161a25] flex flex-col border-l border-[#2a2e39] transition-all shrink-0',
          sidebarOpen ? 'w-72' : 'w-0 overflow-hidden'
        )}>
          <div className="border-b border-[#2a2e39] px-3 py-2">
            <span className="text-xs text-[#787b86] uppercase tracking-wider font-medium">Order Book</span>
          </div>
          <div className="flex-1 min-h-0 flex flex-col">
            <OrderBook />
          </div>

          <div className="border-t border-[#2a2e39]">
            <div className="px-3 py-2 border-b border-[#2a2e39]">
              <span className="text-xs text-[#787b86] uppercase tracking-wider font-medium">Trade</span>
            </div>
            <TradeEntry />
          </div>
        </div>
      </div>

      <BottomPanel />
    </div>
  );
}

export default App;
