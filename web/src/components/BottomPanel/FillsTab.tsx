import { useState } from 'react';
import { useFillState, type Fill } from '../../store/FillStore';
import { formatPrice, formatTime, cn } from '../../utils/format';

type SortKey = 'time' | 'symbol' | 'side' | 'quantity' | 'price' | 'commission';
type SortDir = 'asc' | 'desc';

export function FillsTab() {
  const { fills } = useFillState();
  const [sortKey, setSortKey] = useState<SortKey>('time');
  const [sortDir, setSortDir] = useState<SortDir>('desc');

  function toggleSort(key: SortKey) {
    if (sortKey === key) {
      setSortDir(sortDir === 'asc' ? 'desc' : 'asc');
    } else {
      setSortKey(key);
      setSortDir('desc');
    }
  }

  const sorted = [...fills].sort((a, b) => {
    const av = a[sortKey];
    const bv = b[sortKey];
    const cmp = typeof av === 'string' ? av.localeCompare(bv as string) : (av as number) - (bv as number);
    return sortDir === 'asc' ? cmp : -cmp;
  });

  function header(label: string, key: SortKey) {
    return (
      <th
        className="text-left px-3 py-2 font-medium cursor-pointer hover:text-[#d1d4dc] select-none"
        onClick={() => toggleSort(key)}
      >
        {label} {sortKey === key ? (sortDir === 'asc' ? '\u25b2' : '\u25bc') : ''}
      </th>
    );
  }

  return (
    <div className="h-full overflow-auto">
      <table className="w-full text-xs">
        <thead className="sticky top-0 bg-[#161a25] text-[#787b86]">
          <tr>
            {header('Time', 'time')}
            {header('Symbol', 'symbol')}
            {header('Side', 'side')}
            {header('Qty', 'quantity')}
            {header('Price', 'price')}
            {header('Commission', 'commission')}
            <th className="text-right px-3 py-2 font-medium">Total</th>
          </tr>
        </thead>
        <tbody>
          {sorted.length === 0 ? (
            <tr>
              <td colSpan={7} className="text-center text-[#787b86] py-6">No fills yet</td>
            </tr>
          ) : (
            sorted.map((f: Fill) => (
              <tr key={f.id} className="border-t border-[#2a2e39] hover:bg-[#1e222d]">
                <td className="px-3 py-1.5 text-[#787b86]">{formatTime(f.time)}</td>
                <td className="px-3 py-1.5 text-[#d1d4dc]">{f.symbol}</td>
                <td className={cn('px-3 py-1.5 font-medium', f.side === 'buy' ? 'text-[#26a69a]' : 'text-[#ef5350]')}>
                  {f.side.toUpperCase()}
                </td>
                <td className="px-3 py-1.5 text-[#d1d4dc]">{f.quantity}</td>
                <td className="px-3 py-1.5 text-[#d1d4dc]">{formatPrice(f.price)}</td>
                <td className="px-3 py-1.5 text-[#787b86]">{f.commission.toFixed(4)}</td>
                <td className="px-3 py-1.5 text-right text-[#d1d4dc]">{formatPrice(f.quantity * f.price)}</td>
              </tr>
            ))
          )}
        </tbody>
      </table>
    </div>
  );
}
