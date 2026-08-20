# Strategie-Entwicklung: Pflichtleitfaden für neue `IStrategy`

Stand (Code-of-truth): `2026-08-20`.

**Quellen der Wahrheit (alle genannten Pfade sind im Repository aktuell):**
- `src/strategy/strategy_interface.h`
- `src/strategy/strategy_registry.h`
- `src/strategy/strategy_factory.h` (Legacy/Kompatibilitäts-Schicht)
- `src/strategy/sma/sma_strategy.h`, `src/strategy/sma/sma_strategy.cpp`
- `src/strategy/breakout/breakout_strategy.h`, `src/strategy/breakout/breakout_strategy.cpp`
- `src/exits/default_exit_policy.h`, `src/exits/default_exit_policy.cpp`
- `src/exits/exit_intent.h`
- `src/engine/order_intent_processor.cpp`, `src/engine/engine_market.cpp`
- `src/engine/engine.h` / `src/bin/main.inc` (Strategie-Routen)
- `cmake/Sources.cmake`
- `tests/test_strategies.cpp`
- `AGENTS.md`, bestehende Strategy-Doku unter `docs/`

## Aktualitäts-Hinweise (ältere Verweise)

- `on_bar(...)`: **gilt nicht mehr** als Callback in der aktuellen Strategie-API.
- `reset_for_next_trial(...)`: gibt es nicht mehr als Strategy-Callback; verwenden stattdessen `IStrategy::reset(uint64_t seed = 0)`.
- Manuelle Registry-Einträge im Sinne von „`StrategyFactory` plus harte `if/else`-Pfad für produktive CLI“-Weg sind für neue Strategien nicht mehr der primäre Weg.
- `DefaultExitPolicy` ist **keine Klasse** im Code; aktuell sind es Funktionen (`apply_default_exit_policy`, `make_platform_exit_intent`) und ein Datenobjekt (`default_exit_params`) im Namespace `truetest::exits`.

---

## 1) Aufbau einer neuen `IStrategy`

Eine neue Strategie ist eine Klasse in `src/strategy/<kebab-name>/` mit folgendem Minimum:

- Erbt von `IStrategy`.
- Implementiert `on_market(const market_event&)`.
- Definiert eigene State-Strukturen (`position`, `indikatoren`, `offene Lots` etc.).
- Implementiert sinnvolle `get_param_schema` + `set_param`.
- Registriert sich über `REGISTER_STRATEGY` in der `.cpp`.
- Nutzt `take_pending_exit_intents` nur, wenn eigene Exit-Intents benötigt werden.

Beispiel-Skelett (nur Interface-Teile):

```cpp
class my_strategy : public IStrategy {
public:
    std::optional<order_event> on_market(const market_event& mkt) override;
    std::optional<order_event> on_tick(const tick_event&) override { return std::nullopt; }
    std::optional<order_event> on_l2_update(const l2_update_event&) override { return std::nullopt; }

    std::vector<param_def> get_param_schema() const override;
    void set_param(const std::string& key, double value) override;
    void on_fill(const fill_event& fill, std::uint64_t opener_order_id) override;
    std::vector<truetest::exits::exit_intent> take_pending_exit_intents() override;
    void reset(uint64_t seed = 0) override;
    bool supports_mc_trial_reuse() const override;
};
```

## 2) `on_market`, optional `on_tick` / `on_l2_update`

Die aktuelle Dispatch-Reihenfolge im Hot-Path ist:
- Stops/Exit-Checks (`check_pending_stops`, `evaluate_exits`)
- Strategy-Callback (`on_market` / `on_tick` / `on_l2_update`)
- Route (`route`) + `finalize_route` (`src/engine/engine_market.cpp`, `src/engine/order_intent_processor.cpp`)

`on_market` ist der primäre Einstieg (Bar-/Marktdatenpfad), `on_tick` und `on_l2_update` optional.

