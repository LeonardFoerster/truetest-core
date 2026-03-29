import { useState } from 'react';
import { useWebSocket } from '../contexts/WebSocketContext';
import { useEngineState } from '../store/EngineStore';
import { useMarketState } from '../store/MarketStore';
import { cn } from '../utils/format';

const TIMEFRAMES = ['1s', '5s', '15s', '30s', '1m', '5m', '15m', '1h'] as const;
const SYMBOL_PRESETS = ['btcusdt', 'ethusdt', 'solusdt', 'bnbusdt'] as const;

export function TopBar() {
  const { connectionState, send } = useWebSocket();
  const engine = useEngineState();
  const market = useMarketState();
  const [activeTimeframe, setActiveTimeframe] = useState('1m');
  const [symbolInput, setSymbolInput] = useState('');

  const isConnected = connectionState === 'connected';

  const handleTimeframeChange = (tf: string) => {
    setActiveTimeframe(tf);
    send({ command: 'set_timeframe', timeframe: tf });
  };

  const handleSymbolSubmit = () => {
    const sym = symbolInput.trim().toLowerCase();
    if (sym) {
      send({ command: 'set_symbol', value: sym });
      setSymbolInput('');
    }
  };

  const handleSymbolPreset = (sym: string) => {
    send({ command: 'set_symbol', value: sym });
    setSymbolInput('');
  };

  const handleStrategyChange = (e: React.ChangeEvent<HTMLSelectElement>) => {
    if (e.target.value) {
      send({ command: 'set_strategy', value: e.target.value });
    }
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

      {/* Symbol Display + Input */}
      <div className="flex items-center gap-2">
        <span className="text-sm font-semibold text-[#d1d4dc]">
          {(engine.symbol || market.symbol || 'No Symbol').toUpperCase()}
        </span>
        <div className="flex items-center gap-1">
          {SYMBOL_PRESETS.map((sym) => (
            <button
              key={sym}
              onClick={() => handleSymbolPreset(sym)}
              className={cn(
                'px-1.5 py-0.5 rounded text-[10px] font-medium transition-colors',
                (engine.symbol || '').toLowerCase() === sym
                  ? 'bg-[#2962ff] text-white'
                  : 'text-[#787b86] hover:text-[#d1d4dc] hover:bg-[#1e222d]'
              )}
            >
              {sym.toUpperCase()}
            </button>
          ))}
          <input
            type="text"
            value={symbolInput}
            onChange={(e) => setSymbolInput(e.target.value)}
            onKeyDown={(e) => e.key === 'Enter' && handleSymbolSubmit()}
            placeholder="Custom..."
            className="w-20 bg-[#1e222d] border border-[#2a2e39] rounded px-1.5 py-0.5 text-[10px] text-[#d1d4dc] placeholder-[#787b86] outline-none focus:border-[#2962ff]"
          />
        </div>
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

      {/* Strategy Selector */}
      <div className="flex items-center gap-2">
        <span className="text-xs text-[#787b86]">Strategy:</span>
        {engine.availableStrategies.length > 0 ? (
          <select
            value={engine.strategy || ''}
            onChange={handleStrategyChange}
            className="bg-[#1e222d] border border-[#2a2e39] rounded px-2 py-1 text-xs text-[#d1d4dc] outline-none focus:border-[#2962ff] cursor-pointer"
          >
            {!engine.strategy && <option value="">--</option>}
            {engine.availableStrategies.map((s) => (
              <option key={s} value={s}>{s}</option>
            ))}
          </select>
        ) : (
          <span className="text-sm text-[#d1d4dc]">{engine.strategy || '\u2014'}</span>
        )}
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
