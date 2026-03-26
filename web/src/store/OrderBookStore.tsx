import { createContext, useContext, useReducer, type Dispatch, type ReactNode } from 'react';

export interface OrderBookLevel {
  price: number;
  quantity: number;
}

export interface OrderBookState {
  bids: OrderBookLevel[];
  asks: OrderBookLevel[];
  spread: number;
}

type OrderBookAction =
  | { type: 'UPDATE'; data: OrderBookState }
  | { type: 'RESET' };

const initialState: OrderBookState = { bids: [], asks: [], spread: 0 };

function reducer(_state: OrderBookState, action: OrderBookAction): OrderBookState {
  switch (action.type) {
    case 'UPDATE':
      return action.data;
    case 'RESET':
      return initialState;
    default:
      return _state;
  }
}

const OrderBookStateCtx = createContext<OrderBookState>(initialState);
const OrderBookDispatchCtx = createContext<Dispatch<OrderBookAction>>(() => {});

export function OrderBookProvider({ children }: { children: ReactNode }) {
  const [state, dispatch] = useReducer(reducer, initialState);
  return (
    <OrderBookStateCtx.Provider value={state}>
      <OrderBookDispatchCtx.Provider value={dispatch}>
        {children}
      </OrderBookDispatchCtx.Provider>
    </OrderBookStateCtx.Provider>
  );
}

export const useOrderBookState = () => useContext(OrderBookStateCtx);
export const useOrderBookDispatch = () => useContext(OrderBookDispatchCtx);
