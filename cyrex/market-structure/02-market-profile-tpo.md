# Codex Spec — Market Profile / TPO

**ID**: `cyrex-structure-tpo`  
**Priority**: P1  
**Reference**: Cryexc Market Profile / TPO — letters/blocks, POC, value area, single prints  
**Depends on**: Time series of mid/last or OHLC; optional volume profile sharing

---

## 1. Goal

Build a **Time-Price Opportunity (TPO)** / Market Profile chart: for each time period (e.g. 30 minutes), mark which price bins the market traded through, forming the classic “bell” profile with letters A, B, C, …

This answers “where did time get spent?” vs volume profile’s “where did size trade?”.

---

## 2. User-visible behavior

- Session chart: vertical price, horizontal TPO letters per period.
- Letter sequence A–Z then a–z (or blocks) for each bracketing period.
- **Initial Balance (IB)**: first N periods (default 2 × period) high/low lines.
- **POC**: price with most TPO count (time); optional dual POC with volume.
- **Value area**: 70% of TPO counts (same expand algorithm as volume profile).
- **Single prints**: bins with only one letter — highlight as poor structure / magnets.
- **Wide points / tails**: visual emphasis.
- Controls: period (15m/30m), session template (24/7 crypto default UTC day or custom 00:00 exchange), tick multiple, show IB, show singles.

Crypto note: no RTH/ETH unless user defines session split. Default **UTC rolling day** or **exchange funding-day** — make selectable.

---

## 3. Data inputs

MVP (simplest correct):

- On each trade or 1s mid sample: mark `bin` as touched in current TPO period.

Better:

- Use bar high-low range: for period aggregate, mark all bins from low→high as touched (range TPO). Cryexc-style continuous markets often use trade prints; **document choice**.

Recommended for TrueTest:

```
Mode A (print): each public trade sets letter at price bin
Mode B (range): each 1s last price; OR each period uses OHLC range fill
Default: Mode A for parity with footprint; optional Mode B toggle
```

---

## 4. Algorithms

```cpp
struct TpoPeriod {
  std::int64_t start_ms;
  char letter; // 'A'.. 
  std::unordered_set<int> bins_touched; // or bitset dense
};

class TpoModel {
  void configure(double tick, int multiple, int period_ms, int ib_periods);
  void on_print(std::int64_t ts_ms, double price);
  void on_range(std::int64_t ts_ms, double high, double low); // mode B
  // derived:
  // count[bin] = number of periods that touched bin
  // poc = argmax count
  // value area on counts
  // single_prints = bins where count==1 (and maybe not in IB extremes only — define)
};
```

### Letter assignment

```
period_index = (ts - session_start) / period_ms
letter = letters[period_index]  // "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
```

If session exceeds letter alphabet, wrap with AA style or extend — document; for 30m crypto day, 48 periods → need dual char after 52. Implement `period_label(i) -> string`.

### Single prints

```
single = count[bin] == 1 && bin between session high/low
```

Often singles in the middle of the profile matter more than at extremes — MVP highlight all singles.

---

## 5. Render (ImGui)

Classic layout:

```
price
  |  A
  |  AB
  | ABCDE   <-- fat mid
  |  BC D
  |   D
```

Implementation options:

1. **Text grid**: monospace columns per period (simple, Cryexc-like readability).
2. **Blocks**: colored squares per TPO count (faster LOD).

MVP: text grid with `ImGui::BeginTable` or custom draw with mono font.

POC row: bold / accent background.  
VA band: dim background behind letters.  
IB high/low: horizontal lines.

---

## 6. Wiring

| Component | Role |
|-----------|------|
| `TpoModel` | cold research |
| `DeskPanel::tpo` | structure page |
| Shared session clock | `SessionCalendar` util used by VP + TPO |
| Optional | link VP + TPO side by side on structure page |

Do not couple TPO to strategy bars only — use public tape / mid so empty backtests without tape still can use mid samples from L2.

---

## 7. Tests

```
TEST(Tpo, LetterIncrementsEachPeriod)
TEST(Tpo, PrintTouchesBin)
TEST(Tpo, RangeModeFillsBetweenHighLow)
TEST(Tpo, PocIsMaxTpoCount)
TEST(Tpo, ValueAreaSeventyPercentOfCounts)
TEST(Tpo, SinglePrintDetection)
TEST(Tpo, IbRangeFromFirstNPeriods)
TEST(SessionCalendar, UtcDayBounds)
```

---

## 8. Acceptance checklist

- [ ] Letters advance on period boundary.
- [ ] POC/VA match golden fixtures.
- [ ] IB lines correct for first N periods.
- [ ] Session boundary configurable; default documented.
- [ ] Desk panel usable alongside volume profile.
- [ ] Gates green.

---

## 9. Non-goals

- Full CBOT pit session templates.
- Anomaly scoring / “excess” auto-label beyond singles + IB.
- Multi-day composite profiles (phase 2).

---

## 10. Codex task prompt

```
Implement cyrex/market-structure/02-market-profile-tpo.md only.
TpoModel + SessionCalendar + ImGui TPO panel.
Unit-test letters, POC/VA, IB, single prints.
```
