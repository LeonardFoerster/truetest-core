# Strategie-Entwicklung — IStrategy-Integrationsleitfaden

**Audience:** C++-Entwickler, die eine neue `IStrategy` gegen die TrueTest-Engine implementieren (kein Quant-Edge-Research).

**Canonical Headers:** `src/strategy/strategy_interface.h`, `src/exits/*`, Engine-Route/Finalize in `src/engine/engine.cpp`.

---

> **Zweck**
>
> Technischer SDK-Contract: eine neue Strategy kompilieren, registrieren und Entry/Exit korrekt an die Engine anbinden.
>
> **Out of Scope**
>
> - Quant-Edge-Research, Signal-Hypothesen und Parameter-Optimierung als Research-Workflow
> - Venue-Provider, Kill-Switch, DMS, Reconciler, Live-Capital-Ritual → siehe Governance/Safety-Docs und [AGENTS.md](../../AGENTS.md)
> - Retired Adaptive-Hybrid rebuild contract → [06-adaptive-hybrid-strategy.md](06-adaptive-hybrid-strategy.md)
> - Web-UI, Report-JSON-Schema und QuestDB-ILP-Details
> - StrategyFactory-Wartung und dual Factory/Registry-Konvergenz
> - strategy-validation / dual-portfolio A-B Analytics Roadmap
> - `IBracketAdapter`-Venue-Details jenseits des Defense-in-Depth-Hinweises
> - `position_sizing` / `compute_risk_quantity` Deep-Dive
> - Layer-Graph-Vollaudit und `engine.cpp`-Dekompositionsplan
> - Benchmarks/Perf-Claims ohne Messung
> - Plugin-`.so`-Marketplace oder dynamisches Strategy-Loading
> - Archive Edge1-Guides als aktuelle Source of Truth

Operator-Flags: [04-flags.md](04-flags.md). Build/CLI-Überblick: [01-instructions.md](01-instructions.md). Hot-Path-Regeln: [AGENTS.md §4](../../AGENTS.md).

---

## 1. Zweck, Zielgruppe und Abgrenzung

Dieser Guide ist der **technische Integrationsvertrag** zwischen Strategy-Code und Engine. Ziel ist, dass eine neue `IStrategy`:

1. kompiliert und in `cmake/Sources.cmake` verlinkt ist,
2. sich per `REGISTER_STRATEGY` in der `StrategyRegistry` meldet,
3. maximal ein `order_event` pro Callback emittiert,
4. Exit-Pläne als `exit_intent` deklariert (oder die Platform-Default-Exits nutzt),
5. unter Hot-Path-Constraints und MC-`reset` korrekt läuft.

**Nicht-Ziel:** Alpha-Forschung, Live-Ritual, Venue-Safety, Web-UI.

| Rolle | Zuständigkeit |
|-------|----------------|
| Strategy | Signal → optional `order_event`; optional `exit_intent`-Queue |
| Engine | `order_id`, `strategy_name`, Routing, Risk, Fill-Attribution, Exit-Arming |
| `apply_default_exit_policy` / `default_exit_params` | Merge Platform-SL/TP mit Strategy-Intents (Free Functions + Struct, **keine** Klasse `DefaultExitPolicy`) |
| `ExitManager` | pending → armed on fill; SL/TP/Trail/Time feuern |

**Cross-Links (nicht hier duplizieren):**

- Operator-Flags: [04-flags.md](04-flags.md) (`--strategy`, `--param`, `--sl`, `--tp`, `--exit-policy`)
- Adaptive-Hybrid is retired and **not** a template: [06-adaptive-hybrid-strategy.md](06-adaptive-hybrid-strategy.md)
- Hot Path R1–R10: [AGENTS.md §4](../../AGENTS.md)
- Master-Instructions: [01-instructions.md](01-instructions.md)

---

## 2. Architektur-Überblick: Strategy im Engine-Lifecycle

### Ownership-Grenze

Die Strategy **liefert** optional ein `order_event` und **deklariert** Exit-Pläne. Sie **führt Stops nicht selbst aus** — `ExitManager` + optional Venue-`IBracketAdapter` (Defense-in-Depth) erzwingen Brackets.

Unter `--exit-policy floor` (Default) brauchen Strategies **kein** eigenes SL/TP: Platform-Defaults greifen nach accepted Entry.

### Bar-Pfad

```
check_pending_stops  →  evaluate_exits  →  strategy_->on_market  →  route_order  →  finalize_strategy_route  →  dispatch_extras_on_market
```

(`check_pending_stops` kann im Bar-Pfad mehrfach laufen; die kanonische Reihenfolge vor dem Strategy-Callback ist: pending stops → exits → strategy → route.)

