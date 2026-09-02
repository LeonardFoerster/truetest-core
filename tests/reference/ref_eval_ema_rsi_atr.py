"""Independent reference evaluator for ema_rsi_atr_pullback.

Re-implements EMA / RSI / ATR and the entry conditions directly from
src/indicator/{ema,rsi,atr}.h and
src/strategy/ema_rsi_atr_pullback/ema_rsi_atr_pullback_strategy.cpp.
Shares NO code with the engine, so agreement is independent evidence.
"""
import csv, sys


class EMA:
    def __init__(self, period):
        self.p = period; self.k = 2.0 / (period + 1.0)
        self.sum = 0.0; self.count = 0; self.init = False; self.last = None

    def update(self, px):
        if not self.init:
            self.sum += px; self.count += 1
            if self.count == self.p:
                self.last = self.sum / self.p; self.init = True
                return self.last
            return None
        self.last = px * self.k + self.last * (1.0 - self.k)
        return self.last

    def ready(self):
        return self.last is not None


class RSI:
    def __init__(self, period=14):
        self.p = period; self.prev = 0.0; self.has_prev = False
        self.gs = 0.0; self.ls = 0.0; self.ag = 0.0; self.al = 0.0
        self.count = 0; self.init = False; self.last = None

    def _compute(self):
        if self.al == 0.0:
            self.last = 100.0
        else:
            rs = self.ag / self.al
            self.last = 100.0 - (100.0 / (1.0 + rs))
        return self.last

    def update(self, px):
        if not self.has_prev:
            self.prev = px; self.has_prev = True
            return None
        change = px - self.prev; self.prev = px
        gain = change if change > 0 else 0.0
        loss = -change if change < 0 else 0.0
        if not self.init:
            self.gs += gain; self.ls += loss; self.count += 1
            if self.count == self.p:
                self.ag = self.gs / self.p; self.al = self.ls / self.p
                self.init = True
                return self._compute()
            return None
        dp = float(self.p)
        self.ag = (self.ag * (dp - 1.0) + gain) / dp
        self.al = (self.al * (dp - 1.0) + loss) / dp
        return self._compute()

    def ready(self):
        return self.last is not None


class ATR:
    def __init__(self, period=14):
        self.p = period; self.win = []; self.sum = 0.0
        self.last = None; self.last_close = None; self.init = False

    def update(self, h, l, c):
        prev_c = self.last_close
        tr = (h - l) if prev_c is None else max(h - l, abs(h - prev_c), abs(l - prev_c))
        if not self.init:
            self.win.append(tr); self.sum += tr
            if len(self.win) > self.p:
                self.sum -= self.win.pop(0)
            if len(self.win) == self.p:
                self.last = self.sum / self.p; self.init = True
        else:
            self.last = (self.last * (self.p - 1.0) + tr) / self.p
        self.last_close = c
        return self.last

    def ready(self):
        return self.init and self.last is not None


def evaluate(bars, ema_p=150, rsi_p=14, atr_p=14,
             long_thr=40.0, short_thr=60.0):
    """Yield one dict per bar with indicators and the raw boolean conditions.

    Position/state gating is deliberately NOT modelled: this reports the
    signal a flat strategy would produce, which is what we compare against.
    """
    ema, rsi, atr = EMA(ema_p), RSI(rsi_p), ATR(atr_p)
    prev_rsi = None
    out = []
    for i, b in enumerate(bars):
        o, h, l, c = b['open'], b['high'], b['low'], b['close']
        ema.update(c); atr.update(h, l, c)
        curr = rsi.update(c)
        rec = dict(idx=i, o=o, h=h, l=l, c=c,
                   ema=ema.last, rsi_prev=prev_rsi, rsi=curr, atr=atr.last,
                   long_sig=False, short_sig=False)
        warm = (ema.ready() and rsi.ready() and atr.ready()
                and prev_rsi is not None and curr is not None)
        rec['warm'] = warm
        if warm:
            rec['c_gt_ema'] = c > ema.last
            rec['c_lt_ema'] = c < ema.last
            rec['rp_le_l'] = prev_rsi <= long_thr
            rec['rc_gt_l'] = curr > long_thr
            rec['rp_ge_s'] = prev_rsi >= short_thr
            rec['rc_lt_s'] = curr < short_thr
            rec['long_sig'] = rec['c_gt_ema'] and rec['rp_le_l'] and rec['rc_gt_l']
            rec['short_sig'] = rec['c_lt_ema'] and rec['rp_ge_s'] and rec['rc_lt_s']
            if rec['long_sig'] or rec['short_sig']:
                rec['stop_dist'] = atr.last * 2.0
                rec['stop_price'] = (c - rec['stop_dist']) if rec['long_sig'] \
                                    else (c + rec['stop_dist'])
        if curr is not None:
            prev_rsi = curr
        out.append(rec)
    return out


def load(path):
    bars = []
    with open(path) as f:
        for row in csv.DictReader(f):
            bars.append(dict(open=float(row['open']), high=float(row['high']),
                             low=float(row['low']), close=float(row['close'])))
    return bars


if __name__ == '__main__':
    bars = load(sys.argv[1])
    ema_p = int(sys.argv[2]) if len(sys.argv) > 2 else 150
    rsi_p = int(sys.argv[3]) if len(sys.argv) > 3 else 14
    atr_p = int(sys.argv[4]) if len(sys.argv) > 4 else 14
    for r in evaluate(bars, ema_p, rsi_p, atr_p):
        if r.get('long_sig') or r.get('short_sig'):
            print(f"REF_SIGNAL idx={r['idx']} side={'BUY' if r['long_sig'] else 'SELL'} "
                  f"close={r['c']:.10f} ema={r['ema']:.10f} "
                  f"rsi_prev={r['rsi_prev']:.10f} rsi={r['rsi']:.10f} "
                  f"atr={r['atr']:.10f} stop={r['stop_price']:.10f}")
