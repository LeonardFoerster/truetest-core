import { createContext, useContext, useReducer, type Dispatch, type ReactNode } from 'react';

export type EngineStatus = 'idle' | 'running' | 'paused' | 'halted';

export interface EngineState {
  status: EngineStatus;
  strategy: string;
  symbol: string;
  timeframe: string;
  eventCount: number;
}

type EngineAction =
  | { type: 'SET_STATUS'; status: EngineStatus }
  | { type: 'UPDATE'; data: Partial<EngineState> }
  | { type: 'INCREMENT_EVENTS' }
  | { type: 'RESET' };

const initialState: EngineState = {
  status: 'idle',
  strategy: '',
  symbol: '',
  timeframe: '',
  eventCount: 0,
};

function reducer(state: EngineState, action: EngineAction): EngineState {
  switch (action.type) {
    case 'SET_STATUS':
      return { ...state, status: action.status };
    case 'UPDATE':
      return { ...state, ...action.data };
    case 'INCREMENT_EVENTS':
      return { ...state, eventCount: state.eventCount + 1 };
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
