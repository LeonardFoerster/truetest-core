import { createContext, useContext, useReducer, type Dispatch, type ReactNode } from 'react';

export interface Bar {
  time: number;
  open: number;
  high: number;
  low: number;
  close: number;
  volume: number;
  indicators?: Record<string, number>;
}

export interface MarketState {
  bars: Bar[];
  symbol: string;
  timeframe: string;
}

type MarketAction =
  | { type: 'ADD_BAR'; bar: Bar }
  | { type: 'UPDATE_LAST_BAR'; bar: Bar }
  | { type: 'SET_SYMBOL'; symbol: string }
  | { type: 'SET_TIMEFRAME'; timeframe: string }
  | { type: 'RESET' };

const initialState: MarketState = { bars: [], symbol: '', timeframe: '' };

const MAX_BARS = 2000;

function reducer(state: MarketState, action: MarketAction): MarketState {
  switch (action.type) {
    case 'ADD_BAR': {
      const bars = state.bars;
      const newBar = action.bar;

      // Deduplicate: if last bar has same timestamp, update it instead of appending
      if (bars.length > 0 && bars[bars.length - 1].time === newBar.time) {
        const updated = [...bars];
        updated[updated.length - 1] = newBar;
        return { ...state, bars: updated };
      }

      // Skip bars older than the latest (out-of-order from backfill overlap)
      if (bars.length > 0 && newBar.time < bars[bars.length - 1].time) {
        return state;
      }

      const next = [...bars, newBar];
      return { ...state, bars: next.length > MAX_BARS ? next.slice(-MAX_BARS) : next };
    }
    case 'UPDATE_LAST_BAR': {
      if (state.bars.length === 0) return { ...state, bars: [action.bar] };
      const updated = [...state.bars];
      updated[updated.length - 1] = action.bar;
      return { ...state, bars: updated };
    }
    case 'SET_SYMBOL':
      return { ...state, symbol: action.symbol };
    case 'SET_TIMEFRAME':
      return { ...state, timeframe: action.timeframe };
    case 'RESET':
      return initialState;
    default:
      return state;
  }
}

const MarketStateCtx = createContext<MarketState>(initialState);
const MarketDispatchCtx = createContext<Dispatch<MarketAction>>(() => {});

export function MarketProvider({ children }: { children: ReactNode }) {
  const [state, dispatch] = useReducer(reducer, initialState);
  return (
    <MarketStateCtx.Provider value={state}>
      <MarketDispatchCtx.Provider value={dispatch}>
        {children}
      </MarketDispatchCtx.Provider>
    </MarketStateCtx.Provider>
  );
}

export const useMarketState = () => useContext(MarketStateCtx);
export const useMarketDispatch = () => useContext(MarketDispatchCtx);
