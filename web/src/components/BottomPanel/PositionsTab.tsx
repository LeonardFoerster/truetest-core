import { usePortfolioState } from '../../store/PortfolioStore';
import { formatPrice, formatNumber, cn } from '../../utils/format';

export function PositionsTab() {
  const { positions, cash } = usePortfolioState();

  return (
    <div className="h-full overflow-auto">
      <table className="w-full text-xs">
        <thead className="sticky top-0 bg-[#161a25]">
          <tr className="text-[#787b86]">
            <th className="text-left px-3 py-2 font-medium">Symbol</th>
            <th className="text-right px-3 py-2 font-medium">Qty</th>
            <th className="text-right px-3 py-2 font-medium">Avg Cost</th>
            <th className="text-right px-3 py-2 font-medium">Unrealized PnL</th>
          </tr>
        </thead>
        <tbody>
          {positions.length === 0 ? (
            <tr>
              <td colSpan={4} className="text-center text-[#787b86] py-6">No open positions</td>
            </tr>
          ) : (
            positions.map((pos, i) => (
              <tr key={i} className="border-t border-[#2a2e39] hover:bg-[#1e222d]">
                <td className="px-3 py-1.5 text-[#d1d4dc] font-medium">{pos.symbol}</td>
                <td className="px-3 py-1.5 text-right text-[#d1d4dc]">{formatNumber(pos.qty)}</td>
                <td className="px-3 py-1.5 text-right text-[#d1d4dc]">{formatPrice(pos.cost_basis)}</td>
                <td className={cn(
                  'px-3 py-1.5 text-right font-medium',
                  pos.unrealized_pnl >= 0 ? 'text-[#26a69a]' : 'text-[#ef5350]'
                )}>
                  {pos.unrealized_pnl >= 0 ? '+' : ''}{formatPrice(pos.unrealized_pnl)}
                </td>
              </tr>
            ))
          )}
        </tbody>
      </table>
      <div className="px-3 py-2 border-t border-[#2a2e39] text-xs text-[#787b86]">
        Cash: <span className="text-[#d1d4dc] font-medium">{formatPrice(cash)}</span>
      </div>
    </div>
  );
}
