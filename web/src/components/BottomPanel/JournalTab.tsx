import { useState, useEffect } from 'react';
import { useFillState, type Fill } from '../../store/FillStore';
import { formatPrice, formatTime, cn } from '../../utils/format';

interface JournalEntry {
  notes: string;
  tags: string[];
}

function loadJournal(): Record<string, JournalEntry> {
  try {
    return JSON.parse(localStorage.getItem('truetest_journal') || '{}');
  } catch {
    return {};
  }
}

function saveJournal(data: Record<string, JournalEntry>) {
  localStorage.setItem('truetest_journal', JSON.stringify(data));
}

interface Trade {
  entry: Fill;
  exit: Fill | null;
  pnl: number;
}

function pairFills(fills: Fill[]): Trade[] {
  const trades: Trade[] = [];
  const sorted = [...fills].sort((a, b) => a.time - b.time);
  const stack: Fill[] = [];

  for (const fill of sorted) {
    if (stack.length > 0 && stack[stack.length - 1].side !== fill.side && stack[stack.length - 1].symbol === fill.symbol) {
      const entry = stack.pop()!;
      const pnl = fill.side === 'sell'
        ? (fill.price - entry.price) * Math.min(fill.quantity, entry.quantity)
        : (entry.price - fill.price) * Math.min(fill.quantity, entry.quantity);
      trades.push({ entry, exit: fill, pnl });
    } else {
      stack.push(fill);
    }
  }
  // Unpaired entries
  for (const entry of stack) {
    trades.push({ entry, exit: null, pnl: 0 });
  }

  return trades.reverse();
}

export function JournalTab() {
  const { fills } = useFillState();
  const [journal, setJournal] = useState<Record<string, JournalEntry>>(loadJournal);
  const [expanded, setExpanded] = useState<string | null>(null);
  const [tagInput, setTagInput] = useState('');

  const trades = pairFills(fills);

  useEffect(() => {
    saveJournal(journal);
  }, [journal]);

  function tradeKey(t: Trade): string {
    return `${t.entry.time}-${t.entry.symbol}-${t.entry.side}`;
  }

  function updateNotes(key: string, notes: string) {
    setJournal((j) => ({ ...j, [key]: { ...j[key], notes, tags: j[key]?.tags ?? [] } }));
  }

  function addTag(key: string) {
    if (!tagInput.trim()) return;
    const entry = journal[key] ?? { notes: '', tags: [] };
    if (!entry.tags.includes(tagInput.trim())) {
      setJournal((j) => ({ ...j, [key]: { ...entry, tags: [...entry.tags, tagInput.trim()] } }));
    }
    setTagInput('');
  }

  function removeTag(key: string, tag: string) {
    const entry = journal[key];
    if (!entry) return;
    setJournal((j) => ({ ...j, [key]: { ...entry, tags: entry.tags.filter((t) => t !== tag) } }));
  }

  return (
    <div className="h-full overflow-auto">
      {trades.length === 0 ? (
        <div className="flex items-center justify-center h-full text-[#787b86] text-xs">
          No trades to journal yet
        </div>
      ) : (
        <div className="divide-y divide-[#2a2e39]">
          {trades.map((trade) => {
            const key = tradeKey(trade);
            const isExpanded = expanded === key;
            const entry = journal[key] ?? { notes: '', tags: [] };

            return (
              <div key={key}>
                <div
                  className="flex items-center px-3 py-2 text-xs cursor-pointer hover:bg-[#1e222d]"
                  onClick={() => setExpanded(isExpanded ? null : key)}
                >
                  <span className={cn('w-10 font-medium', trade.entry.side === 'buy' ? 'text-[#26a69a]' : 'text-[#ef5350]')}>
                    {trade.entry.side.toUpperCase()}
                  </span>
                  <span className="w-20 text-[#d1d4dc]">{trade.entry.symbol}</span>
                  <span className="w-16 text-[#787b86]">{formatTime(trade.entry.time)}</span>
                  <span className="w-20 text-[#d1d4dc]">{formatPrice(trade.entry.price)}</span>
                  <span className="mx-2 text-[#787b86]">{'\u2192'}</span>
                  <span className="w-20 text-[#d1d4dc]">{trade.exit ? formatPrice(trade.exit.price) : '—'}</span>
                  <span className={cn(
                    'ml-auto font-medium',
                    trade.pnl > 0 ? 'text-[#26a69a]' : trade.pnl < 0 ? 'text-[#ef5350]' : 'text-[#787b86]'
                  )}>
                    {trade.exit ? (trade.pnl >= 0 ? '+' : '') + formatPrice(trade.pnl) : 'Open'}
                  </span>
                  {entry.tags.length > 0 && (
                    <div className="flex gap-1 ml-3">
                      {entry.tags.map((tag) => (
                        <span key={tag} className="px-1.5 py-0.5 bg-[#2962ff] bg-opacity-20 text-[#2962ff] rounded text-[10px]">
                          {tag}
                        </span>
                      ))}
                    </div>
                  )}
                  <span className="ml-2 text-[#787b86]">{isExpanded ? '\u25b2' : '\u25bc'}</span>
                </div>

                {isExpanded && (
                  <div className="px-3 pb-3 bg-[#1e222d]">
                    <div className="grid grid-cols-2 gap-4 text-xs text-[#787b86] mb-2 pt-2">
                      <div>Entry: {formatTime(trade.entry.time)} @ {formatPrice(trade.entry.price)} x{trade.entry.quantity}</div>
                      <div>Exit: {trade.exit ? `${formatTime(trade.exit.time)} @ ${formatPrice(trade.exit.price)} x${trade.exit.quantity}` : '—'}</div>
                    </div>
                    <textarea
                      value={entry.notes}
                      onChange={(e) => updateNotes(key, e.target.value)}
                      placeholder="Add notes about this trade..."
                      className="w-full bg-[#161a25] border border-[#2a2e39] rounded px-3 py-2 text-xs text-[#d1d4dc] placeholder-[#787b86] outline-none focus:border-[#2962ff] resize-none h-16 mb-2"
                    />
                    <div className="flex items-center gap-2">
                      <input
                        value={tagInput}
                        onChange={(e) => setTagInput(e.target.value)}
                        onKeyDown={(e) => e.key === 'Enter' && addTag(key)}
                        placeholder="Add tag..."
                        className="bg-[#161a25] border border-[#2a2e39] rounded px-2 py-1 text-xs text-[#d1d4dc] placeholder-[#787b86] outline-none focus:border-[#2962ff] w-32"
                      />
                      <button
                        onClick={() => addTag(key)}
                        className="px-2 py-1 bg-[#2962ff] text-white text-xs rounded hover:bg-[#1e4fd8]"
                      >
                        Add
                      </button>
                      {entry.tags.map((tag) => (
                        <span
                          key={tag}
                          className="px-1.5 py-0.5 bg-[#2962ff] bg-opacity-20 text-[#2962ff] rounded text-[10px] cursor-pointer hover:line-through"
                          onClick={() => removeTag(key, tag)}
                        >
                          {tag} x
                        </span>
                      ))}
                    </div>
                  </div>
                )}
              </div>
            );
          })}
        </div>
      )}
    </div>
  );
}