Wichtige Regeln:
- Rückgabe: `std::optional<order_event>` (ein Order pro Aufruf, oder `std::nullopt`).
- Keine Nebenwirkungen außerhalb deterministischer Strategie-Logik im Callback.
- Keine Heap-allozierenden Muster (heißer Pfad: keine neuen `new`/`std::string`-Generierung auf jedem Tick/Bar).

## 3) Erzeugung eines `order_event`

`order_event` wird im Callback per Rückgabe erzeugt und von der Engine geroutet.

```cpp
return order_event(mkt.get_timestamp(), mkt.get_symbol(),
                   order_type::market, order_side::buy,
                   quantity, mkt.get_close());
```

- Konstruktor-Parameter: `(timestamp, symbol, order_type, side, quantity, price = 0.0, tif = gtc, stop_price = 0.0)`.
- In `core/event.h` ist fest kodiert: `market + gtc` wird auf `ioc` gemappt.
- Die Engine setzt ergänzend Metadaten wie Order-ID und `strategy_name` im Routing.

## 4) Registrierung mit `REGISTER_STRATEGY`

Für neue Strategien gilt Registry-first:

```cpp
// my_strategy.cpp
#include "my_strategy.h"
#include "strategy_registry.h"

REGISTER_STRATEGY("my-strategy", []() {
    return std::make_shared<my_strategy>();
});
```

- Registrierung über statischen Initializer beim Linken der TU.
- Unbekannter Name wird bei `StrategyRegistry::create` als Fehler gemeldet.
- `strategy_factory.h` enthält noch Legacy-Zweiglogik; das ist bei neuen Strategien **keine** Zielarchitektur mehr.

## 5) `get_param_schema` und `set_param`

`param_def` nutzt aktuell:
- `name`, `default_value`, `min_value`, `max_value`, `description`.

Leitlinie:
- `get_param_schema()` als UI-/CLI-Quelle zurückgeben.
- `set_param(key, value)` unterstützt genau diese Keys und wirft bei unbekannten Keys (default-Implementierung in `strategy_interface.h` wirft `Unknown parameter`).
- Typ ist aktuell nur `double`.
- Werte-Parsing/Anwendung muss in den eigenen Strategiezustand übersetzt werden (Perioden per Cast auf Integer-Typ, Reset der Indikatoren bei Größenänderung etc.).

`sma_strategy` und `breakout_strategy` nutzen diese API als Minimal-/Exit-Intent-Referenz.

## 6) State, `on_fill`, Positionsverwaltung und `reset(seed)`

State-Verantwortung liegt bei der Strategie.

- `set_position_open(symbol, bool)` ist ein Legacy-Hook (Net-Flat-Signal) und nur begrenzt für Single-Lot-Modelle geeignet.
- Für sauberes Multi-Lot-Tracking ist `on_fill(fill, opener_order_id)` der stabile Pfad:
  - `opener_order_id == fill.order_id`: Eröffnungsfüllung (opener)
  - sonst: Closer/weitere Füllungen
- `set_account_equity(double)` wird für risikobasierte Positionsgrößen im Mark-to-Market-Loop gesetzt.
- `reset(seed)` wird bei `--mc-reuse-objects` genutzt und MUSS allen internen Zustand deterministisch zurücksetzen.
- `supports_mc_trial_reuse()` auf `true` nur, wenn `reset(seed)` vollständig ist.

Hinweis aus aktuellen Tests:
- `breakout_strategy` kann als Referenz für Exit-Intents + on_fill-basiertes Lot-Tracking dienen.
- `sma_strategy` zeigt den kompakten Single-Position-Pfad.

## 7) `exit_intent` und Zusammenspiel mit Default-Exit-Policy

Pflichtwissen für Strategy-Exits:

- Strategie kann bei Entry-Order Rückgabe eigene `exit_intent`s bereitstellen:
  - `take_pending_exit_intent()` (legacy Single) oder
  - `take_pending_exit_intents()` (empfohlen, vor allem für Scale-Outs).
