import { useOrderBookState } from '../../store/OrderBookStore';
import { formatPrice } from '../../utils/format';

export function OrderBook() {
  const { bids, asks, spread } = useOrderBookState();

  const maxQty = Math.max(
    ...bids.map((b) => b.quantity),
    ...asks.map((a) => a.quantity),
    1
  );

  if (bids.length === 0 && asks.length === 0) {
    return (
      <div className="flex-1 flex items-center justify-center text-[#787b86] text-xs px-4 text-center">
        Orderbook data not available — enable orderbook snapshots in the engine.
      </div>
    );
  }

  return (
    <div className="flex-1 flex flex-col text-xs overflow-hidden">
      {/* Header */}
      <div className="flex px-3 py-1.5 text-[#787b86] border-b border-[#2a2e39]">
        <span className="flex-1">Price</span>
        <span className="flex-1 text-right">Qty</span>
      </div>

      {/* Asks (reversed so lowest ask is at bottom) */}
      <div className="flex-1 flex flex-col-reverse overflow-y-auto">
        {asks.slice(0, 15).map((level, i) => (
          <div key={`ask-${i}`} className="flex px-3 py-0.5 relative">
            <div
              className="absolute inset-y-0 right-0 bg-[#ef5350] opacity-10"
              style={{ width: `${(level.quantity / maxQty) * 100}%` }}
            />
            <span className="flex-1 text-[#ef5350] relative z-10">{formatPrice(level.price)}</span>
            <span className="flex-1 text-right text-[#d1d4dc] relative z-10">{level.quantity.toFixed(4)}</span>
          </div>
        ))}
      </div>

      {/* Spread */}
      <div className="flex px-3 py-1.5 bg-[#1e222d] text-[#787b86] justify-between border-y border-[#2a2e39]">
        <span>Spread</span>
        <span>{formatPrice(spread)}</span>
      </div>

      {/* Bids */}
      <div className="flex-1 flex flex-col overflow-y-auto">
        {bids.slice(0, 15).map((level, i) => (
          <div key={`bid-${i}`} className="flex px-3 py-0.5 relative">
            <div
              className="absolute inset-y-0 right-0 bg-[#26a69a] opacity-10"
              style={{ width: `${(level.quantity / maxQty) * 100}%` }}
            />
            <span className="flex-1 text-[#26a69a] relative z-10">{formatPrice(level.price)}</span>
            <span className="flex-1 text-right text-[#d1d4dc] relative z-10">{level.quantity.toFixed(4)}</span>
          </div>
        ))}
      </div>
    </div>
  );
}