`finalize_strategy_route` drainiert `take_pending_exit_intents()`, wendet `apply_default_exit_policy` an, stempelt `opener_order_id` + `strategy_name`, registriert pending im `ExitManager` — **nur** bei accepted Submit (`order_id != 0`, nicht rejected, nicht halt).

**Wichtig — Intent-Drain nur mit Order:** Die Engine ruft `take_pending_exit_intents` ausschließlich aus `finalize_strategy_route` (und den Halt/Reject/oid0-Drain-Zweigen dort). Gibt der Callback **kein** `order_event` zurück, werden pending Intents **nicht** gepollt. Der Interface-Kommentar in `strategy_interface.h` („polls right after each on_market/…“) **überzeichnet** das: Intents ohne simultane Order bleiben in der Strategy liegen.

### Tick-Pfad

Analog: `check_pending_stops` → `evaluate_exits` → `on_tick` → `route_order` → `finalize_strategy_route` → extras.

### L2-Pfad

Bei **Pause/Halt** überspringt die Engine Strategy-Callbacks auf L2 komplett. Sonst: drain pending, `evaluate_exits`, `on_l2_update` (primary + additional) mit route/finalize.

### Multi-Strategy

- CLI: `--strategy a,b,c` — erstes ist primary, Rest `additional_strategies_`.
- Primary und Additional teilen **ein** Portfolio und **ein** Risk-Buch.
- Attribution über `strategy_name` auf dem Order/Fill (Engine stempelt vor Route via `set_strategy_name`).
- Dieselbe `--param`-Bag geht an alle Strategien (kein `strat:key`-Namespace).
- `StrategyRegistry::has(name)` / `available()` existieren. `--dry-run` validiert den **rohen** `--strategy`-String via `has(o.strategy)` **vor** dem CSV-Split — ob Multi-Strategy-CSV end-to-end die Early-Validierung passiert, bleibt eine offene Frage (siehe unten).

---

## 3. IStrategy-Vertragsfläche

Canonical: `src/strategy/strategy_interface.h`.

| Methode | Pflicht | Default | Semantik |
|---------|---------|---------|----------|
| `on_market(const market_event&)` | pure virtual | — | 0 oder 1 `order_event` pro Bar-Event |
| `on_tick(const tick_event&)` | optional | `nullopt` | nur bei Tick-Logik überschreiben |
| `on_l2_update(const l2_update_event&)` | optional | `nullopt` | nur bei L2-Logik überschreiben |
| `set_position_open(symbol, open)` | optional | no-op | Legacy Net-Flat-Push; **nicht** multi-lot-aware |
| `set_position_open(bool)` | optional | leitet an `""`-Symbol weiter | Convenience |
| `on_fill(fill, opener_order_id)` | optional | no-op | nach Portfolio-Buchung; Opener: `opener == fill.order_id` |
| `take_pending_exit_intents()` | optional | drain Singular → 0/1-Vector | **bevorzugte** Multi-Intent-API; Engine drainiert nur bei Order-Return |
| `take_pending_exit_intent()` | optional | `nullopt` | Legacy Singular |
| `get_param_schema()` | optional | `{}` | `param_def`-Liste für CLI/MC |
| `set_param(key, double)` | optional | wirft `Unknown parameter` | nur `double` |
| `get_indicator_values(symbol)` | optional | `{}` | Observability `(name, value)` |
| `reset(uint64_t seed=0)` | optional | no-op | MC object-reuse |

**Explizit nicht im Contract:**

- Multi-Order-Return pro Callback (nur `std::optional<order_event>`)
- Cancel-Request-API aus der Strategy

### Dual-API Exit-Intents

```cpp
// Default: Vector drainiert Legacy-Singular in 0/1-Vector
virtual std::vector<truetest::exits::exit_intent> take_pending_exit_intents() {
    std::vector<truetest::exits::exit_intent> out;
    if (auto one = take_pending_exit_intent())
        out.push_back(std::move(*one));
    return out;
}
```

- **Vector überschreiben** → Singular bleibt ungenutzt (Engine ruft Vector).
- **Nur Singular überschreiben** → Default-Vector drainiert ihn.
- **Emit-Timing:** Intents in derselben Callback-Runde wie der Entry-`order_event` pushen; ohne Order kein Engine-Drain.

### `param_def`

```cpp
struct param_def {
    std::string name;
    double default_value = 0.0;
    double min_value = -std::numeric_limits<double>::max();
    double max_value =  std::numeric_limits<double>::max();
    std::string description;
};
```

`min_value`/`max_value` sind **Schema-Metadaten**; ob `set_param` clampt oder wirft, variiert pro Strategy.

---

## 4. Entry: order_event bauen und emittieren

### Constructor und Identity-Setter

