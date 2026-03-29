import type { Dispatch } from 'react';

// Dispatch types from each store — we accept the raw dispatch functions
interface Dispatchers {
  market: Dispatch<any>;
  portfolio: Dispatch<any>;
  fill: Dispatch<any>;
  analytics: Dispatch<any>;
  orderbook: Dispatch<any>;
  engine: Dispatch<any>;
  toast?: (message: string, type?: 'info' | 'success' | 'error') => void;
}

let fillIdCounter = 0;
let lastBarTime = 0;
let isBackfilling = false;

export function dispatchMessage(msg: Record<string, any>, dispatchers: Dispatchers): void {
  dispatchers.engine({ type: 'INCREMENT_EVENTS' });

  switch (msg.type) {
    case 'market': {
      const d = msg.data;
      const barTime = d.time ?? Math.floor(Date.now() / 1000);

      // lightweight-charts requires strictly ascending timestamps.
      // Drop bars that would go backwards (can happen on reconnect or
      // out-of-order Binance messages).
      if (barTime < lastBarTime) break;

      const bar = {
        time: barTime,
        open: d.open ?? d.close ?? 0,
        high: d.high ?? d.close ?? 0,
        low: d.low ?? d.close ?? 0,
        close: d.close ?? 0,
        volume: d.volume ?? 0,
        indicators: d.indicators,
      };

      // If the new bar has the same timestamp as the last one, update in place
      // (this happens when the aggregator emits partial bar updates)
      if (lastBarTime === barTime) {
        dispatchers.market({ type: 'UPDATE_LAST_BAR', bar });
      } else {
        dispatchers.market({ type: 'ADD_BAR', bar });
        lastBarTime = barTime;
      }

      if (isBackfilling) {
        dispatchers.engine({ type: 'SET_BACKFILLING', value: false });
        isBackfilling = false;
      }

      if (d.symbol) {
        dispatchers.market({ type: 'SET_SYMBOL', symbol: d.symbol });
      }
      break;
    }

    case 'chart_reset': {
      dispatchers.market({ type: 'RESET' });
      dispatchers.engine({ type: 'SET_BACKFILLING', value: true });
      isBackfilling = true;
      lastBarTime = 0;
      if (msg.data?.timeframe) {
        dispatchers.market({ type: 'SET_TIMEFRAME', timeframe: msg.data.timeframe });
      }
      break;
    }

    case 'fill': {
      const d = msg.data;
      dispatchers.fill({
        type: 'ADD_FILL',
        fill: {
          id: ++fillIdCounter,
          time: d.time ?? Date.now() / 1000,
          symbol: d.symbol ?? '',
          side: d.side ?? 'buy',
          quantity: d.quantity ?? 0,
          price: d.price ?? 0,
          commission: d.commission ?? 0,
        },
      });
      if (dispatchers.toast) {
        const side = (d.side ?? 'buy').toUpperCase();
        dispatchers.toast(
          `${side} ${d.quantity ?? 0} ${d.symbol ?? ''} @ ${(d.price ?? 0).toFixed(2)}`,
          d.side === 'buy' ? 'success' : 'error'
        );
      }
      break;
    }

    case 'portfolio': {
      const d = msg.data;
      dispatchers.portfolio({
        type: 'UPDATE',
        data: {
          cash: d.cash ?? 0,
          positions: d.positions ?? [],
          total_trades: d.total_trades ?? 0,
        },
      });
      break;
    }

    case 'analytics': {
      const d = msg.data;
      dispatchers.analytics({
        type: 'UPDATE',
        data: {
          equity: d.final_equity ?? d.equity ?? 0,
          cumulative_return: d.cumulative_return ?? 0,
          sharpe_ratio: d.sharpe_ratio ?? 0,
          sortino_ratio: d.sortino_ratio ?? 0,
          max_drawdown: d.max_drawdown ?? 0,
          win_rate: d.win_rate ?? 0,
          profit_factor: d.profit_factor ?? 0,
          avg_win: d.avg_win ?? 0,
          avg_loss: d.avg_loss ?? 0,
          total_trades: d.total_trades ?? 0,
        },
      });
      if (d.final_equity != null || d.equity != null) {
        dispatchers.analytics({
          type: 'ADD_EQUITY_POINT',
          value: d.final_equity ?? d.equity,
        });
      }
      break;
    }

    case 'orderbook': {
      const d = msg.data;
      dispatchers.orderbook({
        type: 'UPDATE',
        data: {
          bids: d.bids ?? [],
          asks: d.asks ?? [],
          spread: d.spread ?? 0,
        },
      });
      break;
    }

    case 'fills_history': {
      const fills = (msg.data as any[]).map((f: any, i: number) => ({
        id: f.id ?? i,
        time: f.timestamp ? f.timestamp / 1000 : 0,
        symbol: f.symbol ?? '',
        side: f.side ?? 'buy',
        quantity: f.quantity ?? 0,
        price: f.price ?? 0,
        commission: f.commission ?? 0,
      }));
      dispatchers.fill({ type: 'BULK_ADD', fills });
      break;
    }

    case 'equity_history': {
      const points = msg.data as { timestamp: number; equity: number }[];
      for (const p of points) {
        dispatchers.analytics({ type: 'ADD_EQUITY_POINT', value: p.equity });
      }
      break;
    }

    case 'order_response': {
      const d = msg.data;
      const orderId = d.order_id ?? 0;
      const status = (d.status as string) ?? 'unknown';
      const reason = (d.reason as string) ?? '';
      const side = (d.side as string ?? '').toUpperCase();
      const qty = d.quantity ?? 0;
      const price = d.price ?? 0;
      const symbol = d.symbol ?? '';

      dispatchers.engine({
        type: 'ADD_ORDER_RESPONSE',
        response: { orderId, status, reason, symbol, side, qty, price, time: Date.now() },
      });

      if (dispatchers.toast) {
        switch (status) {
          case 'accepted':
            dispatchers.toast(`Order #${orderId} accepted: ${side} ${qty} ${symbol}`, 'info');
            break;
          case 'filled':
            dispatchers.toast(`Order #${orderId} filled: ${side} ${qty} ${symbol} @ ${price.toFixed(2)}`, 'success');
            break;
          case 'rejected':
            dispatchers.toast(`Order rejected: ${reason}`, 'error');
            break;
          case 'error':
            dispatchers.toast(`Order error: ${reason}`, 'error');
            break;
        }
      }
      break;
    }

    case 'error': {
      const message = (msg.data as any).message ?? 'Unknown engine error';
      if (dispatchers.toast) {
        dispatchers.toast(message, 'error');
      }
      break;
    }

    case 'status': {
      const d = msg.data;
      dispatchers.engine({
        type: 'UPDATE',
        data: {
          status: d.state ?? 'idle',
          strategy: d.strategy ?? '',
          symbol: d.symbol ?? '',
          timeframe: d.timeframe ?? '',
        },
      });
      if (dispatchers.toast && d.state === 'halted') {
        dispatchers.toast('Engine halted by risk manager', 'error');
      }
      break;
    }

    case 'strategies': {
      dispatchers.engine({
        type: 'SET_AVAILABLE_STRATEGIES',
        strategies: msg.data as string[],
      });
      break;
    }
  }
}
