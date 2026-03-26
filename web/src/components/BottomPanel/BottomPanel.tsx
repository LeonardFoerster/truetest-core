import { useState, useRef, useCallback } from 'react';
import { PositionsTab } from './PositionsTab';
import { FillsTab } from './FillsTab';
import { OrdersTab } from './OrdersTab';
import { AnalyticsTab } from './AnalyticsTab';
import { JournalTab } from './JournalTab';
import { cn } from '../../utils/format';

const TABS = ['Positions', 'Fills', 'Orders', 'Analytics', 'Journal'] as const;
type Tab = typeof TABS[number];

export function BottomPanel() {
  const [activeTab, setActiveTab] = useState<Tab>('Positions');
  const [height, setHeight] = useState(256);
  const dragRef = useRef<{ startY: number; startH: number } | null>(null);

  const onMouseDown = useCallback((e: React.MouseEvent) => {
    e.preventDefault();
    dragRef.current = { startY: e.clientY, startH: height };

    const onMouseMove = (ev: MouseEvent) => {
      if (!dragRef.current) return;
      const delta = dragRef.current.startY - ev.clientY;
      setHeight(Math.max(100, Math.min(600, dragRef.current.startH + delta)));
    };

    const onMouseUp = () => {
      dragRef.current = null;
      document.removeEventListener('mousemove', onMouseMove);
      document.removeEventListener('mouseup', onMouseUp);
    };

    document.addEventListener('mousemove', onMouseMove);
    document.addEventListener('mouseup', onMouseUp);
  }, [height]);

  return (
    <div className="bg-[#161a25] border-t border-[#2a2e39] flex flex-col shrink-0" style={{ height }}>
      {/* Drag handle */}
      <div
        className="h-1 cursor-row-resize hover:bg-[#2962ff] transition-colors shrink-0"
        onMouseDown={onMouseDown}
      />

      {/* Tab bar */}
      <div className="flex border-b border-[#2a2e39] shrink-0">
        {TABS.map((tab) => (
          <button
            key={tab}
            onClick={() => setActiveTab(tab)}
            className={cn(
              'px-4 py-2 text-xs font-medium transition-colors border-b-2',
              activeTab === tab
                ? 'text-[#2962ff] border-[#2962ff]'
                : 'text-[#787b86] border-transparent hover:text-[#d1d4dc]'
            )}
          >
            {tab}
          </button>
        ))}
      </div>

      {/* Tab content */}
      <div className="flex-1 min-h-0 overflow-hidden">
        {activeTab === 'Positions' && <PositionsTab />}
        {activeTab === 'Fills' && <FillsTab />}
        {activeTab === 'Orders' && <OrdersTab />}
        {activeTab === 'Analytics' && <AnalyticsTab />}
        {activeTab === 'Journal' && <JournalTab />}
      </div>
    </div>
  );
}