```cpp
order_event(
    std::chrono::system_clock::time_point timestamp,
    const std::string& symbol,
    order_type order_type,
    order_side side,
    double quantity,
    double price = 0.0,
    time_in_force tif = time_in_force::gtc,
    double stop_price = 0.0);
// market + GTC → TIF wird im Ctor zu IOC gezwungen
```

Identity-Felder sind **keine** Ctor-Argumente — sie werden über Setter gesetzt:

```cpp
order.set_order_id(id);                 // Engine (route_order)
order.set_strategy_name(name);          // Engine (primary/additional)
order.set_opener_order_id(opener);      // Strategy bei lot-aware Closern; Engine/ExitManager bei Bracket-Fires
order.set_earliest_eligible_ts(ts);     // Engine (Delay/Latency)
```

| Feld | Wer setzt | API |
|------|-----------|-----|
| `type` / `side` / `qty` / `price` / `stop_price` / `tif` | Strategy | Ctor |
| `opener_order_id` (Closer) | Strategy (lot-aware Closes) | `set_opener_order_id` |
| `order_id` | Engine (`route_order`) | `set_order_id` |
| `strategy_name` | Engine (primary/additional Name) | `set_strategy_name` |
| `earliest_eligible_ts` | Engine (Delay/Latency) | `set_earliest_eligible_ts` |

**Nicht auf resting Market verlassen:** `market` + Default-`gtc` wird zu `ioc`.

### SMA-Muster (kanonischer Entry)

```cpp
// Signal auf Close; market mit ref_px=close; optimistisches position_open_-Gate
if (!is_open && mkt.get_close() > *sma_value) {
    position_open_[mkt.get_symbol()] = true;  // optimistic gate
    return order_event(mkt.get_timestamp(), mkt.get_symbol(),
                       otype, order_side::buy, 100.0, ref_px);
}
```

`fill_style` (sma-Param) — Order-Typ und Engine-Delay sind **getrennt**:

| Wert | Order-Typ | Engine-Delay / Fill-Ort |
|------|-----------|-------------------------|
| `0` (Default) | `order_type::market` | Mit Default `execution_bar_delay=1` → Order bis zum nächsten preisführenden Event **desselben Symbols** geparkt (meist Bar-N+1). Fill ist **MM-Book-Walk nach Re-Center**, kein garantierter exakter Open. Mit `delay=0` → same-bar Close-Book. |
| `1` | `order_type::limit` at close (`limit_at_close`) | Limit am Close; Sweep-Semantik separat |

### Signal-Closes vs. Platform-Brackets

Orders, die Net **reduzieren** (`is_position_reducing`), bekommen **keine** invertierten Platform-Brackets unter `floor`/`union`. Unter `engine_only` werden Strategy-Intents auf Closern ebenfalls verworfen.

### Stop / stop_limit

Strategy-Orders vom Typ `stop` / `stop_limit` landen in `pending_stops_` (nicht in der Bar-Delay-Queue). Trigger über Bar High/Low; Fill-Anker am Stop oder Gap-Open. `check_pending_stops` läuft **vor** `evaluate_exits` und dem Strategy-Callback.

---

## 5. Exit: exit_intent, apply_default_exit_policy und ExitManager

> **Namensklarstellung:** Es gibt **keine** C++-Klasse `DefaultExitPolicy`. Platform-Merge läuft über Free Functions `apply_default_exit_policy` / `make_platform_exit_intent` und das Struct `default_exit_params` (`src/exits/default_exit_policy.h`). „DefaultExitPolicy“ ist nur ein konzeptionelles Label.

### `exit_intent`-Felder

| Feld | Bedeutung |
|------|-----------|
| `symbol` | Instrument |
| `close_side` | `sell` (Long-Close) / `buy` (Short-Close) |
| `qty` | Größe — beim Opener-Arm **immer** aus `fill_qty * qty_fraction` neu gesetzt (nicht „oft“) |
| `qty_fraction` | Default `1.0`; Scale-Out-Anteile. Mapping on arm/partial: `≤0 → 1.0`, `>1 → 1.0` (kein offenes Intervall-Clamp `(0,1]`) |
| `stop_loss` / `take_profit` | absolute Levels (optional) |
| `reference_entry` | gesetzt → Rebase um `(fill − reference)` on arm |
| `trailing_pct` | Trail-Anteil vom Best-Price |
| `deadline` | Time-Stop |
| `opener_order_id` | Engine stempelt nach accepted Route |
| `strategy_name` | Engine stempelt Routen-Namen |

### Helpers

```cpp
// reference_entry + pct-SL/TP mit korrektem Vorzeichen
auto ei = truetest::exits::make_long_exit_intent(
    symbol, entry, qty, sl_pct, tp_pct, /*strategy_name=*/{});
// Short: make_short_exit_intent(...) — SL oberhalb, TP unterhalb Entry
```

