import { createContext, useContext, useReducer, type Dispatch, type ReactNode } from 'react';

export type EngineStatus = 'idle' | 'running' | 'paused' | 'halted';

export interface OrderResponse {
  orderId: number;
  status: 'accepted' | 'filled' | 'rejected' | 'error';
  reason: string;
  symbol: string;
  side: string;
  qty: number;
  price: number;
  time: number;
}

export interface EngineState {
  status: EngineStatus;
  strategy: string;
  symbol: string;
  timeframe: string;
  eventCount: number;
  orderResponses: OrderResponse[];
  availableStrategies: string[];
  backfilling: boolean;
}

type EngineAction =
  | { type: 'SET_STATUS'; status: EngineStatus }
  | { type: 'UPDATE'; data: Partial<EngineState> }
  | { type: 'INCREMENT_EVENTS' }
  | { type: 'ADD_ORDER_RESPONSE'; response: OrderResponse }
  | { type: 'SET_AVAILABLE_STRATEGIES'; strategies: string[] }
  | { type: 'SET_BACKFILLING'; value: boolean }
  | { type: 'RESET' };

const MAX_ORDER_RESPONSES = 100;

const initialState: EngineState = {
  status: 'idle',
  strategy: '',
  symbol: '',
  timeframe: '',
  eventCount: 0,
  orderResponses: [],
  availableStrategies: [],
  backfilling: false,
};

function reducer(state: EngineState, action: EngineAction): EngineState {
  switch (action.type) {
    case 'SET_STATUS':
      return { ...state, status: action.status };
    case 'UPDATE':
      return { ...state, ...action.data };
    case 'INCREMENT_EVENTS':
      return { ...state, eventCount: state.eventCount + 1 };
    case 'ADD_ORDER_RESPONSE': {
      const orderResponses = [action.response, ...state.orderResponses].slice(0, MAX_ORDER_RESPONSES);
      return { ...state, orderResponses };
    }
    case 'SET_AVAILABLE_STRATEGIES':
      return { ...state, availableStrategies: action.strategies };
    case 'SET_BACKFILLING':
      return { ...state, backfilling: action.value };
    case 'RESET':
      return initialState;
    default:
      return state;
  }
}

const EngineStateCtx = createContext<EngineState>(initialState);
const EngineDispatchCtx = createContext<Dispatch<EngineAction>>(() => {});

export function EngineProvider({ children }: { children: ReactNode }) {
  const [state, dispatch] = useReducer(reducer, initialState);
  return (
    <EngineStateCtx.Provider value={state}>
      <EngineDispatchCtx.Provider value={dispatch}>
        {children}
      </EngineDispatchCtx.Provider>
    </EngineStateCtx.Provider>
  );
}

export const useEngineState = () => useContext(EngineStateCtx);
export const useEngineDispatch = () => useContext(EngineDispatchCtx);
