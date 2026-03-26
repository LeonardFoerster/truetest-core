import { useEffect, useRef } from 'react';
import { useAnalyticsState } from '../../store/AnalyticsStore';
import { formatPercent, cn } from '../../utils/format';

function StatCard({ label, value, colored }: { label: string; value: string; colored?: 'auto' | 'red' }) {
  const isNeg = value.startsWith('-');
  return (
    <div className="bg-[#1e222d] rounded-lg p-3 flex flex-col gap-1">
      <span className="text-[10px] text-[#787b86] uppercase tracking-wider">{label}</span>
      <span className={cn(
        'text-lg font-bold',
        colored === 'red' ? 'text-[#ef5350]' :
        colored === 'auto' ? (isNeg ? 'text-[#ef5350]' : 'text-[#26a69a]') :
        'text-[#d1d4dc]'
      )}>
        {value}
      </span>
    </div>
  );
}

function MiniChart({ data, color, invert }: { data: number[]; color: string; invert?: boolean }) {
  const canvasRef = useRef<HTMLCanvasElement>(null);

  useEffect(() => {
    const canvas = canvasRef.current;
    if (!canvas || data.length < 2) return;
    const ctx = canvas.getContext('2d');
    if (!ctx) return;

    canvas.width = canvas.offsetWidth * 2;
    canvas.height = canvas.offsetHeight * 2;
    ctx.scale(2, 2);

    const w = canvas.offsetWidth;
    const h = canvas.offsetHeight;

    const values = invert ? data.map((v) => -v) : data;
    const min = Math.min(...values);
    const max = Math.max(...values);
    const range = max - min || 1;

    ctx.clearRect(0, 0, w, h);

    // Fill area
    ctx.beginPath();
    ctx.moveTo(0, h);
    for (let i = 0; i < values.length; i++) {
      const x = (i / (values.length - 1)) * w;
      const y = h - ((values[i] - min) / range) * (h - 4) - 2;
      ctx.lineTo(x, y);
    }
    ctx.lineTo(w, h);
    ctx.closePath();
    ctx.fillStyle = color + '20';
    ctx.fill();

    // Line
    ctx.beginPath();
    for (let i = 0; i < values.length; i++) {
      const x = (i / (values.length - 1)) * w;
      const y = h - ((values[i] - min) / range) * (h - 4) - 2;
      if (i === 0) ctx.moveTo(x, y);
      else ctx.lineTo(x, y);
    }
    ctx.strokeStyle = color;
    ctx.lineWidth = 1.5;
    ctx.stroke();
  }, [data, color, invert]);

  return (
    <canvas
      ref={canvasRef}
      className="w-full h-24 bg-[#1e222d] rounded-lg"
    />
  );
}

export function AnalyticsTab() {
  const analytics = useAnalyticsState();

  return (
    <div className="h-full overflow-auto p-3">
      {/* Row 1: Key Stats */}
      <div className="grid grid-cols-4 gap-3 mb-3">
        <StatCard label="Total Return" value={formatPercent(analytics.cumulative_return)} colored="auto" />
        <StatCard label="Sharpe Ratio" value={analytics.sharpe_ratio.toFixed(3)} />
        <StatCard label="Max Drawdown" value={formatPercent(analytics.max_drawdown)} colored="red" />
        <StatCard label="Win Rate" value={formatPercent(analytics.win_rate)} />
      </div>

      {/* Row 2: Charts */}
      <div className="grid grid-cols-2 gap-3 mb-3">
        <div>
          <span className="text-[10px] text-[#787b86] uppercase tracking-wider mb-1 block">Equity Curve</span>
          {analytics.equity_history.length >= 2 ? (
            <MiniChart
              data={analytics.equity_history}
              color={analytics.cumulative_return >= 0 ? '#26a69a' : '#ef5350'}
            />
          ) : (
            <div className="w-full h-24 bg-[#1e222d] rounded-lg flex items-center justify-center text-[#787b86] text-xs">
              Waiting for data...
            </div>
          )}
        </div>
        <div>
          <span className="text-[10px] text-[#787b86] uppercase tracking-wider mb-1 block">Drawdown</span>
          {analytics.drawdown_history.length >= 2 ? (
            <MiniChart data={analytics.drawdown_history} color="#ef5350" invert />
          ) : (
            <div className="w-full h-24 bg-[#1e222d] rounded-lg flex items-center justify-center text-[#787b86] text-xs">
              Waiting for data...
            </div>
          )}
        </div>
      </div>

      {/* Row 3: Additional Stats */}
      <div className="grid grid-cols-4 gap-3">
        <StatCard label="Profit Factor" value={analytics.profit_factor.toFixed(2)} />
        <StatCard label="Avg Win / Loss" value={`${analytics.avg_win.toFixed(2)} / ${analytics.avg_loss.toFixed(2)}`} />
        <StatCard label="Sortino Ratio" value={analytics.sortino_ratio.toFixed(3)} />
        <StatCard label="Total Trades" value={analytics.total_trades.toString()} />
      </div>
    </div>
  );
}
