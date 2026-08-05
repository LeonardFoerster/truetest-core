# Codex Spec — Volume Profile

**ID**: `cyrex-structure-volume-profile`  
**Priority**: P0  
**Reference**: Cryexc Volume Profile — POC, VAH/VAL, HVN/LVN  
**Depends on**: Public trades (same tape as footprint)

---

## 1. Goal

Compute and display a **volume-at-price histogram** for a selectable range (session, fixed lookback, or visible chart range), with standard market-profile statistics used by discretionary traders.

Often shown as a **side histogram** attached to footprint/candles (Cryexc) or as a standalone panel.

---

## 2. User-visible behavior

- Horizontal bars: length ∝ volume at price bin; dual-tone optional (buy vs sell volume).
- Markers:
  - **POC** — point of control (max volume bin)
  - **VAH / VAL** — value area high/low (default 70% of volume around POC)
  - **HVN / LVN** — local maxima / minima (simple peak/trough on histogram)
- Modes: total volume | delta profile | buy/sell split.
- Range: session | last N minutes | selection (phase 2 drag-select).
- Naked POC list (untested POCs from prior sessions) — phase 1.5.

---

## 3. Algorithms

### 3.1 Accumulate

```cpp
struct VpBin {
  double buy = 0, sell = 0;
  double total() const { return buy + sell; }
  double delta() const { return buy - sell; }
};

// for each trade in range:
//   bins[price_to_bin(px)].buy/sell += qty
```

### 3.2 POC

```
poc = argmax_bin(bins[i].total())
```

### 3.3 Value area (70% classic)

```
target = 0.70 * sum(total)
va_low = va_high = poc
covered = bins[poc].total()
while covered < target:
  expand toward neighbor with larger volume (up or down)
  if tie, prefer side with closer volume or both rules documented
  covered += added
VAH = price(va_high top), VAL = price(va_low bottom)
```

Document the exact expand-on-tie rule in code comments; lock it with a golden test.

### 3.4 HVN / LVN (simple)

```
HVN: bin i where total[i] > total[i-1] && total[i] > total[i+1] && total[i] > threshold
LVN: local minima with same
```

Threshold: e.g. `> 0.15 * poc_volume` for HVN to reduce noise.

---

## 4. Model API

```cpp
class VolumeProfileModel {
public:
  void configure(double tick, int tick_multiple);
  void set_range(std::int64_t start_ms, std::int64_t end_ms);
  void on_trade(const CyrexTrade&);      // ignore if outside range / or store all and filter
  void rebuild_from(std::span<const CyrexTrade>);
  struct View {
    std::vector<VpBin> bins; // dense from min_bin..max_bin
    int min_bin, max_bin;
    int poc_bin;
    int va_low_bin, va_high_bin;
    std::vector<int> hvn_bins, lvn_bins;
    double total_volume;
    std::uint64_t version;
  };
  View view() const;
};
```

Prefer: keep a session tape ring; `rebuild_from` when range changes; incremental `on_trade` when range is live session.

---

## 5. UI wiring

| Placement | Detail |
|-----------|--------|
| Footprint side strip | Right of chart, width ~80–120 px |
| Standalone `DeskPanel::volume_profile` | Full histogram + stats table |
| Shared | `VolumeProfileModel` in research store; footprint panel may borrow `view()` |

ImGui draw:

```cpp
for bin in bins:
  y0,y1 = price_to_y
  w = width * (bin.total / max_total)
  draw rect from right edge leftward (or left edge rightward)
mark POC line thicker; shade VA band lightly across chart if overlay mode
```

Stats header: `POC=…  VA=…–…  Vol=…`

---

## 6. Tests

```
TEST(VolumeProfile, PocIsMaxBin)
TEST(VolumeProfile, ValueAreaCoversSeventyPercent)
TEST(VolumeProfile, ValueAreaExpandTieRule)  // golden
TEST(VolumeProfile, DeltaModeBuySellSplit)
TEST(VolumeProfile, HvnLvnLocalExtrema)
TEST(VolumeProfile, RangeFilterExcludesOutsideTrades)
```

---

## 7. Acceptance checklist

- [ ] Profile matches hand-computed histogram on fixture trades.
- [ ] VA ≈ 70% volume (assert `covered/total ∈ [0.70, 0.70+one_bin]`).
- [ ] Panel + optional footprint side strip render.
- [ ] Tick multiple merges bins.
- [ ] Cold path only; gates green.

---

## 8. Non-goals

- TPO letters (see market profile spec).
- Composite multi-day profiles with overnight rules (phase 2).
- Developing / initial balance statistics pack.

---

## 9. Codex task prompt

```
Implement cyrex/market-structure/01-volume-profile.md only.
VolumeProfileModel with POC/VA/HVN/LVN + ImGui panel and optional footprint side strip.
Golden tests for value-area algorithm mandatory.
```