### Multi-Intent Scale-Out (Breakout)

Breakout emittiert z. B. zwei Intents: `qty_fraction = 0.45` (SL+TP) und `0.55` (SL+`trailing_pct`). Engine speichert Multi-Intents in `unordered_multimap` keyed by `opener_order_id`.

### Policy-Modi (`apply_default_exit_policy`)

| Modus | Verhalten |
|-------|-----------|
| `floor` (**Default**) | leere Intents → voller Platform-Plan; sonst fehlende SL/TP/Trail **global** prüfen (`intents_have_*` über **alle** Intents), dann nur `front()` patchen |
| `strategy_only` | nur Strategy-Intents (keine Platform-Injection) |
| `engine_only` | nur Platform; Strategy-Intents verworfen |
| `union` | Strategy-Intents unverändert behalten; höchstens einen Platform-Intent mit den global fehlenden, konfigurierten SL-/TP-/Trail-Legtypen **appenden**. Ist der Strategy-Plan leer, den vollständigen Platform-Intent verwenden. |

Params in `default_exit_params`: `sl_pct` (Default `0.003` = 0.3%), `tp_pct` (`0.01` = 1%), `trail_pct` (`0.0` — **kein** CLI-Flag, nur Config-Struct).

CLI → `engine_config.exit_defaults` (`apply_exit_defaults` in `main.inc`):

```text
--sl 0.003  --tp 0.01  --exit-policy floor
```

`parse_exit_policy_mode` akzeptiert zusätzlich Hyphen-Aliase:

| Token | Modus |
|-------|-------|
| `floor` | floor |
| `strategy_only` / `strategy-only` | strategy_only |
| `engine_only` / `engine-only` | engine_only |
| `union` | union |
| unbekannt | `nullopt` → stderr-Warning und **Force `floor`** |

Details Operator-seitig: [04-flags.md — Platform exits](04-flags.md).

### Engine-Handoff

```cpp
// Nur erreichbar, wenn Strategy ein order_event zurückgab und finalize_strategy_route lief
auto intents = strategy.take_pending_exit_intents();
intents = apply_default_exit_policy(config_.exit_defaults, order, net_qty,
                                    std::move(intents));
for (auto& intent : intents) {
    intent.opener_order_id = order_id;   // Engine-Stamp
    intent.strategy_name   = strategy_name;
    exit_manager_.register_pending(std::move(intent));
}
```

| Situation | Verhalten |
|-----------|-----------|
| Halt / Reject / `order_id == 0` | Intents drainen **ohne** Arming; ggf. `set_position_open`-Resync |
| `opener_order_id == 0` bei `register_pending` | **still drop** |
| accepted Submit | pending registriert; armed on opener-fill |
| Callback ohne Order | Intents **nicht** drainiert |

### ExitManager-Lifecycle

1. `register_pending` bei accepted Entry
2. `on_fill` (Opener, erster Fill): pending → armed; `qty` **immer** = `fill_qty * frac` mit `frac`: `≤0→1.0`, `>1→1.0`; Rebase wenn `reference_entry` gesetzt
3. `on_fill` (Opener, **subsequent partial**): pending schon promoted → armed `qty` **wächst** um `fill_qty * frac`, Entry auf **VWAP** über Partial-Fills; `opener_remaining_qty_` wächst mit
4. `on_fill` (Closer): Qty gegen `opener_close_in_flight_qty_` matchen. **Matchende** In-Flight-Closer (eigene Bracket-Fires) **bulk-canceln nicht** die restlichen Multi-Intent-Brackets — nur unaccounted/manual Closes oder Rest-qty ≤ 0 rufen `cancel(opener)`
5. Auswertung:
   - **`on_bar`:** **SL vor TP** bei gleichem Bar; Trail wird erst nach Survival (Bar ohne Fire) für den **nächsten** Bar angehoben; Fire-Px = Level oder Open on Gap; **Time-Stop (`deadline`)** nach SL/TP-Checks mit `fire_px = close` (kein Level/Gap-Anker)
   - **`on_price` (Tick):** MFE + Trail **vor** Trigger-Checks — Trail kann im **selben** Tick angehoben und gefeuert werden
6. Fire → `route_order(..., anchor_immediate=true)` — **kein** Bar-Delay; Close-Order bekommt `set_opener_order_id` / `set_strategy_name`

**Strategy soll Stops nicht selbst ausführen** — nur Intent deklarieren.

Venue-`IBracketAdapter` ist optional Defense-in-Depth; in-process Intent bleibt armed. Multi-Intent: Venue `place()` nur für den **ersten** Intent.

---

## 6. Indikatoren unter `src/indicator/`

Neun header-only Klassen. **Kein** `on_bar`/`on_tick` an Indikatoren — die Strategy ruft `update()` in `on_market`/`on_tick`.

