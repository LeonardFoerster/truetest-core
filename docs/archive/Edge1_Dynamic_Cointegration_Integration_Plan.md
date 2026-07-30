# Step-by-Step Plan: Integration von Edge 1 "Dynamic Cointegration Pairs" in TrueTest Engine

**Erstellt:** 2026-06-20  
**Basis:** 
- Vollständige Extraktion der Anforderungen aus `Edge1_Dynamic_Cointegration_Pairs_TrueTest_Guideline-1.pdf` (12 Seiten, verbatim durch dedizierten Agent).
- Analyse der Engine-Architektur durch mehrere parallele Sub-Agents (Strategy Interface & Patterns, L2/Queue/Realism/MC, Risk/Portfolio/Multi-Leg, Data/Event Flow).
- Respektierung aller Constraints aus CLAUDE.md, prod.md, README (Live-Safety Freeze, Hot-Path Rules, MC Determinismus, zero-alloc, REGISTER_STRATEGY, etc.).

**Ziel:** Integriere die in der Guideline beschriebene Architektur & Strategie (Kalman dynamischer Beta, OU + Half-Life, Z-Score Signals, L2-Queue Filter via TrueTest's QueueAware/L2SnapshotQueueModel, MC-Validation mit correlated Paths) sauber, wartbar und testbar in die TrueTest HFT Engine.

**Wichtige Erkenntnisse aus den Agents (Zusammenfassung):**
- PDF fordert explizit: Kalman State-Space (Predict/Update, Q/R), Spread = P1 - beta*P2, Z-Score Crossover Entry/Exit, L2-Queue-Filter vor Entry (size_ahead / Level), MC mit correlated GBM + Cointegration-Noise + Realism (Impact/Queue/Fill/Latency), RiskOverlay (4% daily DD etc.), per-leg Sizing via compute_quantity + ExitManager per-opener.
- Engine hat exzellente Basis: IStrategy + on_market/on_l2_update/on_fill + take_pending_exit_intents, REGISTER_STRATEGY + Registry, multi-symbol DataHandler/Portfolio (per-symbol net + per-opener lots), L2SnapshotQueueModel + QueueAwareBookAdapter (genau wie im PDF referenziert), MC Controller + GBMGenerator + reset_for_next_trial + strategy->reset(seed), RiskManager + ExitManager + Funding Events + QuestDB Audit.
- Gaps (präzise): 
  - Kein nativer Kalman/Z/OU.
  - Kein correlated multi-asset Generator (McGeneratorConfig ist single-symbol).
  - L2-Queue für Decision-Time Filter muss in Strategy selbst implementiert werden (kein direkter Adapter-Zugriff aus IStrategy; nur on_l2_update + self-hosted L2SnapshotQueueModel-Logik).
  - MC wired aktuell keine queue models + keine L2 in Trials.
  - Keine pair-spezifische Config (pair=LEG1,LEG2).
  - Funding-Bias und joint-spread-risk sind Strategy-Overlay (Engine unterstützt per-leg perfekt).
- Empfohlener Ansatz (konsistent mit PDF "Option B sauberer" + Agent-Empfehlung): **Neue dedizierte Strategy** `dynamic-cointegration` (oder `kalman-pairs`) via REGISTER. Wiederverwendung von Mustern aus mean_reversion + hedge_demo + adaptive_hybrid. Keine Berührung der gefrorenen Safety-Surface (engine.cpp Kern, risk/*, tt_target etc. nur wo unvermeidbar + CCB).

**Phasen-Übersicht (8 Phasen, sequentiell mit parallelen Sub-Tasks wo sinnvoll):**

## Phase 1: Vorbereitung, Primitives & Utilities (Core Math)

**Ziel:** Implementiere die mathematischen Bausteine als saubere, testable, zero-alloc friendly Header (POD wo möglich). In <50 Zeilen pro Kalman wie im PDF empfohlen.

**Schritte (detailliert):**

1. **Neues Verzeichnis / Dateien anlegen:**
   - `src/analytics/kalman_filter.h` (und .cpp falls nötig für Tests)
   - `src/analytics/zscore_calculator.h`
   - `src/analytics/ou_estimator.h` (optional, für Half-Life Schätzung aus historischem Spread)
   - Aktualisiere `src/analytics/analytics.h` oder eigenen Header nur bei Bedarf.

2. **KalmanFilter (State-Space exakt nach PDF 2.2):**
   - Struct `KalmanState { double alpha=0, beta=1; double P[2][2] = {{1,0},{0,1}}; ... };` (POD, fixed-size arrays, keine std::vector/Eigen für Hotpath).
   - Methoden:
     ```cpp
     void predict(double process_noise_q = 1e-5);  // F=Identity, Q=diag(q)
     void update(double price_leg1, double price_leg2, double obs_noise_r = 1e-3);
     double get_beta() const;
     // Optional: get_alpha(), get_cov()
     void reset();  // für MC
     ```
   - State-Transition & Kalman Gain exakt wie im PDF (mit numerischer Stabilität: small eps).
   - Default Q=1e-5, R=1e-3 (wie Tabelle 5.4).
   - Keine Allocation im Hotpath. Pre-warm in ctor.
   - Unit Tests: deterministische Predict/Update Sequenzen, Konvergenz bei stationärem Beta.

3. **ZScoreCalculator:**
   - Rolling Mean + Std (Welford oder deque + sum für Effizienz, aber deque ok da nicht ultra-hot).
   - `double update(double spread);` → current z.
   - Config: window (30-120 bars, oder via half-life).
   - Crossover Detection Helper: `bool crossed_below_entry(double prev_z, double curr_z, double threshold)`.
   - Reset + seed support.

4. **OU / Half-Life Estimator (für Pair-Selektion & Window):**
   - Einfacher OLS oder AR(1) Fit auf Spread-Diff um theta zu schätzen.
   - `double estimate_half_life(const std::vector<double>& spreads);` → ln(2)/theta.
   - Nutze in MC Reporter oder Pair-Pre-Filter.
   - Optional: rolling online Version.

5. **SpreadCalculator Helper:**
   - `double compute_spread(double p1, double beta, double p2) const { return p1 - beta * p2; }`

**Deliverables:**
- Kompilierbar, Tests in `tests/test_analytics.cpp` oder neu `test_kalman_zscore.cpp`.
- CLI-Beispiel später mit --param.
- Dokumentation der Formeln in Kommentaren + Verweis auf PDF Appendix.

**Validation:** `ctest -R kalman` oder manuell, deterministisch über Seeds.

**Zeit/Risiko:** Niedrig. Safe (keine frozen files).

## Phase 2: Neue Strategy "dynamic-cointegration" (Kern-Implementierung)

**Ziel:** Voll funktionsfähige IStrategy nach PDF §5.2 Pseudo-Code + vollem IStrategy Contract.

**Schritte:**

1. **Dateien:**
   - `src/strategy/dynamic_cointegration_pairs_strategy.h`
   - `src/strategy/dynamic_cointegration_pairs_strategy.cpp`
   - Registrierung via `REGISTER_STRATEGY("dynamic-cointegration", ...)` (genau wie hedge-demo, mean-reversion).

2. **Klasse Design:**
   ```cpp
   class dynamic_cointegration_pairs_strategy : public IStrategy {
   public:
       // ctor mit Defaults
       explicit dynamic_cointegration_pairs_strategy(...);

       // Core
       std::optional<order_event> on_market(const market_event&) override;
       std::optional<order_event> on_l2_update(const l2_update_event&) override;  // für Queue + optionale Filter
       void on_fill(const fill_event&, uint64_t opener) override;
       std::vector<exit_intent> take_pending_exit_intents() override;
       void reset(uint64_t seed = 0) override;

       // Param Surface (PDF Tabelle + mehr)
       std::vector<param_def> get_param_schema() const override;
       void set_param(const std::string& key, double val) override;
       std::vector<std::pair<std::string,double>> get_indicator_values(const std::string&) const override;
   private:
       // Config
       std::string leg1_ = "SOLUSDT", leg2_ = "AVAXUSDT";
       double kalman_q_ = 1e-5, kalman_r_ = 1e-3;
       double z_entry_ = 2.0, z_exit_ = 0.7;
       double risk_fraction_ = 0.01;  // per leg or total
       double l2_queue_max_bps_ = 3000; // z.B. < 30%
       // ...

       // State
       double price1_ = 0, price2_ = 0;
       KalmanFilter kalman_;
       ZScoreCalculator zcalc_;
       // Per-symbol L2 state for queue filter
       std::unordered_map<std::string, L2QueueState> l2_states_;  // oder embedded L2SnapshotQueueModel-ähnlich

       // Position / lot tracking (wie hedge_demo: pro leg opener counters)
       std::unordered_map<std::string, int> open_legs_;  // or use two specific
       std::vector<exit_intent> pending_intents_;

       // Helpers: update_prices, compute_beta_spread_z, is_queue_favorable, is_regime_ok, compute_leg_qty_with_beta, submit_hedged_entry, flatten_legs
   };
   ```

3. **on_market Logik (getreu PDF Pseudo + Engine Patterns):**
   - Wenn symbol == leg1 oder leg2 → Preise updaten.
   - kalman_.predict();
   - if (leg1) kalman_.update(p1, p2);
   - beta = kalman_.get_beta();
   - spread = p1 - beta * p2;
   - z = zcalc_.update(spread);
   - if (l2_enabled && !is_favorable_queue_for_entry( z < -z_entry )) return;
   - Optional: if (!is_mean_reverting_regime( ema_regime_detector.get(leg1 or combined) )) return;
   - Signal Logic:
     - if (position_flat && z < -z_entry && prev_z > -z_entry) → long spread: BUY leg1, SELL leg2 (beta-adjusted qty)
     - if (in_position && z > -z_exit ) → flatten both legs via ExitManager intents
   - Sizing: 
     - qty1 = equity * risk_fraction / p1 (oder portfolio.compute + fixed risk)
     - notional = qty1 * p1
     - qty2 = (notional * beta) / p2
   - Erzeuge zwei order_events (return eines, pending state für zweites? Oder engine erlaubt nur eines pro call → interner Pending Buffer oder return erste, track und emitiere zweite beim nächsten Tick wenn nötig. Besser: nutze zwei Strategie-Instanzen oder sequentielle in einem Aufruf mit State, aber current dispatch erlaubt nur optional single pro call. Lösung: pending_orders_ interne Queue + return nacheinander oder ein "meta" batch simulieren durch State Machine. Für Einfachheit: zwei Intents + return erste Order, zweite wird beim nächsten on_market oder on_tick emittiert. Hedge-demo Pattern adaptieren.)
   - Registriere **zwei** exit_intents (make_long... für leg1, make_short... für leg2) via pending_intents_ + take_...

4. **L2 Queue Filter Implementierung (PDF 2.5 + 5.1, exakt referenzierte Komponenten):**
   - Strategy hält eigene `std::map<double, double> bids1_, asks1_;` (oder reuse struct aus L2SnapshotQueueModel).
   - In on_l2_update(ev): feed updates in per-leg map (if ev.symbol == legX).
   - `bool is_favorable_entry(bool is_long_spread) { ... compute size_ahead for intended limit price (use best or current mid for market? PDF bevorzugt passive). queue_frac = size_ahead / level_size; return queue_frac < 0.3 || bps < threshold; }`
   - Für Market Orders: Filter kann weniger streng sein oder vor Market-Entscheidung prüfen.
   - Bootstrap: first L2 snapshot via on_l2 oder accept 0 initially (degrade gracefully).
   - Exponiere in get_indicator_values: "queue_bps_leg1", "queue_favorable".

5. **Exits, on_fill, reset:**
   - Exakt wie mean_reversion (create_exit_intents) + hedge_demo (per-leg opener tracking).
   - on_fill: update leg counters per symbol + opener_side map.
   - reset(seed): clear alle maps, kalman_.reset(), zcalc_.reset(), pending.clear().

6. **Indikatoren & Param:**
   - Schema: pair (special parsing? double nur → nutze "leg1_sym" "leg2_sym" als strings? Warte: param_def ist nur double! Lösung: harte Defaults + set via ctor/JSON oder erweitere? Aktuell CLI --param nur double. Für pair: entweder zwei --symbol oder custom in main oder "pair_encoded" hack. Besser: unterstütze in Strategy "leg1" und "leg2" als separate params (werden als double geparst? nein). 
     - Real: viele Strategies nutzen strings intern. Für CLI: erlaube --param leg1_code=... aber da double: entweder neue CLI support für string params oder harte Kodierung in Test + JSON Config (adaptive tut das) oder symbol aus Data kommen lassen und param nur numeric.
     - Praktisch: Strategy hat Defaults "SOLUSDT","AVAXUSDT"; user setzt via Code oder füge string-param surface hinzu (kleine Erweiterung von param surface, da aktuell nur double). Oder CLI-spezifisch pair handling vor Strategy-Erzeugung.
     - Empfehlung: Füge in dieser Phase eine kleine Erweiterung in main.inc für bekannte string params oder dokumentiere "set in code / future". Für MC/Backtest nutze --param kalman_q=... und harte pair oder erweitere McRunConfig.
   - get_indicator_values: "beta", "spread", "zscore", "half_life_est", "queue_bps" etc.

**Deliverables:**
- Kompilierbar + in Registry.
- Funktioniert mit multi-symbol CSV (symbol col oder separate loads).
- Verwendet ExitManager voll (keine manuellen Closes).
- Nutzt vorhandene ema_regime für optionalen Filter.

**Validation:** test mit sample dual-symbol data; manuelle MC Vorbereitung.

## Phase 3: MC & Synthetic Erweiterung für Correlated Paths + Realism (PDF 5.3)

**Ziel:** Ermögliche " --monte-carlo --mc-trials 2000 --param pair=... " mit realistischen cointegrated Paths + L2-Queue Stats.

**Schritte (kritisch für Validierung):**

1. **Erweiterung der MC Types:**
   - `src/simulation/monte_carlo_types.h`: Erweitere McGeneratorConfig um:
     ```cpp
     std::vector<std::string> symbols = {"SOLUSDT", "AVAXUSDT"};
     double correlation = 0.85;
     double coint_theta = 0.05; // für OU Spread Noise
     double coint_sigma = 0.001;
     bool emit_synthetic_l2 = true;  // per symbol
     ```
   - SyntheticPath: Unterstützung für multi-symbol (vector<...> oder map, backward: wenn size==1 legacy).

2. **Neuer / erweiterter Generator:**
   - `src/simulation/generators/correlated_gbm_generator.h/cpp`
   - Implementiere joint Paths: Cholesky für correlated Brownian + separater OU Process auf Spread der beta-adjusted Preise (um Cointegration zu injizieren).
   - Erzeuge L2 Snapshots/Updates pro Symbol (stilisiert aber mit Depth für queue tests).
   - Interface IMonteCarloGenerator beibehalten (default_config anpassen).

3. **MC Controller Updates:**
   - `monte_carlo_controller.cpp`: 
     - Support für neue Generator ("coint-gbm" oder erweitere "gbm").
     - load_multi_symbol_path_into_handler (bars + optional L2 events).
     - In run_single_trial: setze queue_position_model + maker_queue_model im ecfg (wie main.inc für shadow).
     - Nach Trial: extrahiere Half-Life Verteilung, queue adverse, max DD etc. in TrialResult erweitern.
   - MonteCarloReporter erweitern für stat-arb Metrics (PDF: "Half-Life Verteilung, Queue Stats (adverse selection)").

4. **Wiring in main.inc / CLI:**
   - Neue --mc-model coint-gbm
   - --param pair=... oder --param symbols=SOL,AVAX + korrelations etc.
   - Aktiviere --depth-stream Äquivalent für synthetic.

5. **Realism in MC Trials:**
   - Setze impact (SquareRoot), latency, fill model, queue (L2Snapshot) in MC engine_config.
   - Das erlaubt echte Filter-Tests + Adverse Selection Messung.

**Deliverables:**
- 1000+ Trial fähig auf SOL-AVAX mit positiver Erwartung unter realistischen Conditions.
- JSON/Report enhält neue Felder.

**Validation:** Vergleich Static-OLS vs Kalman Beta in MC; Queue Filter Impact messen (mit/ohne).

## Phase 4: CLI, Registry, Factory, Multi-Symbol UX

- main.inc: apply_strategy_params erweitern für pair-spezifische (auch string handling minimal).
- Update StrategyFactory::available() und legacy Pfade (optional).
- Help Texts, examples in instructions.md.
- Beispiel CLI aus PDF übernehmen + anpassen:
  `./engine_backtest --provider synthetic --monte-carlo --mc-trials 2000 --mc-model coint-gbm --strategy dynamic-cointegration --param pair=SOLUSDT,AVAXUSDT --param kalman_q=1e-5 --param z_entry=2.0 --risk max_daily_loss=0.04 --queue-model l2-snapshot --maker-queue-model uniform --depth-stream --persist --output json`

## Phase 5: Risk Overlay, Funding Bias, Prop Compliance (PDF 5.5)

- In Strategy: Funding Bias Gate (z.B. track cumulative funding per leg via on_fill? oder da Funding im Engine verarbeitet wird, Strategy kann separate state führen oder auf equity curve basieren. Für exakt: kleine non-hot Erweiterung oder nutze get from analytics snapshot wenn exposed).
- Strategy: Per-leg + joint gross exposure checks bevor Order.
- Nutze bestehende engine limits (--max-daily-loss etc.) + strategy conservative risk_fraction (0.5-1.5% per leg).
- ExitManager: ATR-basiert + trailing wie in mean_reversion (wiederverwenden oder helper extrahieren).
- QuestDB: bereits gut (strategy_name, opener, queue in fills wo verfügbar).
- Keine Änderungen an frozen risk/ engine core wenn möglich.

## Phase 6: Polish, Indicators, Analytics Integration

- Erweitere get_indicator_values + dashboard Snapshot um beta, z, spread, queue, regime.
- AdverseSelectionTracker + neue Queue-Analytics in Reports.
- Optional: neuer BarAggregator oder Spread Series.

## Phase 7: Testing, Validation & MC Campaigns (PDF 7. "Nächste Schritte")

1. Unit: Kalman, ZScore, Strategy State Machine (entry/exit, no double entry).
2. Integration: test_engine + dual-symbol fixtures (tests/fixtures/ erweitern).
3. MC Campaign: 2000 Trials, walk-forward, OOS, sensitivity (Q/R, z thresholds, queue %).
4. Shadow: real Bybit depth20 + trades für SOL/AVAX → predicted vs actual queue/fills.
5. Golden: wenn Reports stabil.
6. Metrics: Sharpe, Half-Life Dist, Winrate, MaxDD, Funding P&L separiert, Queue adverse.

**Spezifische Gates:**
- Ohne L2-Queue Filter: höheres adverse.
- Mit Kalman (vs static beta): bessere Reversion Capture.
- Alle Tests pass + clean shadow run.

## Phase 8: Docs, Reporting, Rollout & Erweiterungen

- Neue Doc: `docs/edge1_dynamic_cointegration.md` (basierend auf PDF + impl Details).
- Update instructions.md (MC section, strategy list, flags für queue/MC).
- Update README, feature.md oder summary.
- Reporter Erweiterung.
- Nächste (Basket, Multi-TF, Funding Overlay) als separate Issues.

**Gesamte Erweiterungen (Priorisiert):**

- **Muss (Phase 1-3):** Kalman, Z, Strategy Core + L2 Filter, MC Correlated + L2 in Trials.
- **Sollte:** Regime Filter, ATR Exits, Funding Bias, erweiterte Reporter.
- **Kann:** Basket (Johansen), Multi-TF, Bybit-spezifischer Provider.

**Risiken & Mitigations (aus PDF + Engine):**
- Regime Shift → Kalman + Regime Filter + MC Regime-Switching + Shadow.
- Overfit → Walk-Forward + OOS + 2000 Trials + Uncertainty.
- Execution → L2 Filter + Impact + cons. sizing + Maker wo möglich.
- Funding Carry → Short Half-Life Paare priorisieren + Bias Filter + Funding in Report.
- Safety: Keine Änderungen an frozen files ohne Token + CCB + Shadow.

**Invariants (unbedingt einhalten):**
- Strategy hot path: no alloc, no nlohmann (ctor ok wie adaptive).
- MC: deterministisch per seed, reset() korrekt.
- Engine dispatch single-writer.
- Per-opener lots für korrekte Attribution (hedge-demo Pattern).
- Live: nur engine_live (compile gate).

**Nächste konkrete Aktionen nach Plan Approval:**
1. Phase 1 implementieren + Tests.
2. Phase 2 Strategy + lokaler Backtest mit realen CSV Paar-Daten.
3. Phase 3 MC.
4. Shadow PoC.

Dieser Plan ist ausführlich genug für vollständige Umsetzung durch Grok Build / Agent Loops (implement + review). Jede Phase hat klare Abnahmekriterien.

**Quellen:** Agent Reports (PDF verbatim, strategy lines, queue_position_model.h:64 etc.), engine source, guideline PDF §5 + §2 + §6.

— Ende des Plans —
