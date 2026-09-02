"""Golden dataset (F-01 regression, docs/todos/11-F-forensic-lifecycle-audit.md):

Regenerate with:  python3 tests/reference/make_golden_f01.py tests/reference

Golden dataset: one clean LONG pullback entry followed by a strictly
rising market that NEVER trades at or below the designed stop level."""
import sys, datetime
sys.path.insert(0, sys.argv[1] if len(sys.argv) > 1 else '.')
from ref_eval_ema_rsi_atr import evaluate

EMA_P, RSI_P, ATR_P = 20, 5, 5

closes = []
# Phase 1: no trend — tight oscillation, keeps ATR small.
for i in range(30):
    closes.append(7000.0 + (0.6 if i % 2 == 0 else -0.6))
# Phase 2: trend formation — steady rise so close sits above EMA-20.
for i in range(20):
    closes.append(closes[-1] + 1.2)
# Phase 3: pullback — three down bars to push RSI-5 under 40 while the
# lagging EMA-20 stays below price.
for i in range(3):
    closes.append(closes[-1] - 2.2)
# Phase 4: confirmation bar — RSI crosses back over 40 -> LONG signal.
closes.append(closes[-1] + 2.6)
# Phase 5: strictly rising aftermath. Nothing here can reach a stop that
# sits below the entry.
for i in range(25):
    closes.append(closes[-1] + 1.5)

bars = []
t0 = datetime.datetime(2024, 1, 1, tzinfo=datetime.timezone.utc)
prev_close = None
for i, c in enumerate(closes):
    o = prev_close if prev_close is not None else c
    h = max(o, c) + 0.35
    l = min(o, c) - 0.35
    bars.append(dict(open=o, high=h, low=l, close=c,
                     ts=t0 + datetime.timedelta(minutes=i)))
    prev_close = c

res = evaluate([dict(open=b['open'], high=b['high'], low=b['low'], close=b['close'])
                for b in bars], EMA_P, RSI_P, ATR_P)
sigs = [r for r in res if r.get('long_sig') or r.get('short_sig')]
print("reference signals:", [(r['idx'], 'BUY' if r['long_sig'] else 'SELL') for r in sigs])
for r in sigs:
    print(f"  idx={r['idx']} close={r['c']:.4f} ema={r['ema']:.4f} "
          f"rsi_prev={r['rsi_prev']:.4f} rsi={r['rsi']:.4f} atr={r['atr']:.4f} "
          f"stop={r['stop_price']:.4f}")
    lows_after = [b['low'] for b in bars[r['idx']:]]
    print(f"  min low from signal bar onward = {min(lows_after):.4f} "
          f"(stop = {r['stop_price']:.4f}) -> stop "
          f"{'NEVER touched' if min(lows_after) > r['stop_price'] else 'TOUCHED'}")

out = sys.argv[2] if len(sys.argv) > 2 else 'golden.csv'
with open(out, 'w') as f:
    f.write("date,open_time,open,high,low,close,volume\n")
    for b in bars:
        ms = int(b['ts'].timestamp() * 1000)
        f.write(f"{b['ts'].strftime('%Y-%m-%d')},{ms},{b['open']:.4f},"
                f"{b['high']:.4f},{b['low']:.4f},{b['close']:.4f},100\n")
print("wrote", out, len(bars), "bars")
