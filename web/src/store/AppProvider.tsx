import { useEffect, type ReactNode } from 'react';
import { WebSocketProvider, useWebSocket } from '../contexts/WebSocketContext';
import { MarketProvider, useMarketDispatch } from './MarketStore';
import { PortfolioProvider, usePortfolioDispatch } from './PortfolioStore';
import { FillProvider, useFillDispatch } from './FillStore';
import { AnalyticsProvider, useAnalyticsDispatch } from './AnalyticsStore';
import { OrderBookProvider, useOrderBookDispatch } from './OrderBookStore';
import { EngineProvider, useEngineDispatch } from './EngineStore';
import { ToastProvider, useToast } from '../components/Toast';
import { dispatchMessage } from './dispatcher';

function MessageRouter({ children }: { children: ReactNode }) {
  const { subscribe } = useWebSocket();
  const market = useMarketDispatch();
  const portfolio = usePortfolioDispatch();
  const fill = useFillDispatch();
  const analytics = useAnalyticsDispatch();
  const orderbook = useOrderBookDispatch();
  const engine = useEngineDispatch();
  const { toast } = useToast();

  useEffect(() => {
    const unsub = subscribe((msg) => {
      dispatchMessage(msg, { market, portfolio, fill, analytics, orderbook, engine, toast });
    });
    return unsub;
  }, [subscribe, market, portfolio, fill, analytics, orderbook, engine, toast]);

  return <>{children}</>;
}

export function AppProvider({ children }: { children: ReactNode }) {
  return (
    <WebSocketProvider>
      <EngineProvider>
        <MarketProvider>
          <PortfolioProvider>
            <FillProvider>
              <AnalyticsProvider>
                <OrderBookProvider>
                  <ToastProvider>
                    <MessageRouter>
                      {children}
                    </MessageRouter>
                  </ToastProvider>
                </OrderBookProvider>
              </AnalyticsProvider>
            </FillProvider>
          </PortfolioProvider>
        </MarketProvider>
      </EngineProvider>
    </WebSocketProvider>
  );
}
