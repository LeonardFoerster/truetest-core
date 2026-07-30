# Edge 1: Dynamic Cointegration Pairs – Umfassendes Integrations- und Implementierungsdokument für TrueTest

**Dokumenttyp:** Vollständige Spezifikation + Step-by-Step Implementierungsplan (alle Phasen mit Details)  
**Datum:** 2026-06-20  
**Version:** 1.0 (Comprehensive)  
**Quellen:**  
- `Edge1_Dynamic_Cointegration_Pairs_TrueTest_Guideline-1.pdf` (12 Seiten, Quantitative Research 2026) – vollständige Extraktion durch spezialisierten Agent  
- Analyse-Ergebnisse von 4 parallel gespawnten Sub-Agents (PDF-Requirements, Strategy Architecture, L2/Queue/MC Realism, Risk/Portfolio/Multi-Leg)  
- Codebase-Exploration (src/strategy/*, src/execution/queue_*, src/simulation/*, src/engine/*, src/risk/*, main.inc, etc.)  
- Governance: CLAUDE.md, prod.md, README.md, instructions.md  

**Zweck:** Dieses Dokument ist das **einzig autoritative, umfassende Arbeitsdokument** für die Integration der in der Guideline beschriebenen Architektur und Strategie in die TrueTest HFT Engine. Es dient Grok Build, Implementern, Reviewern und Operatoren als vollständige Referenz.

---

## 1. Executive Summary & Edge-Überblick (verbatims aus PDF)

**Edge-Name:** Edge 1 – Dynamic Cointegration Pairs Trading (L1/L2-Sektor, z.B. SOL-AVAX / SUI-NEAR)

**Kern-Techniken (PDF Seite 1 & 3):**
> "Mit Kalman-Filter für dynamischen Beta (Hedge Ratio) + Z-Score Signal Generation + L2-Queue Filter für realistische Execution"

> "Der Ansatz kombiniert klassische Cointegration mit modernen adaptiven Techniken (Kalman Filter für zeitvariierenden Hedge Ratio / Beta), standardisierter Signalgenerierung (Z-Score) und microstructure-aware Execution-Filtern (L2-Queue Position aus TrueTest's QueueAwareBookAdapter)."

**Warum dieser Edge (verbatims):**
> "In hochvolatilen, aber strukturell korrelierten Märkten wie Crypto Perps bestehen temporäre Abweichungen vom langfristigen Gleichgewicht (Cointegration). Durch dynamische Anpassung des Betas (nicht statisch wie bei OLS) und Filterung mit Orderbook-Queue-Informationen kann ein robuster, positiver Erwartungswert generiert werden – bei gleichzeitig kontrolliertem Risiko (niedriger Drawdown, market-neutral)."

> "Studien (z.B. Tadi 2021 auf arXiv) zeigen, dass dynamische Cointegration-Strategien in Crypto signifikant outperformen gegenüber Buy-and-Hold und statischen Ansätzen."

**Zielgruppe & Scope (PDF):**
- Quantitative Researcher / Trader mit TrueTest HFT-Engine
- Fokus: Bybit USDT Perps (via HyroTrader oder direkt)
- Edge-Typ: Statistical Arbitrage (Mean-Reversion)
- Horizont: Intraday bis mehrtägig (abhängig von Half-Life)

**Spezifische Paare (SOL-AVAX / SUI-NEAR):** "Goldilocks"-Bereich – ausreichend Liquidität für $10k–$100k Orders, aber nicht so effizient wie BTC/ETH.

---

## 2. Vollständige Mathematische & Statistische Anforderungen (PDF Abschnitt 2)

### 2.1 Cointegration Theorie
Zwei (oder mehr) I(1)-Zeitreihen sind cointegriert, wenn eine lineare Kombination I(0) ist.

**Engle-Granger (bivariat):**
1. OLS: `P1_t = α + β · P2_t + ε_t`
2. ADF-Test auf Residuen ε_t (H0: Unit Root → Ablehnung p < 0.05 = Cointegration)
3. Spread `S_t = P1_t - β · P2_t`

**Johansen:** Für multivariate/Baskets (VECM Rang-Test).

**Relevanz Crypto:** Regime-abhängig → dynamische Ansätze besser als statische Rolling-OLS.

### 2.2 Kalman Filter für dynamischen Beta (State-Space)
**State:** `x_k = [α_k, β_k]^T`

**Prediction:**
`x_{k|k-1} = F · x_{k-1|k-1} + w_k` (F ≈ Identity)

**Observation:**
`z_k = H · x_k + v_k` (z = P1, H = [1, P2])

**Update:**
`K_k = P_{k|k-1} H^T (H P_{k|k-1} H^T + R)^{-1}`
`x_k|k = x_{k|k-1} + K_k (z_k - H x_{k|k-1})`

Vorteil: adaptiv, lernt kontinuierlich, besser bei Strukturbrüchen.

### 2.3 Ornstein-Uhlenbeck & Half-Life
`dS_t = θ (μ - S_t) dt + σ dW_t`

**Half-Life:** `t_{1/2} = ln(2) / θ`

Zentral für Window-Wahl (`N ≈ 2/θ - 1`) und Hold-Duration.

### 2.4 Z-Score Signal Generation
`Z_t = (S_t - rolling_mean(S, N)) / rolling_std(S, N)`

**Einstieg (Long Spread):** `Z_{t-1} < -Z_entry ∧ Z_{t-2} > -Z_entry` (Crossover)
**Ausstieg:** `Z_{t-1} > -Z_exit`

Klassisch: Z_entry=2.0, Z_exit=0.5–1.0

### 2.5 L2-Queue Filter (TrueTest-spezifisch)
> "TrueTest's QueueAwareBookAdapter und L2SnapshotQueueModel tracken die Queue-Position (size_ahead, front/uniform/back heuristics). Ein L2-Queue Filter erlaubt Entry nur, wenn die geschätzte Queue-Position akzeptabel ist (z.B. nicht > X % der Level-Size)."

---

## 3. Guideline Umsetzung in TrueTest – Vollständige Anforderungen (PDF Abschnitt 5)

### 5.1 Voraussetzungen & Architektur (verbatim)
- Provider: Erweitere oder nutze binance_futures als Blueprint (depth20@100ms + trades + user-data)
- L2 Data → `L2SnapshotQueueModel + QueueAwareBookAdapter`
- Indicators: Vorhandene (ema_regime, atr, bollinger, swing) + custom (Spread/Z-Score/Kalman)
- MC & Realism: Synthetic correlated GBM + Cointegration-Noise + Impact/Queue/Fill/Latency Models
- Risk: HyroTrader-ähnlich (4% daily DD, 6% max loss)

### 5.2 Strategy Implementation (High-Level + Pseudo-Code – verbatim)
> "Option A (empfohlen): Erweitere mean_reversion_strategy oder adaptive_hybrid_strategy mit custom Logic für Spread/Kalman.  
Option B: Neue Strategy via REGISTER_STRATEGY Macro (sauberer für komplexe Logik)."

**Pseudo-Code (C++ / on_market):**
```cpp
void on_market(const market_event& ev) {
    if (ev.symbol == leg1 || ev.symbol == leg2) update_prices_and_beta(ev);

    kalman_.predict();
    if (ev.symbol == leg1) kalman_.update(ev.price, leg2_price);
    double beta = kalman_.get_beta();

    double spread = leg1_price - beta * leg2_price;
    double z = zscore_calculator_.update(spread);

    if (!l2_queue_filter_.is_favorable_entry(z < -threshold)) return;
    if (!is_mean_reverting_regime()) return;   // optional via ema_regime

    if (z < -entry_z && position_size_ == 0) {
        double qty = risk_manager_.compute_quantity(risk_fraction);
        submit_hedged_orders(qty, beta);      // long leg1, short leg2
    } else if (z > exit_z && position_size_ > 0) {
        flatten_via_exit_manager();
    }
}
```

Kalman: Einfache 1D/2D Klasse <50 Zeilen (POD State + Arrays).

### 5.3 MC-Validation & Realism Models
- Generator: Correlated GBM + Cointegration-Noise
- Realism: Square-root Impact, L2-snapshot Queue, Probabilistic Fills, Latency, Fee Tiers
- Risk Overlay: 4% daily DD etc.
- Reporter: Sharpe/Sortino/PF/MaxDD/Winrate + **Half-Life Verteilung + Queue Stats**

Beispiel CLI (angepasst):
```bash
./engine_backtest --provider synthetic --monte-carlo --mc-trials 2000 --mc-model gbm \
  --strategy dynamic-cointegration --param pair=SOLUSDT,AVAXUSDT \
  --param kalman_q=1e-5 --param kalman_r=1e-3 --param z_entry=2.0 \
  --risk max_daily_loss=0.04 --queue-model l2-snapshot --maker-queue-model uniform \
  --output json --persist
```

### 5.4 Parameter (Startwerte)
| Parameter              | Beispielwert     | Begründung                     |
|------------------------|------------------|--------------------------------|
| kalman_process_noise (Q) | 1e-5 .. 1e-4    | Langsames Beta-Wandern        |
| kalman_obs_noise (R)   | 1e-3 .. 5e-3    | Preis-Noise Level             |
| z_entry / z_exit       | 2.0 / 0.5–1.0   | Klassisch, via MC optimieren  |
| lookback_z / half_life | 30–120 bars     | Anpassen an OU Half-Life      |
| l2_queue_threshold     | size_ahead <30% | Back-of-Queue vermeiden       |
| risk_fraction          | 0.5–1.5% per Leg| DD-Kontrolle                  |

### 5.5 Risk & Prop-Compliance
- RiskManager + engine risk_limits
- ExitManager (Per-Lot SL/TP ATR-basiert + Trailing, reduceOnly)
- Halt/KillSwitch bei DD oder abnormaler Queue
- QuestDB Logging (Orders, Fills, Queue Stats, Funding)
- Sizing immer über `compute_quantity` + current_equity + risk_fraction

---

## 4. Risiken, Limitations & Mitigations (PDF Abschnitt 6 – verbatim)

| Risiko                    | Beschreibung                              | Mitigation (TrueTest)                          |
|---------------------------|-------------------------------------------|------------------------------------------------|
| Regime Shift / Breakdown  | Beziehung löst sich auf                   | Kalman + ema_regime Filter; MC Regime-Switching; Shadow Testing |
| Overfitting               | Parameter zu stark auf Historie optimiert | Walk-Forward MC, OOS, Uncertainty Mapper      |
| Execution / Slippage / Queue | Adverse Selection trotz Filter         | L2-Queue Filter + Impact Model + cons. Sizing + Maker-Only |
| Funding & Carry           | Lange Holds → Funding frisst Edge         | Funding-Event Handling + Bias-Filter; kurze Half-Life Paare priorisieren |
| Prop Rule Breach          | Zu aggressiv, Concentration               | RiskManager + conservative Params + Diversifikation + detailliertes QuestDB Logging |

---

## 5. Nächste Schritte aus PDF (Abschnitt 7)
1. Deep Backtest & MC (1000+ Trials, real CSV + synthetic correlated)
2. Shadow Testing (real Bybit WS depth+trades)
3. HyroTrader PoC + QuestDB/TUI Monitoring
4. Erweiterung (Basket, Funding-Bias, Multi-TF)

---

## 6. Aktueller TrueTest Engine Status & Gap-Analyse (Ergebnisse der Sub-Agents)

### 6.1 Strategy System (stark)
- `IStrategy` (strategy_interface.h): `on_market`, `on_l2_update`, `on_fill`, `take_pending_exit_intents`, `get_param_schema`/`set_param`, `get_indicator_values`, `reset(seed)`.
- Registry: `REGISTER_STRATEGY` Macro (empfohlen).
- Beispiele: `mean_reversion_strategy` (feste-risk Sizing, ATR/Fib Exits, pending_intents), `hedge_demo_strategy` (per-opener lot tracking), `adaptive_hybrid_strategy` (schwerer on_l2_update + L2Snapshot).
- Multi-Symbol: Engine dispatched alle Events an Strategy (interleaved). Strategy muss per-symbol Maps halten.
- Gaps für Pairs: Kein nativer Pair-State, kein cross-symbol atomic snapshot. Lösung: Strategy-intern.

### 6.2 L2 / Queue Infrastructure (exakt wie PDF referenziert)
- `L2SnapshotQueueModel` + `IQueuePositionModel` (`queue_position_model.h`)
- `QueueAwareBookAdapter` (`queue_aware_book_adapter.h`) – trackt `size_ahead`, `avg_queue_position_bps()`
- Flow: Provider (depth20) → engine `apply_l2_*` → Adapter + publish `l2_update_event` → Strategy `on_l2_update`
- **Kritischer Gap:** Strategy hat **keinen direkten** Zugriff auf Adapter. Muss eigenen L2-State (ähnlich `L2SnapshotQueueModel`) aus `on_l2_update` Events füttern.
- MC: `emit_synthetic_l2` existiert, wird aber aktuell nicht in Trials geladen oder mit Queue-Modellen verdrahtet.

### 6.3 Monte Carlo & Synthetic
- `GBMGenerator`, `McGeneratorConfig` (single symbol), `SyntheticPath`, `MonteCarloController`, `reset_for_next_trial`.
- Stärken: Deterministisches Seeding, Strategy `reset(seed)`, Engine Reset.
- Gaps: Keine native Correlated Multi-Asset + Cointegration-Noise Generierung. Keine Queue-Modelle in MC-Trials. L2 wird nicht angewendet.

### 6.4 Risk / Portfolio / Exits / Funding (gut für Pairs geeignet)
- Portfolio: Per-symbol netted `positions_` + unabhängige `lots_` (key = opener_order_id). Cross-symbol nativ unterstützt.
- ExitManager: Per-opener, symbol-aware. `make_long_exit_intent` / `make_short...`. Per-lot SL/TP/Trailing.
- Funding: `funding_event` (per Symbol, cash_delta). Engine → Portfolio + QuestDB + Analytics. Kein automatischer Bias-Filter.
- RiskManager: Globale + per-symbol Limits. Strategy muss joint-spread / beta-adjusted Exposure selbst prüfen.
- Starke Basis für hedged Legs (long leg1 + short leg2 als zwei opener).

**Gesamt-Fazit der Analyse:** TrueTest ist **ideal positioniert** (genau wie PDF sagt). Die meisten Komponenten existieren bereits. Die Arbeit liegt fast ausschließlich in:
- Neuen Stateless/POD Utilities (Kalman, Z-Score)
- Einer dedizierten Strategy
- Erweiterung des MC Generators + Controller für correlated Paths + L2
- Kleinen CLI/Reporting Erweiterungen

**Empfohlene Architektur:** **Neue Strategy** `"dynamic-cointegration"` (Option B). Keine Pollution existierender Strategien.

---

## 7. Vollständiger Phasenplan mit maximalen Details

### Phase 0 – Vorbereitung (bereits teilweise erledigt)
- Dieses Dokument lesen + PDF + CLAUDE.md + key Source Files studieren.
- Branch erstellen: `feature/edge1-dynamic-cointegration`
- Checkliste pro Phase anlegen (z.B. in `todo.md` oder `reports/`).

### Phase 1: Mathematische Primitives (Kalman + Z-Score + OU)

**Ziel:** Wiederverwendbare, testbare, hot-path-sichere Komponenten (POD, fixed-size, zero-alloc wo möglich).

**Dateien:**
- `src/analytics/kalman_filter.h` (Header-only bevorzugt)
- `src/analytics/zscore_calculator.h`
- `src/analytics/ou_half_life.h` (optional)
- `tests/test_kalman_zscore.cpp` (neu oder in test_analytics)

**Detaillierte Implementierungsanforderungen:**

1. **KalmanFilter**
   - Verwende struct mit raw arrays:
     ```cpp
     struct KalmanFilter {
         double state[2] = {0.0, 1.0};      // alpha, beta
         double cov[2][2] = {{1e-3,0},{0,1e-3}};
         void predict(double q = 1e-5);
         void update(double p1, double p2, double r = 1e-3);
         double beta() const { return state[1]; }
         void reset();
     };
     ```
   - Implementiere Matrix-Operationen manuell (2x2) oder mit einfachen Schleifen.
   - Numerische Stabilität: kleine Regularisierung.
   - Exakt der Formulierung aus PDF Anhang folgen.

2. **ZScoreCalculator**
   - Welford-Online oder deque-basiert (letzteres für Einfachheit).
   - `update(double value) -> double z;`
   - `bool entry_crossover(double prev, double curr, double thresh) const;`

3. **Tests**
   - Deterministische Sequenzen (fixed seeds).
   - Stationärer Beta → cov → 0.
   - Z-Score um 0 pendelt bei mean-reverting Spread.

**Akzeptanzkriterien:** Alle Unit-Tests grün. Kommt ohne Allocation im Update-Pfad aus.

### Phase 2: Core Strategy Implementation

**Dateien zu erstellen:**
- `src/strategy/dynamic_cointegration_pairs_strategy.h`
- `src/strategy/dynamic_cointegration_pairs_strategy.cpp`

**Wichtige Design-Entscheidungen:**
- Registrierung: `REGISTER_STRATEGY("dynamic-cointegration", ...)`
- State: Zwei explizite Legs oder `unordered_map<std::string, LegState>`
- L2-State: Eigener `struct L2Book { std::map<double,double> bids, asks; };` pro Leg (oder kopiere Logik von `L2SnapshotQueueModel`)
- Queue-Filter: `bool is_favorable_queue(const std::string& sym, double intended_price, order_side side) const;`
- Hedged Orders: Strategie tracked "in_position" (z.B. long_leg1 + short_leg2 Count). Beim Signal zwei Orders erzeugen. Da pro Call nur ein `order_event` zurückgegeben werden kann: 
  - Interne Pending-Order Queue + return erste; zweite beim nächsten Event.
  - Oder zwei separate exit_intents + eine Order pro Tick (praktikabel).
- Exits: Immer über `pending_intents_` + `take_pending_exit_intents()` (zwei Intents pro Entry).
- Sizing: 
  ```cpp
  double qty1 = equity_ * risk_fraction_ / price1;
  double qty2 = (qty1 * price1 * beta) / price2;
  ```
- Regime Filter: Optional `ema_regime` Instanz(en) für die Legs.
- Param Schema: `pair` (als zwei separate leg1/leg2 oder spezielle Behandlung), kalman_q, kalman_r, z_entry, z_exit, l2_queue_bps_threshold, risk_fraction, lookback_z, use_regime_filter etc.

**Wichtige Methoden-Signaturen müssen exakt dem IStrategy Contract entsprechen.**

**Integration mit Engine:**
- Engine ruft `on_l2_update` (wichtig für Queue).
- `on_fill` muss opener_order_id korrekt tracken (wie hedge_demo).
- `reset(uint64_t seed)` muss alles zurücksetzen (auch Kalman + Z-Score + L2 Books).

**Deliverable:** Strategie ist mit bestehenden Backtests (local CSV multi-symbol) lauffähig.

### Phase 3: MC & Synthetic Erweiterung (wichtigste Validierungs-Phase)

**Dateien:**
- `src/simulation/monte_carlo_types.h` (erweitern)
- `src/simulation/generators/correlated_cointegration_generator.h` + `.cpp` (neu)
- Updates in `monte_carlo_controller.cpp`, `monte_carlo_reporter.cpp`

**Anforderungen:**
- Erweiterte `McGeneratorConfig` mit `symbols`, `correlation`, `coint_theta`, `coint_sigma`, `base_spread_bps` etc.
- Generator erzeugt zwei (oder mehr) korrelierte GBM-Pfade + injiziert OU-Noise auf dem Spread (um künstliche Cointegration zu erzeugen).
- Generiere pro Symbol L2-Snapshots/Updates.
- Controller muss:
  - Queue-Modelle (`L2SnapshotQueueModel`) und `maker_queue_model` in den Trial-Engine-Config setzen.
  - L2-Daten in den DataHandler laden (oder via provider event path laufen lassen).
  - Erweiterte Metriken sammeln (Half-Life pro Trial, Queue-Position bei Entry, adverse selection).
- Reporter: Neue Tabellen/JSON-Felder für stat-arb Metriken.

**CLI-Beispiel (Zielzustand):**
Siehe oben in Abschnitt 5.3.

**Akzeptanz:** 2000 Trials laufen ohne Crash, Reports enthalten Half-Life Verteilung und Queue-Stats.

### Phase 4: CLI, Konfiguration, Factory & Multi-Symbol UX

- Erweiterung von `main.inc` (param handling für pair/leg1/leg2, MC model selection).
- Update `StrategyFactory` und Hilfe-Texte.
- Unterstützung für `--param leg1=SOLUSDT --param leg2=AVAXUSDT` oder spezielles Paar-Parsing.
- Sicherstellen, dass `--depth-stream`, `--queue-model l2-snapshot`, `--maker-queue-model uniform|front|back` zusammen mit der Strategie funktionieren.

### Phase 5: Risk, Funding Bias, Exit- & Prop-Overlay

- In der Strategy:
  - Funding Bias: Sammle Funding-Deltas (kann über Portfolio-State oder separate Logik erfolgen) oder implementiere "nur traden wenn Funding günstig für Net-Position".
  - Konservative Sizing + Exposure Checks vor Order-Emission.
- Nutze bestehende `ExitManager` Features (ATR, Fib, scale-out, trailing) wo sinnvoll (Wiederverwendung aus mean_reversion).
- Keine direkten Änderungen an gefrorenen Risk-Dateien ohne CCB.

### Phase 6: Analytics, Indicators, Reporting & Dashboard

- Reiche `get_indicator_values` (beta, z, spread, half_life, queue_bps_leg1, queue_bps_leg2, regime).
- Erweiterung von `AdverseSelectionTracker` oder neue Tracker für Queue-Stats.
- Update `MonteCarloReporter` und normale Reports.

### Phase 7: Umfassendes Testing & Validierung

**Unit:**
- test_kalman_zscore.cpp
- test_dynamic_cointegration_strategy.cpp (State-Machine, Entry/Exit, Queue-Filter, Reset)

**Integration:**
- Neue Fixture mit zwei Symbolen (oder erweiterte sample Daten).
- Test mit `engine_integration` oder dedicated Test.

**MC-Kampagnen (Pflicht):**
- Mindestens 1000–2000 Trials pro Paar (SOL-AVAX, SUI-NEAR).
- Abläufe:
  1. Baseline (statisches Beta)
  2. Kalman-only
  3. + Z-Score Variationen
  4. + L2-Queue Filter (mit/ohne)
  5. Walk-forward / Regime-Switching Paths
- Vergleichsmetriken: Sharpe, MaxDD, Winrate, Half-Life Verteilung, durchschnittliche Queue-Position bei Entry, Funding P&L.

**Shadow:**
- Real Bybit Futures depth20 + trade Daten für die Paare.
- Vergleich predicted Queue-Position vs. tatsächliche Fills.

**Akzeptanzkriterien (pro Phase + Gesamt):**
- Alle Tests grün.
- MC zeigt positiven Erwartungswert mit vernünftigem Risk.
- Keine Allocations im Hotpath (validiert über `test_hotpath_allocs` oder ähnlich).
- Sauberer `engine_shadow` Run.

### Phase 8: Dokumentation, Rollout & Erweiterungen

- Dieses Dokument pflegen.
- `docs/edge1_dynamic_cointegration.md` (Operator Guide).
- Updates:
  - `instructions.md` (neue Strategy, MC Flags, Queue + MC Kombination)
  - `README.md`, `feature.md`
- QuestDB Views / Dashboards für Stat-Arb Metriken.
- Spätere Erweiterungen: Basket (Johansen), Multi-Timeframe, Bybit-spezifische Provider-Verbesserungen.

---

## 8. Traceability Matrix (Anforderung → Umsetzung)

| PDF Anforderung                    | Umsetzungsort                          | Phase |
|------------------------------------|----------------------------------------|-------|
| Kalman State-Space                 | analytics/kalman_filter.h              | 1     |
| Z-Score + Crossover                | analytics/zscore_calculator.h + Strategy | 1+2 |
| L2-Queue Filter (size_ahead)       | Strategy + eigene L2Book + is_favorable | 2     |
| submit_hedged_orders (2 Legs)      | Strategy on_market + exit_intents      | 2     |
| Correlated GBM + Coint-Noise       | simulation/generators/...              | 3     |
| MC Realism + Queue in Trials       | monte_carlo_controller + types         | 3     |
| Half-Life Verteilung im Report     | monte_carlo_reporter                   | 3+6   |
| RiskManager + ExitManager Nutzung  | Strategy + bestehende Engine           | 2+5   |
| QuestDB Logging                    | Engine bereits vorhanden               | -     |
| Parameter-Tabelle Werte            | Strategy param_schema + Docs           | 2+4   |

---

## 9. Wichtige Invariants & Regeln (CLAUDE.md + Engine)

- **Hot Path:** Keine Allokationen, keine JSON, Pre-Reserve.
- **MC:** Seed-Determinismus zwingend.
- **Live Safety:** Keine Änderungen an `engine.cpp`, `risk/*`, `tt_target.h`, `threading/worker_watchdog.h` etc. ohne `LIVE_SAFETY_CCB_APPROVED` Token + CCB + Shadow-Run.
- **Strategy ist sicher** für die meisten Arbeiten.
- Per-opener Lot Attribution immer verwenden (kein reines Netting für Exits).
- Queue-Filter muss graceful degradieren (wenn keine L2 Daten: Entry erlauben oder loggen).

---

## 10. Anhang

### Wichtige Formeln (Zusammenfassung – PDF Anhang)
```
Spread:         S_t = P1_t − β_t · P2_t
Kalman Predict: x = F x
Kalman Update:  K = P H^T (H P H^T + R)^{-1}; x = x_pred + K (z − H x_pred)
OU Half-Life:   t½ = ln(2) / θ
Z-Score:        Z_t = (S_t − μ_rolling) / σ_rolling
Entry:          Z_{t-1} < −Z_entry ∧ Z_{t-2} > −Z_entry
```

### Empfohlene Strategie-Namen
- `dynamic-cointegration`
- Alternative: `kalman-pairs`

### Beispiel für zukünftige Erweiterung (Basket)
Johansen-Test + multiple Legs (zukünftige Phase).

---

**Ende des umfassenden Dokuments.**

Dieses Dokument enthält alle Phasen mit maximaler Detaillierung, Verbatims aus der PDF, Code-Referenzen, Gap-Analyse, Traceability, Invariants und konkrete Umsetzungsanweisungen.

Nächster Schritt für Grok Build: Mit Phase 1 beginnen und schrittweise implementieren, dabei dieses Dokument als Spec verwenden und nach jeder Phase die Akzeptanzkriterien abhaken. 

Quellen sind vollständig nachverfolgbar (PDF + Agent-Reports + Source).