| Klasse | Header | Input | Ready-Semantik | Warm-up-State | `reset()` |
|--------|--------|-------|----------------|---------------|-----------|
| `simple_moving_average` | `sma.h` | `price` | nach `period` Samples | `std::queue` | **nein** — Map/Objekt clearen |
| `exponential_moving_average` | `ema.h` | `price` | SMA-Seed über `period`, dann `k=2/(period+1)` | nur Skalare (`sum_`, `count_`, `last_value_`) | **nein** |
| `average_true_range` | `atr.h` | H,L,C | SMA-Seed, dann Wilder | `std::queue` (TR-Window) | ja |
| `bollinger_bands` | `bollinger.h` | `price` | nach `period`; population variance | `std::queue` | **nein** |
| `relative_strength_index` | `rsi.h` | `price` | effektiv `period+1` Preise (prev + changes); Wilder | nur Skalare | **nein** |
| `stochastic_oscillator` | `stochastic.h` | H,L,C | `k_period` + nested SMAs (`k_smoothing`, `d_period`) | `std::deque` | ja |
| `rolling_extreme` | `rolling_extreme.h` | `value` | Window == period; min/max Scan | `std::deque` | ja |
| `swing_detector` | `swing_detector.h` | H,L,C | ≥1 confirmed Pivot **und** `bar_count_ > 1 + 2*strength` | `std::deque` | ja |
| `ema_regime_detector` | `ema_regime.h` | EMA-Werte (+ optional Swing/ATR) | `ready()` = `has_values_ && !dist_pct_history_.empty()`; Observer, **keine** EMA-Ownership | `std::deque` (Dist-History) | ja |

### Ownership-Muster

| Muster | Beispiel | Hinweis |
|--------|----------|---------|
| Lazy `unordered_map<string, Indicator>` | `sma_strategy` | einfach; per-bar String-Hash |
| `SymbolStateStore<symbol_state>` | mean-reversion, structure-continuation, breakout | **bevorzugt** für Multi-Indikator |
| Observer ohne Ownership | `ema_regime_detector` | Strategy füttert EMA-Werte; Detektor besitzt keine EMAs |

```cpp
// Member-Update in on_market (SMA-Stil)
auto& sma = get_sma(mkt.get_symbol());
auto sma_value = sma.update(mkt.get_close());
if (!sma_value) return std::nullopt;
```

### SMA-Namenszuordnung

| Pfad | Was es ist |
|------|------------|
| `indicator/sma.h` | `simple_moving_average` |
| `sma_strategy` + `REGISTER_STRATEGY("sma")` | Production-IStrategy |

### Production vs. Tests

- Production-Strategien nutzen u. a. SMA/EMA/ATR/Stoch/Swing/Rolling/Regime.
- `bollinger_bands` und `relative_strength_index` erscheinen in Tests; keine Production-Strategy-Includes im Tree (Stand Code-Research).

### Allokation

- Warm-up: `std::queue` (SMA/Bollinger/ATR) bzw. `std::deque` (stochastic/rolling/swing/regime) wachsen bis Period/History, danach bounded; EMA/RSI halten nur Skalar-State.
- `swing_detector::snapshot` / `get_indicator_values` und Regime-Snapshots **allokieren** — nicht auf ultra-hot Diagnose-Pfad.

---

## 7. Parameter: Schema, set_param und CLI `--param`

### Contract

- Nur `double` über `set_param(key, value)`.
- Unbekannte Keys: Default wirft `std::runtime_error("Unknown parameter: …")`.
- `min`/`max` im Schema sind Metadaten; Clamp-Verhalten ist strategy-spezifisch.

### SMA-Schema-Beispiel

```cpp
std::vector<param_def> get_param_schema() const override {
    return {
        {"period", static_cast<double>(period_), 1, 10000, "SMA lookback period"},
        {"fill_style", static_cast<double>(fill_style_), 0, 1,
         "0=market (default); 1=limit_at_close"},
    };
}

void set_param(const std::string& key, double value) override {
    if (key == "period") { period_ = static_cast<std::size_t>(value); smas_.clear(); }
    else if (key == "fill_style") { /* 0|1 validate */ }
    else throw std::runtime_error("Unknown parameter: " + key);
}
```

### CLI-Verdrahtung

1. Optional strategy-spezifische Flags (`--sma-period`, Balance/Risk für mean-reversion/breakout).
2. `apply_execution_cost_params` — pusht `entry_fee_rate` / `exit_fee_rate`, **wenn** im Schema.
3. `apply_strategy_params` — jedes `--param key=value` (wiederholbar).

Ungültige Zeilen **ohne** `=` → Warnung auf stderr, **kein** Abort.

Multi-Strategy: dieselbe Param-Bag an primary **und** alle additional.