- `order_intent_processor.cpp` ruft `register_strategy_exit_intent` **nur**, wenn die zuletzt geroutete Order akzeptiert wurde (Order-ID vorhanden, kein Reject/Halt).
- Auf dieser Basis wendet der Engine-Handler `apply_default_exit_policy` an.

Modes in `default_exit_params::mode`:
- `floor` (Default): Plattform-Defaults ergänzen, ohne Strategy-Pläne zu ersetzen.
- `strategy_only`: nur Strategie-Intents.
- `engine_only`: nur Plattform-Defaults (Strategie-Intents werden verworfen).
- `union_mode`: Strategy-Intents bleiben, fehlende Teile können ergänzt werden.

Spezialfall:
- Für position-reduzierende Orders werden Strategy-Intents unter `engine_only` nicht als schützende Plattform-Shorts/Longs armd.

Breakout ist das praktische Beispiel für mehr als ein Exit-Intent (TP + Trailing-Aufteilung).

## 8) `cmake/Sources.cmake`

Neue Strategy-Strategiedateien müssen als explizite `.cpp`-Einträge eingefügt werden; keine Globs.

Im Bereich `ENGINE_CORE_SOURCES` unter `// --- Strategies ---`:

```cmake
# --- Strategies ---
src/strategy/<kebab-name>/my_strategy.h
src/strategy/<kebab-name>/my_strategy.cpp
```

Ohne TU im Core-Build wird die Registry-Makro-Registrierung nicht linkbar.

## 9) Tests für neue Strategie

Erforderliche Prüfpunkte vor Merge:

- **Unit** (Signale/Parameter/State): bestehend ergänzen in `tests/test_strategies.cpp`.
- **Integration** (Callback-Route, Attributionspfad, Fill/Exit-Interaktion): mind. ergänzen in `tests/test_engine_integration.cpp` oder `tests/test_engine_lookahead.cpp`.
- **Exit-Konsistenz**: für Bracket-/Intent-Pfade mindestens mit `tests/test_exit_manager.cpp`/`tests/test_default_exit_policy*.cpp` abgleichen.
- **Hot-Path-Verifikation**: `ctest`-Filter für Hot-Path-/Order-Pfade (`ObjectPool`/`Ring`/`hotpath`-Namen je vorhandener Testnamen).

## 10) Build-, Backtest- und Verifikationsbefehle

- Build-Tree neu und konsistent:
```bash
cmake --preset linux-tests
cmake --build --preset linux-tests --target engine_backtest truetest_tests
ctest --preset linux-tests
```

- Minimaler Headless-Backtest:
```bash
./out/build/linux-tests/engine_backtest \
  --provider synthetic \
  --strategy my-strategy \
  --param period=20 \
  --sl 0.003 --tp 0.01 --exit-policy floor \
  --seed 424242 --no-pin --status-format off --no-tui \
  --output /tmp/run.json
```

- Multi-Strategy (falls relevant): `--strategy a,b,c` (gleiche Param-Map für alle Strategie-Instanzen).

- Nach Strategy- oder Engine-Route-Änderung:
```bash
./scripts/check-hotpath-json.sh
./scripts/check-layer-deps.sh
./scripts/check-live-safety-freeze.sh
```

- Für Wiederverwendung in MC: `--mc-reuse-objects` nur mit korrekt implementiertem `reset(seed)` / `supports_mc_trial_reuse()`.

## 11) Final DoD für neue Strategie

- [ ] Dateistruktur + Klassensignatur implementiert (`src/strategy/<kebab-name>/`).
- [ ] `on_market` implementiert; `on_tick`/`on_l2_update` nur bei Bedarf.
- [ ] `order_event`-Emission eindeutig (0/1 per Callback-Aufruf).
- [ ] `REGISTER_STRATEGY("...")` in `.cpp` gesetzt.
- [ ] `get_param_schema`/`set_param` konsistent und validierbar.
- [ ] Entweder Platform-Exit-Policy genutzt oder eigene `exit_intent`-Hooks vollständig.
- [ ] State-Management: `on_fill` oder dokumentierter `set_position_open`-Fallback (ohne stillen Desync).
- [ ] `reset(seed)` und `supports_mc_trial_reuse()` sauber entschieden.
- [ ] cmake-Eintrag unter `cmake/Sources.cmake` vorhanden.
- [ ] Unit- und Integrations-Tests ergänzt; relevante Hot-Path/Exit-Tests geprüft.
- [ ] Dokumentationsverweise in `docs/README.md`, `docs/00-INDEX.md` aktualisiert.

