import { createContext, useContext, useReducer, type Dispatch, type ReactNode } from 'react';

export interface AnalyticsState {
  equity: number;
  cumulative_return: number;
  sharpe_ratio: number;
  sortino_ratio: number;
  max_drawdown: number;
  win_rate: number;
  profit_factor: number;
  avg_win: number;
  avg_loss: number;
  total_trades: number;
  equity_history: number[];
  drawdown_history: number[];
}

type AnalyticsAction =
  | { type: 'UPDATE'; data: Partial<AnalyticsState> }
  | { type: 'ADD_EQUITY_POINT'; value: number }
  | { type: 'RESET' };

const initialState: AnalyticsState = {
  equity: 0,
  cumulative_return: 0,
  sharpe_ratio: 0,
  sortino_ratio: 0,
  max_drawdown: 0,
  win_rate: 0,
  profit_factor: 0,
  avg_win: 0,
  avg_loss: 0,
  total_trades: 0,
  equity_history: [],
  drawdown_history: [],
};

function reducer(state: AnalyticsState, action: AnalyticsAction): AnalyticsState {
  switch (action.type) {
    case 'UPDATE':
      return { ...state, ...action.data };
    case 'ADD_EQUITY_POINT':
      return {
        ...state,
        equity_history: [...state.equity_history, action.value],
      };
    case 'RESET':
      return initialState;
    default:
      return state;
  }
}

const AnalyticsStateCtx = createContext<AnalyticsState>(initialState);
const AnalyticsDispatchCtx = createContext<Dispatch<AnalyticsAction>>(() => {});

export function AnalyticsProvider({ children }: { children: ReactNode }) {
  const [state, dispatch] = useReducer(reducer, initialState);
  return (
    <AnalyticsStateCtx.Provider value={state}>
      <AnalyticsDispatchCtx.Provider value={dispatch}>
        {children}
      </AnalyticsDispatchCtx.Provider>
    </AnalyticsStateCtx.Provider>
  );
}

export const useAnalyticsState = () => useContext(AnalyticsStateCtx);
export const useAnalyticsDispatch = () => useContext(AnalyticsDispatchCtx);