---

## 8. Registrierung und Build-Integration

### Registry (bevorzugter Pfad)

```cpp
REGISTER_STRATEGY("my-strategy", []() {
    return std::make_shared<my_strategy>();
})
```

- Static-Init in `StrategyRegistry` Singleton.
- `create(name)` wirft bei unknown — **kein** stiller Fallback.
- `has(name)` / `available()` für Validierung und Listing.
- CLI und Monte Carlo nutzen die Registry.

### Registrierte Namen (Stand Code)

| Registry-Name | Implementierung |
|---------------|-----------------|
| `mean-reversion` | `mean_reversion_strategy` |
| `sma` | `sma_strategy` |
| `ma-crossover` | `ma_crossover_strategy` |
| `breakout` | `breakout_strategy` |
| `coiled-spring` | thin Alias von `breakout_strategy` |
| `adaptive-hybrid` | **Nicht verfügbar**; ehemaliger Prototyp, Anforderungen in Doc 06 |
| `structure-continuation` | `structure_continuation_strategy` |
| `hedge-demo` | `hedge_demo_strategy` (Multi-Lot-Demo) |
| `larry_connor` | `larry_connor_strategy` |

CLI-Default bei leerem `--strategy`: **`mean-reversion`**.

CLI-Help-String ist **unvollständig** (`hedge-demo`, `larry_connor` fehlen dort) — **Registry ist autoritativ**. [04-flags.md](04-flags.md) listet den vollen Satz.

### Sources.cmake

Neue `.cpp` unter `ENGINE_CORE_SOURCES` eintragen (**no globs**). Ohne Link der TU läuft `REGISTER_STRATEGY` nie.

```cmake
# --- Strategies ---
src/strategy/my_strategy.cpp
```

### StrategyFactory (Legacy — nicht für neuen Code)

- Hard-coded Name-Map, unvollständig (`larry_connor`/`hedge-demo` fehlen).
- Unbekannte Namen → **stiller** `mean_reversion_strategy`-Fallback.
- Production-CLI: `StrategyRegistry::create` only.

---

## 9. Engine-Dispatch-Gotchas

### `execution_bar_delay` / `--exec-bar-delay`

| Fakt | Detail |
|------|--------|
| Semantik im Code | `N` zählt zukünftige preisführende Events **desselben Symbols**. Events anderer Symbole geben die Order nicht frei. |
| Typischer Effekt | Signal Bar-N Close mit `N=1` → Fill bei der nächsten Beobachtung desselben Symbols (meist Bar-N+1 Open-Region; Book re-centered). Am Stream-Ende ohne Folgebeobachtung verfällt die Order statt synthetisch zu fillen. Fill = MM Book-Walk, **kein** garantierter exakter Open. |
| `delay = 0` | same-bar Close-Book (sofort eligible) |
| `latency_model` | hat **Vorrang** vor `execution_bar_delay` |
| Live | CLI forciert `execution_bar_delay = 0` |

### Position-Gates

- Optimistic `position_open_` bei Emit (sma/ma-crossover).
- Reject / `oid==0`: Engine resync nur via `notify_position_change_all` → `set_position_open` — **nur** wenn Override vorhanden.
- Multi-Lot: `on_fill` + `opener_order_id`; `set_position_open` ungeeignet.
- Net-Flat Bulk-Cancel von ExitManager-Brackets nur wenn `openers_for <= 1`.
- Closer-Fills mit matching In-Flight-Qty (eigene Bracket-Closes) canceln **nicht** restliche Multi-Intent-Brackets.

### Fill-Attribution

- Reihenfolge: Portfolio → `dispatch_fill_to_strategy` → `ExitManager::on_fill`.
- Match `strategy_name` → primary oder additional; **no match = silent no-op**.

### Pause / Halt

| Zustand | Bar/Tick Strategy-Callback | Route | L2 Callbacks |
|---------|----------------------------|-------|--------------|
| Pause | läuft weiter | blockt (`oid=0`), Intents drained | **skipped** |
| Halt | nach Halt keine weiteren Calls/Routes | refused | skipped |

Hinweis: `engine.h`-Kommentar zu „Pause skippt on_market“ weicht vom Code ab — Bar/Tick laufen, nur Submit blockt.

### Exit-Arming-Timing

- Arming der Intents bei **accepted submit** (pending), nicht erst bei Fill.
- Level-Rebase nur mit gesetztem `reference_entry`.
- Absolute Structure-Stops: `reference_entry` unset lassen.
- Partial Opener-Fills nach Promotion wachsen armed qty (VWAP).

### Include-Pfad

```cpp
#include "exits/exit_intent.h"   // unter src/, nicht strategy/exits/
```

---

## 10. Hot-Path-Constraints für Strategy-Callbacks

