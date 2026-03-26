import { createContext, useContext, useReducer, type Dispatch, type ReactNode } from 'react';

export interface Position {
  symbol: string;
  qty: number;
  cost_basis: number;
  unrealized_pnl: number;
}

export interface PortfolioState {
  cash: number;
  positions: Position[];
  total_trades: number;
}

type PortfolioAction =
  | { type: 'UPDATE'; data: PortfolioState }
  | { type: 'RESET' };

const initialState: PortfolioState = { cash: 0, positions: [], total_trades: 0 };

function reducer(_state: PortfolioState, action: PortfolioAction): PortfolioState {
  switch (action.type) {
    case 'UPDATE':
      return action.data;
    case 'RESET':
      return initialState;
    default:
      return _state;
  }
}

const PortfolioStateCtx = createContext<PortfolioState>(initialState);
const PortfolioDispatchCtx = createContext<Dispatch<PortfolioAction>>(() => {});

export function PortfolioProvider({ children }: { children: ReactNode }) {
  const [state, dispatch] = useReducer(reducer, initialState);
  return (
    <PortfolioStateCtx.Provider value={state}>
      <PortfolioDispatchCtx.Provider value={dispatch}>
        {children}
      </PortfolioDispatchCtx.Provider>
    </PortfolioStateCtx.Provider>
  );
}

export const usePortfolioState = () => useContext(PortfolioStateCtx);
export const usePortfolioDispatch = () => useContext(PortfolioDispatchCtx);
