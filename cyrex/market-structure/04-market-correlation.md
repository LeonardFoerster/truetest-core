# Codex Spec — Market Correlation

**ID**: `cyrex-structure-correlation`  
**Priority**: P2  
**Reference**: Cryexc market correlation (BTC vs majors / optional TradFi)  
**Depends on**: Multi-symbol mid/last price series (not L2)

---

## 1. Goal

Provide a **rolling correlation matrix and pairwise series** so researchers see regime coupling (e.g. BTC–ETH, BTC–SOL, later BTC–NQ).

This is a **slow analytics page** (1–5 s refresh), not a hot microstructure view.

---

## 2. User-visible behavior

- N×N heatmap of Pearson correlation (color −1…+1).
- Symbol multi-select (universe: configured watchlist).
- Window: 15m / 1h / 4h / 24h of returns.
- Return interval: 1s / 5s / 1m sampled mids.
- Pair detail: scatter or dual-line normalized prices + rolling ρ sparkline.
- Status: sample count, last update, missing data warnings.

Cryexc also mixes TradFi — TrueTest MVP is **crypto-only** from existing providers; leave hooks for external mid feed.

---

## 3. Algorithms

### 3.1 Sampling

```cpp
// every sample_interval_ms, for each symbol in universe:
//   mid = (bid+ask)/2 or last trade
//   push to ring buffer prices[sym][t]
```

### 3.2 Returns

```
r[t] = log(p[t] / p[t-1])   // preferred
// or simple: p[t]/p[t-1] - 1
```

Align timestamps by bucket; if a symbol missing bucket, skip pair contribution for that t (pairwise complete observations).

### 3.3 Pearson

```
ρ(x,y) = cov(x,y) / (σx σy)
require n >= min_samples (e.g. 30) else NaN
```

### 3.4 Optional Spearman

Rank correlation — phase 1.5 toggle.

---

## 4. Model API

```cpp
class CorrelationModel {
public:
  void set_universe(std::vector<std::uint32_t> symbol_ids);
  void set_window(std::chrono::milliseconds lookback, std::chrono::milliseconds step);
  void on_mid(std::uint32_t symbol_id, std::int64_t ts_ms, double mid);
  struct MatrixView {
    std::vector<std::string> labels;
    std::vector<double> rho; // row-major n*n
    int n = 0;
    int samples_used = 0;
    std::uint64_t version = 0;
  };
  MatrixView matrix() const;
  std::vector<double> rolling_rho(std::uint32_t a, std::uint32_t b, int points) const;
};
```

---

## 5. Wiring

| Concern | Approach |
|---------|----------|
| Multi-symbol mids | Research worker subscribes to L2 tops or trade lasts for watchlist; **or** poll `orderbook_registry` under lock for N symbols |
| Engine config | Watchlist from CLI `--symbol` list / desk UI list — do not hardcode only BTC |
| Desk | `DeskPanel::correlation` on structure page |
| TradFi | Interface `IExternalMidSource` stub |

**Do not** require strategy to be multi-symbol for research mids — research subscription should be independent when possible. If engine only streams configured symbols, document that universe ⊆ session symbols.

---

## 6. UI

- ImPlot heatmap or custom cell grid with `ρ` text when cell large.
- Click cell → pair chart below.
- Color: diverging palette (down/neutral/up) via theme.

---

## 7. Tests

```
TEST(Correlation, PerfectPositive)
TEST(Correlation, PerfectNegative)
TEST(Correlation, IndependentNearZero)
TEST(Correlation, MinSamplesYieldsNaN)
TEST(Correlation, AlignsSparseBuckets)
TEST(Correlation, LogReturns)
```

Use synthetic price paths (GBM / fixed series).

---

## 8. Acceptance checklist

- [ ] Matrix correct on synthetic fixtures.
- [ ] UI updates on timer without blocking engine.
- [ ] NaN/insufficient data shown honestly.
- [ ] Universe limited to available streams with warning.
- [ ] Gates green.

---

## 9. Non-goals

- Cointegration / lead-lag causality (phase 2).
- PCA factor model.
- Live TradFi broker feeds in MVP.

---

## 10. Codex task prompt

```
Implement cyrex/market-structure/04-market-correlation.md only.
CorrelationModel with log returns + Pearson matrix + ImGui heatmap panel.
Synthetic unit tests for ±1 and insufficient samples.
```
