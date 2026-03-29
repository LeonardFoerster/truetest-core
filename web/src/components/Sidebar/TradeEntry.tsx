import { useState, useEffect, useCallback } from 'react';
import { useWebSocket } from '../../contexts/WebSocketContext';
import { useEngineState } from '../../store/EngineStore';
import { cn } from '../../utils/format';

export function TradeEntry() {
  const { connectionState, send } = useWebSocket();
  const engine = useEngineState();
  const [quantity, setQuantity] = useState('');
  const [price, setPrice] = useState('');
  const [orderType, setOrderType] = useState<'market' | 'limit'>('market');
  const [pendingOrder, setPendingOrder] = useState(false);

  const disabled = connectionState !== 'connected' || engine.status !== 'running';

  // Clear pending state when we receive any order_response
  const latestResponse = engine.orderResponses[0];
  useEffect(() => {
    if (latestResponse && pendingOrder) {
      setPendingOrder(false);
    }
  }, [latestResponse, pendingOrder]);

  // Timeout: clear pending state after 5 seconds
  useEffect(() => {
    if (!pendingOrder) return;
    const timer = setTimeout(() => setPendingOrder(false), 5000);
    return () => clearTimeout(timer);
  }, [pendingOrder]);

  const submitOrder = useCallback((side: 'buy' | 'sell') => {
    if (!quantity || disabled || pendingOrder) return;
    const order: Record<string, unknown> = {
      command: 'order',
      side,
      quantity: parseFloat(quantity),
      type: orderType,
    };
    if (orderType === 'limit' && price) {
      order.price = parseFloat(price);
    }
    send(order);
    setPendingOrder(true);
    setQuantity('');
    setPrice('');
  }, [quantity, price, orderType, disabled, pendingOrder, send]);

  const buttonDisabled = disabled || !quantity || pendingOrder;

  return (
    <div className="p-3 flex flex-col gap-3">
      {/* Order Type */}
      <div className="flex rounded overflow-hidden border border-[#2a2e39]">
        {(['market', 'limit'] as const).map((t) => (
          <button
            key={t}
            onClick={() => setOrderType(t)}
            className={cn(
              'flex-1 py-1.5 text-xs font-medium transition-colors',
              orderType === t
                ? 'bg-[#2962ff] text-white'
                : 'bg-[#1e222d] text-[#787b86] hover:text-[#d1d4dc]'
            )}
          >
            {t.charAt(0).toUpperCase() + t.slice(1)}
          </button>
        ))}
      </div>

      {/* Quantity */}
      <div>
        <label className="text-[10px] text-[#787b86] uppercase tracking-wider mb-1 block">Quantity</label>
        <input
          id="trade-qty-input"
          type="number"
          value={quantity}
          onChange={(e) => setQuantity(e.target.value)}
          placeholder="0.00"
          disabled={disabled}
          className="w-full bg-[#1e222d] border border-[#2a2e39] rounded px-3 py-2 text-sm text-[#d1d4dc] placeholder-[#787b86] outline-none focus:border-[#2962ff] disabled:opacity-50"
        />
      </div>

      {/* Price (limit only) */}
      {orderType === 'limit' && (
        <div>
          <label className="text-[10px] text-[#787b86] uppercase tracking-wider mb-1 block">Price</label>
          <input
            type="number"
            value={price}
            onChange={(e) => setPrice(e.target.value)}
            placeholder="0.00"
            disabled={disabled}
            className="w-full bg-[#1e222d] border border-[#2a2e39] rounded px-3 py-2 text-sm text-[#d1d4dc] placeholder-[#787b86] outline-none focus:border-[#2962ff] disabled:opacity-50"
          />
        </div>
      )}

      {/* Buy/Sell Buttons */}
      <div className="flex gap-2">
        <button
          onClick={() => submitOrder('buy')}
          disabled={buttonDisabled}
          className="flex-1 py-2.5 rounded text-sm font-semibold bg-[#26a69a] text-white hover:bg-[#2bbd9e] disabled:opacity-40 disabled:cursor-not-allowed transition-colors"
        >
          {pendingOrder ? 'Submitting...' : 'Buy'}
        </button>
        <button
          onClick={() => submitOrder('sell')}
          disabled={buttonDisabled}
          className="flex-1 py-2.5 rounded text-sm font-semibold bg-[#ef5350] text-white hover:bg-[#d32f2f] disabled:opacity-40 disabled:cursor-not-allowed transition-colors"
        >
          {pendingOrder ? 'Submitting...' : 'Sell'}
        </button>
      </div>

      {disabled && !pendingOrder && (
        <p className="text-[10px] text-[#787b86] text-center">
          {connectionState !== 'connected' ? 'Disconnected from engine' : 'Engine not running'}
        </p>
      )}
    </div>
  );
}
