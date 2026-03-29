import { useEngineState } from '../../store/EngineStore';

const statusColors: Record<string, string> = {
  accepted: 'text-[#2962ff]',
  filled: 'text-[#26a69a]',
  rejected: 'text-[#ef5350]',
  error: 'text-[#ef5350]',
};

export function OrdersTab() {
  const { orderResponses } = useEngineState();

  return (
    <div className="h-full flex flex-col">
      <table className="w-full text-xs">
        <thead className="bg-[#161a25] text-[#787b86]">
          <tr>
            <th className="text-left px-3 py-2 font-medium">Time</th>
            <th className="text-left px-3 py-2 font-medium">Order ID</th>
            <th className="text-left px-3 py-2 font-medium">Symbol</th>
            <th className="text-left px-3 py-2 font-medium">Side</th>
            <th className="text-right px-3 py-2 font-medium">Qty</th>
            <th className="text-right px-3 py-2 font-medium">Price</th>
            <th className="text-left px-3 py-2 font-medium">Status</th>
            <th className="text-left px-3 py-2 font-medium">Reason</th>
          </tr>
        </thead>
        <tbody>
          {orderResponses.length === 0 ? (
            <tr>
              <td colSpan={8} className="text-center text-[#787b86] py-6 text-xs">
                No order responses yet.
              </td>
            </tr>
          ) : (
            orderResponses.map((r, i) => (
              <tr key={`${r.orderId}-${r.status}-${i}`} className="border-t border-[#2a2e39] hover:bg-[#1e222d]">
                <td className="px-3 py-1.5 text-[#d1d4dc]">{new Date(r.time).toLocaleTimeString()}</td>
                <td className="px-3 py-1.5 text-[#d1d4dc]">#{r.orderId}</td>
                <td className="px-3 py-1.5 text-[#d1d4dc]">{r.symbol}</td>
                <td className={`px-3 py-1.5 font-medium ${r.side === 'BUY' ? 'text-[#26a69a]' : 'text-[#ef5350]'}`}>
                  {r.side}
                </td>
                <td className="px-3 py-1.5 text-right text-[#d1d4dc]">{r.qty > 0 ? r.qty : ''}</td>
                <td className="px-3 py-1.5 text-right text-[#d1d4dc]">{r.price > 0 ? r.price.toFixed(2) : ''}</td>
                <td className={`px-3 py-1.5 font-medium ${statusColors[r.status] ?? 'text-[#787b86]'}`}>
                  {r.status}
                </td>
                <td className="px-3 py-1.5 text-[#787b86] truncate max-w-[200px]">{r.reason}</td>
              </tr>
            ))
          )}
        </tbody>
      </table>
    </div>
  );
}
