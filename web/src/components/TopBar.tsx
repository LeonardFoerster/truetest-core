import { useState } from 'react';
import { useWebSocket } from '../contexts/WebSocketContext';
import { useEngineState } from '../store/EngineStore';
import { useMarketState } from '../store/MarketStore';
import { cn } from '../utils/format';

const TIMEFRAMES = ['1s', '5s', '15s', '30s', '1m', '5m', '15m', '1h'] as const;

export function TopBar() {
  const { connectionState, send } = useWebSocket();
  const engine = useEngineState();
  const market = useMarketState();
  const [activeTimeframe, setActiveTimeframe] = useState('1m');

  const isConnected = connectionState === 'connected';

  const handleTimeframeChange = (tf: string) => {
    setActiveTimeframe(tf);
    send({ command: 'set_timeframe', timeframe: tf });
  };

  return (
    <div className="h-12 bg-[#161a25] border-b border-[#2a2e39] flex items-center px-4 gap-6 shrink-0">
      {/* Connection Status */}
      <div className="flex items-center gap-2">
        <div className={cn(
          'w-2.5 h-2.5 rounded-full',
          isConnected ? 'bg-[#26a69a]' : 'bg-[#ef5350]'
        )} />
        <span className="text-xs text-[#787b86]">
          {connectionState === 'connected' ? 'Connected' :
           connectionState === 'reconnecting' ? 'Reconnecting...' : 'Disconnected'}
        </span>
      </div>

      {/* Symbol */}
      <div className="flex items-center gap-2">
        <span className="text-sm font-semibold text-[#d1d4dc]">
          {engine.symbol || market.symbol || 'No Symbol'}
        </span>
      </div>

      {/* Timeframe Selector */}
      <div className="flex items-center gap-0.5">
        {TIMEFRAMES.map((tf) => (
          <button
            key={tf}
            onClick={() => handleTimeframeChange(tf)}
            className={cn(
              'px-2 py-1 rounded text-xs font-medium transition-colors',
              activeTimeframe === tf
                ? 'bg-[#2962ff] text-white'
                : 'text-[#787b86] hover:text-[#d1d4dc] hover:bg-[#1e222d]'
            )}
          >
            {tf}
          </button>
        ))}
      </div>

      {/* Strategy */}
      <div className="flex items-center gap-2">
        <span className="text-xs text-[#787b86]">Strategy:</span>
        <span className="text-sm text-[#d1d4dc]">{engine.strategy || '—'}</span>
      </div>

      {/* Engine Controls */}
      <div className="flex items-center gap-1 ml-auto">
        <button
          onClick={() => send({ command: 'start' })}
          disabled={engine.status === 'running'}
          className={cn(
            'px-3 py-1 rounded text-xs font-medium transition-colors',
            engine.status === 'running'
              ? 'bg-[#1e222d] text-[#787b86] cursor-not-allowed'
              : 'bg-[#26a69a] text-white hover:bg-[#2bbd9e]'
          )}
        >
          Play
        </button>
        <button
          onClick={() => send({ command: 'pause' })}
          disabled={engine.status !== 'running'}
          className={cn(
            'px-3 py-1 rounded text-xs font-medium transition-colors',
            engine.status !== 'running'
              ? 'bg-[#1e222d] text-[#787b86] cursor-not-allowed'
              : 'bg-[#2962ff] text-white hover:bg-[#1e4fd8]'
          )}
        >
          Pause
        </button>
        <button
          onClick={() => send({ command: 'stop' })}
          disabled={engine.status === 'idle'}
          className={cn(
            'px-3 py-1 rounded text-xs font-medium transition-colors',
            engine.status === 'idle'
              ? 'bg-[#1e222d] text-[#787b86] cursor-not-allowed'
              : 'bg-[#ef5350] text-white hover:bg-[#d32f2f]'
          )}
        >
          Stop
        </button>
      </div>

      {/* Event Counter */}
      <div className="text-xs text-[#787b86]">
        Events: {engine.eventCount.toLocaleString()}
      </div>
    </div>
  );
}