## Verbleibende technische Unklarheiten (Stand heute)

- `--strategy`-Validierung im Dry-Run prüft den rohen String einmalig (`--strategy` als Ganzes), die CSV-Auflösung für Additional-Strategien findet erst im Laufzeitpfad statt.
- Einige Dokumente verwenden noch Beispielpfade zu `StrategyFactory`; diese sind dokumentiert, aber nicht mehr der empfohlene Implementierungsweg.

---

## 12) Referenzstrategie: `ema-rsi-atr-pullback`

Die Strategie `ema-rsi-atr-pullback` (`src/strategy/ema_rsi_atr_pullback/`) dient als Referenzimplementierung für Trend-Pullback-Handel mit kombiniertem Exit-Modell:

- **Registry-Key:** `ema-rsi-atr-pullback`
- **Indikatoren:** EMA (150), RSI (14, Wilder), ATR (14, Wilder)
- **Trendfilter:** Long bei `close > EMA`, Short bei `close < EMA`, neutral bei `close == EMA`.
- **Trigger:** Long bei `previous_rsi <= 40` und `current_rsi > 40`; Short bei `previous_rsi >= 60` und `current_rsi < 60`.
- **Positionsgröße:** Fixed-Risk (Standard: `0.005` = 0,5 % des Equity pro Trade) über `truetest::risk::compute_risk_quantity` mit Berücksichtigung von Gebühren, Slippage und Notional-Cap. Kein Fixed-Quantity-Fallback.
- **Initialer Stop:** `2.0 * ATR` als `exit_intent` (`qty_fraction = 1.0`, `stop_loss`, `reference_entry = signal_close`, kein TP).
- **Trend-Exit:** Bei Trend-Invalidierung (`close < EMA` für Long, `close > EMA` für Short) emittiert die Strategie eine Market-Closer-Order mit gesetzter `opener_order_id`.
- **Single Active Trade:** Höchstens 1 Trade je Symbol gleichzeitig (kein Pyramiding).
- **Execution Bar Delay:** Standardbetrieb mit `execution_bar_delay = 1` (Signal auf Bar N, Fill frühestens auf Bar N+1).
- **Exit-Policy:** Zur Ausführung ausschließlich der Strategy-eigenen Stops `--exit-policy strategy_only` verwenden (unter `floor` würde die globale Platform-Policy sonst fehlende Take-Profits ergänzen).
- **Portfolio-Heat:** Das Trade-Risiko ist auf 0,5 % des Equity normiert. Eine globale Grenze für das Gesamtexposure (z.B. 2 % Portfolio-Heat) wird über die Engine-/Risk-Konfiguration gesteuert.
- **Monte Carlo:** Vollständiger State-Reset in `reset(seed)` implementiert (`supports_mc_trial_reuse() == true`).
- **Hinweis:** Die Implementierung stellt keine Aussage über Profitabilität dar.

### Beispiel-CLI-Aufruf

```bash
./out/build/linux-tests/engine_backtest \
  --provider synthetic \
  --strategy ema-rsi-atr-pullback \
  --param ema_period=150 \
  --param rsi_period=14 \
  --param atr_period=14 \
  --param long_rsi_threshold=40 \
  --param short_rsi_threshold=60 \
  --param atr_stop_multiplier=2 \
  --param risk_fraction=0.005 \
  --exec-bar-delay 1 \
  --exit-policy strategy_only \
  --seed 424242 --no-pin --status-format off --no-tui \
  --output /tmp/run.json
```