`on_market` / `on_tick` / `on_l2_update` / `on_fill` laufen auf der **Engine-Event-Loop**. Vollständige Red Lines: [AGENTS.md §4 R1–R10](../../AGENTS.md).

| Erlaubt | Verboten |
|---------|----------|
| Prealloc Indicator-State, `SymbolStateStore` | `new`, unbounded `vector` grow, JSON |
| `return order_event{...}` by value | Heavy `fmt`→`string`, sync Logging |
| Intent in vorgehaltenem `pending_intents_` queue | Parallel-Subsysteme, eigene Pools/Rings |
| `update()` auf bestehenden Indikatoren | Unbounded Map-Wachstum pro Bar ohne Bound |

Engine besitzt Pools, `route_order`, `publish_event`. Strategy liefert nur optional `order_event` + Intent-Queue.

### Monte Carlo / `reset`

```cpp
// MC mit --mc-reuse-objects:
reusable_strategy_->reset(result.seed_used);
// sonst: frische StrategyRegistry::create + apply_strategy_params
```

| Strategy | `IStrategy::reset` überschrieben? |
|----------|-----------------------------------|
| mean-reversion, structure-continuation, larry_connor | ja |
| sma, ma-crossover, breakout, hedge-demo | **nein** → unsafe mit object reuse (frische Instanz pro Trial nötig) |

Ohne Override: Indicator/Position/RNG-State leckt zwischen Trials.

---

## 11. Minimales Worked Example und Multi-Lot-Muster

### Skeleton (sma-Stil, Platform-Floor für SL/TP)

```cpp
// my_strategy.h
#pragma once
#include "../core/event.h"
#include "../indicator/sma.h"
#include "strategy_interface.h"
#include <optional>
#include <string>
#include <unordered_map>

class my_strategy : public IStrategy {
public:
    explicit my_strategy(std::size_t period = 20) : period_(period) {}

    std::optional<order_event> on_market(const market_event& mkt) override {
        auto& sma = get_sma(mkt.get_symbol());
        auto v = sma.update(mkt.get_close());
        if (!v) return std::nullopt;

        bool open = position_open_[mkt.get_symbol()];
        if (!open && mkt.get_close() > *v) {
            position_open_[mkt.get_symbol()] = true;
            return order_event(mkt.get_timestamp(), mkt.get_symbol(),
                               order_type::market, order_side::buy,
                               100.0, mkt.get_close());
        }
        if (open && mkt.get_close() < *v) {
            position_open_[mkt.get_symbol()] = false;
            return order_event(mkt.get_timestamp(), mkt.get_symbol(),
                               order_type::market, order_side::sell,
                               100.0, mkt.get_close());
        }
        return std::nullopt;
    }

    void set_position_open(const std::string& symbol, bool open) override {
        position_open_[symbol] = open;  // Engine-Resync Reject/oid0
    }

    // Optional: eigene Brackets — sonst greift Platform floor nach accepted entry.
    // Intents nur zusammen mit dem Entry-order_event pushen (sonst kein Drain).
    // std::vector<truetest::exits::exit_intent> take_pending_exit_intents() override;

    std::vector<param_def> get_param_schema() const override {
        return {{"period", static_cast<double>(period_), 1, 10000, "lookback"}};
    }
    void set_param(const std::string& key, double value) override {
        if (key == "period") { period_ = static_cast<std::size_t>(value); smas_.clear(); }
        else throw std::runtime_error("Unknown parameter: " + key);
    }

    void reset(uint64_t /*seed*/ = 0) override {
        smas_.clear();
        position_open_.clear();
        // pending_intents_.clear();  // falls Intents genutzt
    }

private:
    std::size_t period_;
    std::unordered_map<std::string, simple_moving_average> smas_;
    std::unordered_map<std::string, bool> position_open_;
    simple_moving_average& get_sma(const std::string& symbol) {
        auto it = smas_.find(symbol);
        if (it == smas_.end()) {
            smas_.emplace(symbol, simple_moving_average(period_));
            return smas_.at(symbol);
        }
        return it->second;
    }
};
```

```cpp
// my_strategy.cpp
#include "my_strategy.h"
#include "strategy_registry.h"

REGISTER_STRATEGY("my-strategy", []() {
    return std::make_shared<my_strategy>();
})
```

Optional Intent-Emission bei Entry:

```cpp
pending_intents_.push_back(
    truetest::exits::make_long_exit_intent(sym, price, qty, 0.003, 0.01));
// take_pending_exit_intents: return std::move(pending_intents_); clear
// Engine drainiert nur, wenn dieser Callback auch ein order_event returnt.
```

### Multi-Lot: `hedge-demo`

Referenzmuster in `src/strategy/hedge_demo_strategy.*`:

