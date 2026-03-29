import { createContext, useContext, useReducer, type Dispatch, type ReactNode } from 'react';

export interface Fill {
  id: number;
  time: number;
  symbol: string;
  side: 'buy' | 'sell';
  quantity: number;
  price: number;
  commission: number;
}

export interface FillState {
  fills: Fill[];
}

type FillAction =
  | { type: 'ADD_FILL'; fill: Fill }
  | { type: 'BULK_ADD'; fills: Fill[] }
  | { type: 'RESET' };

const MAX_FILLS = 1000;
const initialState: FillState = { fills: [] };

function reducer(state: FillState, action: FillAction): FillState {
  switch (action.type) {
    case 'ADD_FILL': {
      const fills = [action.fill, ...state.fills].slice(0, MAX_FILLS);
      return { fills };
    }
    case 'BULK_ADD': {
      const existingIds = new Set(state.fills.map(f => f.id));
      const newFills = action.fills.filter(f => !existingIds.has(f.id));
      const fills = [...newFills, ...state.fills].slice(0, MAX_FILLS);
      return { fills };
    }
    case 'RESET':
      return initialState;
    default:
      return state;
  }
}

const FillStateCtx = createContext<FillState>(initialState);
const FillDispatchCtx = createContext<Dispatch<FillAction>>(() => {});

export function FillProvider({ children }: { children: ReactNode }) {
  const [state, dispatch] = useReducer(reducer, initialState);
  return (
    <FillStateCtx.Provider value={state}>
      <FillDispatchCtx.Provider value={dispatch}>
        {children}
      </FillDispatchCtx.Provider>
    </FillStateCtx.Provider>
  );
}

export const useFillState = () => useContext(FillStateCtx);
export const useFillDispatch = () => useContext(FillDispatchCtx);
