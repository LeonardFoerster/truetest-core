import { useEffect, useRef } from 'react';
import { createChart, type IChartApi, type ISeriesApi, ColorType, CrosshairMode } from 'lightweight-charts';
import { useMarketState, type Bar } from '../../store/MarketStore';
import { useFillState } from '../../store/FillStore';

const INDICATOR_COLORS = [
  '#2962ff', '#ff6d00', '#ab47bc', '#00bcd4', '#ffeb3b', '#e91e63',
];

export function ChartPanel() {
  const containerRef = useRef<HTMLDivElement>(null);
  const chartRef = useRef<IChartApi | null>(null);
  const candleSeriesRef = useRef<ISeriesApi<'Candlestick'> | null>(null);
  const lineSeriesRef = useRef<ISeriesApi<'Line'> | null>(null);
  const volumeSeriesRef = useRef<ISeriesApi<'Histogram'> | null>(null);
  const indicatorSeriesRef = useRef<Map<string, ISeriesApi<'Line'>>>(new Map());
  const lastBarCountRef = useRef(0);
  const isUserScrollingRef = useRef(false);

  const { bars } = useMarketState();
  const { fills } = useFillState();

  // Initialize chart
  useEffect(() => {
    if (!containerRef.current) return;

    const chart = createChart(containerRef.current, {
      layout: {
        background: { type: ColorType.Solid, color: '#0f1117' },
        textColor: '#787b86',
        fontSize: 11,
      },
      grid: {
        vertLines: { color: '#1e222d' },
        horzLines: { color: '#1e222d' },
      },
      crosshair: {
        mode: CrosshairMode.Normal,
        vertLine: { color: '#2a2e39', labelBackgroundColor: '#2962ff' },
        horzLine: { color: '#2a2e39', labelBackgroundColor: '#2962ff' },
      },
      rightPriceScale: {
        borderColor: '#2a2e39',
        scaleMargins: { top: 0.1, bottom: 0.2 },
      },
      timeScale: {
        borderColor: '#2a2e39',
        timeVisible: true,
        secondsVisible: false,
      },
    });

    chartRef.current = chart;

    // Detect user scrolling to disable auto-scroll
    chart.timeScale().subscribeVisibleLogicalRangeChange(() => {
      if (isUserScrollingRef.current) return;
    });
    containerRef.current.addEventListener('mousedown', () => {
      isUserScrollingRef.current = true;
    });

    // Resize observer
    const ro = new ResizeObserver(() => {
      if (containerRef.current) {
        chart.applyOptions({
          width: containerRef.current.clientWidth,
          height: containerRef.current.clientHeight,
        });
      }
    });
    ro.observe(containerRef.current);

    return () => {
      ro.disconnect();
      chart.remove();
      chartRef.current = null;
      candleSeriesRef.current = null;
      lineSeriesRef.current = null;
      volumeSeriesRef.current = null;
      indicatorSeriesRef.current.clear();
      lastBarCountRef.current = 0;
    };
  }, []);

  // Ensure the correct series type exists and return it
  const ensureSeries = (chart: IChartApi, hasOHLC: boolean) => {
    if (hasOHLC) {
      if (!candleSeriesRef.current) {
        if (lineSeriesRef.current) {
          chart.removeSeries(lineSeriesRef.current);
          lineSeriesRef.current = null;
        }
        candleSeriesRef.current = chart.addCandlestickSeries({
          upColor: '#26a69a',
          downColor: '#ef5350',
          borderDownColor: '#ef5350',
          borderUpColor: '#26a69a',
          wickDownColor: '#ef5350',
          wickUpColor: '#26a69a',
        });
      }
    } else {
      if (!lineSeriesRef.current) {
        if (candleSeriesRef.current) {
          chart.removeSeries(candleSeriesRef.current);
          candleSeriesRef.current = null;
        }
        lineSeriesRef.current = chart.addLineSeries({
          color: '#2962ff',
          lineWidth: 2,
        });
      }
    }
  };

  const ensureVolumeSeries = (chart: IChartApi) => {
    if (!volumeSeriesRef.current) {
      volumeSeriesRef.current = chart.addHistogramSeries({
        priceFormat: { type: 'volume' },
        priceScaleId: 'volume',
      });
      chart.priceScale('volume').applyOptions({
        scaleMargins: { top: 0.8, bottom: 0 },
      });
    }
  };

  // Update data — uses incremental update() for streaming performance
  useEffect(() => {
    const chart = chartRef.current;
    if (!chart || bars.length === 0) return;

    const prevCount = lastBarCountRef.current;
    const newCount = bars.length;
    const isIncremental = newCount >= prevCount && prevCount > 0;

    const hasOHLC = bars.some((b) => b.open !== b.close || b.high !== b.low);
    ensureSeries(chart, hasOHLC);

    if (isIncremental && newCount - prevCount <= 2) {
      // Incremental path: only update the changed bars
      // This covers: new bar appended (count+1) or last bar updated in-place (same count)
      const startIdx = newCount === prevCount ? prevCount - 1 : prevCount;
      for (let i = startIdx; i < newCount; i++) {
        const b = bars[i];
        if (hasOHLC && candleSeriesRef.current) {
          candleSeriesRef.current.update({
            time: b.time as any, open: b.open, high: b.high, low: b.low, close: b.close,
          });
        } else if (lineSeriesRef.current) {
          lineSeriesRef.current.update({ time: b.time as any, value: b.close });
        }

        if (b.volume > 0) {
          ensureVolumeSeries(chart);
          volumeSeriesRef.current!.update({
            time: b.time as any,
            value: b.volume,
            color: b.close >= b.open ? 'rgba(38,166,154,0.3)' : 'rgba(239,83,80,0.3)',
          });
        }

        // Incremental indicator updates
        if (b.indicators) {
          let colorIdx = 0;
          for (const [name, value] of Object.entries(b.indicators)) {
            let series = indicatorSeriesRef.current.get(name);
            if (!series) {
              series = chart.addLineSeries({
                color: INDICATOR_COLORS[colorIdx % INDICATOR_COLORS.length],
                lineWidth: 1,
                title: name,
              });
              indicatorSeriesRef.current.set(name, series);
            }
            series.update({ time: b.time as any, value });
            colorIdx++;
          }
        }
      }
    } else {
      // Full reset path: initial load or chart reset
      if (hasOHLC && candleSeriesRef.current) {
        candleSeriesRef.current.setData(
          bars.map((b) => ({ time: b.time as any, open: b.open, high: b.high, low: b.low, close: b.close }))
        );
      } else if (lineSeriesRef.current) {
        lineSeriesRef.current.setData(
          bars.map((b) => ({ time: b.time as any, value: b.close }))
        );
      }

      // Volume
      if (bars.some((b) => b.volume > 0)) {
        ensureVolumeSeries(chart);
        volumeSeriesRef.current!.setData(
          bars.map((b) => ({
            time: b.time as any,
            value: b.volume,
            color: b.close >= b.open ? 'rgba(38,166,154,0.3)' : 'rgba(239,83,80,0.3)',
          }))
        );
      }

      // Full indicator reset
      const indicatorNames = new Set<string>();
      for (const b of bars) {
        if (b.indicators) {
          for (const key of Object.keys(b.indicators)) indicatorNames.add(key);
        }
      }

      let colorIdx = 0;
      for (const name of indicatorNames) {
        let series = indicatorSeriesRef.current.get(name);
        if (!series) {
          series = chart.addLineSeries({
            color: INDICATOR_COLORS[colorIdx % INDICATOR_COLORS.length],
            lineWidth: 1,
            title: name,
          });
          indicatorSeriesRef.current.set(name, series);
        }
        const data = bars
          .filter((b) => b.indicators?.[name] != null)
          .map((b) => ({ time: b.time as any, value: b.indicators![name] }));
        if (data.length > 0) series.setData(data);
        colorIdx++;
      }

      for (const [name, series] of indicatorSeriesRef.current) {
        if (!indicatorNames.has(name)) {
          chart.removeSeries(series);
          indicatorSeriesRef.current.delete(name);
        }
      }
    }

    // Fill markers on the active price series
    const activeSeries = candleSeriesRef.current ?? lineSeriesRef.current;
    if (activeSeries && fills.length > 0) {
      const markers = fills
        .filter((f) => f.time > 0)
        .map((f) => ({
          time: f.time as any,
          position: f.side === 'buy' ? 'belowBar' as const : 'aboveBar' as const,
          color: f.side === 'buy' ? '#26a69a' : '#ef5350',
          shape: f.side === 'buy' ? 'arrowUp' as const : 'arrowDown' as const,
          text: `${f.side.toUpperCase()} ${f.quantity} @ ${f.price.toFixed(2)}`,
        }))
        .sort((a, b) => (a.time as number) - (b.time as number));
      activeSeries.setMarkers(markers);
    }

    // Auto-scroll to latest
    if (!isUserScrollingRef.current && newCount > prevCount) {
      chart.timeScale().scrollToRealTime();
    }
    lastBarCountRef.current = newCount;
  }, [bars, fills]);

  return (
    <div className="flex-1 bg-[#0f1117] relative min-h-0">
      <div ref={containerRef} className="absolute inset-0" />
      {bars.length === 0 && (
        <div className="absolute inset-0 flex items-center justify-center text-[#787b86] text-sm">
          Waiting for market data...
        </div>
      )}
    </div>
  );
}