- ignoriert `set_position_open`
- trackt Legs über `on_fill` (`opener_order_id`, Side-Map)
- emittiert `make_long_exit_intent` / `make_short_exit_intent` in `pending_intents_`
- `take_pending_exit_intents` moved den Vector

### Nicht als First Template

| Name | Grund |
|------|-------|
| `adaptive-hybrid` | Retired/unavailable; Doc 06 ist nur noch Rebuild-Spezifikation |
| `coiled-spring` | nur Registry-Alias von breakout |

---

## 12. Backtest ausführen und Autoren-Checkliste

### Build

```bash
# Preset
cmake --preset linux-tests
cmake --build --preset linux-tests

# oder ad-hoc
cmake -B build -DBUILD_TESTS=ON && cmake --build build -j1 --target engine_backtest
```

### Beispiel-Lauf

```bash
./build/engine_backtest \
  --provider synthetic \
  --strategy my-strategy \
  --param period=20 \
  --sl 0.003 --tp 0.01 --exit-policy floor \
  --seed 424242 \
  --no-pin --status-format off --no-tui \
  --output /tmp/run.json
```

Multi-Strategy:

```bash
--strategy sma,mean-reversion   # first = primary
# gleiche --param-Bag an alle
# Hinweis: dry-run has() prüft den rohen CSV-String vor Split
```

### Monte Carlo

- Mit `--mc-reuse-objects`: `reset(seed)` **muss** Indicator/Position/RNG clearen.
- Parallel: nur mit kompatiblem Thread-Preset (`inline`) — siehe [01-instructions.md](01-instructions.md) / AGENTS MC-Hinweise.

### Autoren-Checkliste (Definition of Done)

- [ ] Klasse erbt `IStrategy`; `on_market` implementiert (max. ein Order pro Call)
- [ ] `REGISTER_STRATEGY("name", factory)` in der `.cpp`
- [ ] TU in `cmake/Sources.cmake` unter Strategies eingetragen
- [ ] `get_param_schema` + `set_param` konsistent; unbekannte Keys throw
- [ ] Exit-Modell gewählt: Platform `floor` **oder** `exit_intent` (+ optional Scale-Out) via `apply_default_exit_policy`
- [ ] Exit-Intents **nur** zusammen mit Entry-`order_event` emittieren (sonst kein Drain)
- [ ] Gate: `set_position_open` (Single-Lot) **oder** `on_fill` (Multi-Lot) — kein stummes Desync
- [ ] `reset(seed)` wenn MC-Reuse vorgesehen; sonst frische Instanzen dokumentieren
- [ ] Hot Path: kein Heap/JSON/heavy string in Callbacks
- [ ] Focused Strategy-Tests grün
- [ ] Nach Edit unter `src/`: `./scripts/check-hotpath-json.sh`, `check-layer-deps.sh`, `check-live-safety-freeze.sh`

---

## Offene Fragen

1. Ob Multi-Strategy-CSV end-to-end immer die Early-`has(o.strategy)`-Validierung vor dem Split passiert, ist aus CLI allein nicht vollständig geklärt (`has` prüft den rohen `--strategy`-String).
2. `StrategyFactory` bleibt für TUI-Runtime-Switch-Pfade relevant — Konvergenzzeitplan unklar.
3. `param_def` min/max: kein einheitliches Clamp-Verhalten über alle Strategies.
4. `exit_reason`-Enum existiert in `exit_intent.h`, wird vom Fire-Pfad derzeit nicht gestempelt.
5. `default_exit_params.trail_pct` hat kein CLI-Flag.
6. Kommentar „risk layer catches qty_fraction sum > 1“ ist in `exits/` nicht als Enforcement implementiert.
7. The retired Adaptive Hybrid prototype is not an implementation reference;
   any rebuild must satisfy the reset and exit-intent contracts in Doc 06.

---

## Index-Updates (für Doc-Maintainer)

Bei Merge dieses Guides:

1. [docs/00-INDEX.md](../00-INDEX.md): Reference-Eintrag `07-strategy-development.md` + Audience-Pfad „Strategy-Autor“
2. [docs/README.md](../README.md): How-to-Navigate um „I am writing a strategy“ ergänzen
3. [01-instructions.md](01-instructions.md) §8: Pointer auf 07 statt Zwei-Satz-API
4. [04-flags.md](04-flags.md): Cross-Link von `--strategy`/`--param`/`--sl`/`--tp`/`--exit-policy` auf 07
5. [02-user-manual.md](02-user-manual.md): Data-Flow-Bullet auf diesen Author-Guide verlinken

---

*Code is truth. Stand abgeleitet aus `strategy_interface.h`, `exits/*`, `engine.cpp`, `main.inc`, `Sources.cmake` und den Strategy-TUs unter `src/strategy/`.*
