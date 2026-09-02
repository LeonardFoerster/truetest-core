# Deterministische Runs und Replays

TrueTest kann lokale Backtests und Monte-Carlo-Kampagnen als versionierte,
maschinenprüfbare Runs ausführen. Ein solcher Run besitzt ein unveränderliches
`run_manifest.json`, deterministische Ergebnisartefakte und ein getrenntes
`run_receipt.json` mit Laufzeitbeobachtungen.

Diese Funktion beweist technische Reproduzierbarkeit. Sie beweist weder die
fachliche Güte einer Strategie noch die Markt-, Queue- oder Fill-Fidelity eines
Simulationsmodells.

## Determinismus-Envelope

Bitidentität wird für denselben folgenden Zustand getestet und zugesichert:

- identischer Git-Commit und bei einem Dirty-Build identischer Worktree-Diff,
- identisches Buildprofil, ausführbares Binary, C-/C++-Compiler, vollständig
  erfasster Satz geladener ELF-Laufzeitobjekte und Architektur,
- identische Dependency-Versionen und Floating-Point-Flags,
- identisches versioniertes Manifest und identischer expliziter Master Seed,
- identische Dataset-Bytes, Instrument- und Fee-Snapshots,
- identische effektive Strategie-, Simulator- und Modellkonfiguration.

Das Manifest weist diesen Umfang als
`same-source-build-toolchain-libc-stdlib-architecture` aus. Eine Bitidentität
über andere CPUs, Compiler, Standardbibliotheken oder `libm`-Implementierungen
ist nicht behauptet. Insbesondere verwenden normalverteilte Pfade weiterhin
`log`, `sqrt`, `sin` und `cos`; deren letzte Bits sind keine portable
C++-Sprachgarantie. Komprimierte Event-Logs sind zusätzlich an die im Build
erfasste zstd-Version gebunden.

## Expliziter Master Seed

Jeder Backtest und jede Monte-Carlo-Kampagne benötigt einen expliziten Seed:

```bash
engine_backtest --provider synthetic --strategy sma --seed 424242 \
  --no-pin --status-format off --no-tui
```

Ein fehlendes `--seed` ist ein Konfigurationsfehler vor Run-Beginn. Der Wert
`0` ist zulässig, aber nur, wenn er ausdrücklich über CLI, Run-Konfiguration
oder Manifest gesetzt wurde. Er bedeutet nicht mehr „zufällig“ und löst keinen
Fallback aus. Systemzeit, PID, Thread-ID, Adressen und `std::random_device`
werden in deterministischen Pfaden nicht als Entropiequelle verwendet.

Alte Konfigurationen müssen daher ein Feld `seed` erhalten beziehungsweise
`--seed` übergeben. Anwendungen der C-API müssen zusätzlich die vorhandene
Seed-Präsenzinformation setzen; ein bloß nullinitialisierter Wert ist keine
explizite Konfiguration.

Direkte C++-Komponentenharnesses, die `engine` ohne CLI/C-API/MC-Composition-
Root konstruieren, sind keine persistierbaren Run-Surface; für einen
maschinenprüfbaren Run muss die Konfiguration über einen der validierten
Startpfade mit gesetztem Presence-Bit erfolgen.

## Seed-Hierarchie

`DeterministicSeedDeriver` implementiert Ableitungsschema v1 als reine
64-Bit-Funktion. Die Spezifikation besteht aus unsigned Wraparound,
SplitMix64-Mixing, dem Namespace `TTR6SEED`, einer stabilen numerischen Domain
und einem stabilen Index. Die Reihenfolge von Konstruktion und Ausführung ist
kein Input.

Domänen existieren für:

- Run und Monte-Carlo-Trial,
- Strategie und Market Maker,
- Synthetic Price/GBM, Volume und L2,
- Queue-, Fill-, Latency- und Impact-Modell.

Ein Trial-Seed hängt ausschließlich von Master Seed und Trial-Index ab. Ein
Komponenten-Seed wird anschließend aus diesem Trial-Seed und der
Komponentendomäne abgeleitet. Kein global fortgeschriebener RNG verteilt Seeds
an Worker. Known-Answer-Tests sperren sowohl diese Ableitung als auch den
expliziten xoshiro256**-PRNG. Uniform-Samples verwenden definierte 53-Bit-Werte;
Normal-Samples verwenden einen repository-eigenen Box-Muller-Pfad. Ein Reset
setzt auch den gecachten zweiten Normalwert zurück.

## Manifest und Receipt

`run_manifest.json` besitzt `manifest_schema_version: 1` und trennt vier
Bereiche:

- `deterministic_inputs`: einzige Quelle des Run-Fingerprints,
- `dataset_locations`: auflösbare, aber nicht gehashte Dateipfade,
- `expected_hashes`: nach erfolgreichem Erstrun ergänzte Output-Hashes,
- `exact_lifecycle_replayable`: Persistenzvertrag des Runs.

Ein Manifest mit `exact_lifecycle_replayable: true` ist nur publizierbar, wenn
alle Run- und bei Monte Carlo alle Trial-Hashes vorhanden sind. Während eines
Runs bleibt das Manifest ausschließlich im Speicher; die finale Datei wird
erst nach Event-Log, Lifecycle, Result/Report und Hashbildung atomar
veröffentlicht. Ein Abbruch kann daher kein formal gültiges, aber
unvollständiges Replay-Manifest hinterlassen. Das eingecheckte Beispiel ist
absichtlich als nicht exakt replaybar markiert, weil es keine zugehörigen
Golden-Artefakte mitliefert.

Schema v1 begrenzt Kampagnen auf 100000 Trials und Manifestdateien auf
128 MiB. Das schema-spezifische Leselimit berücksichtigt die vollständige
Seed-Hierarchie und die optionalen Synthetic-L2-Hashes einer Kampagne; größere
oder unbeschränkte Eingaben werden vor dem JSON-Parse abgelehnt.

Der Fingerprint lautet:

```text
SHA256(canonical_json(deterministic_inputs))
```

Wandzeit, Dauer, Hostname, PID, Thread-ID, temporäre/absolute Ausgabepfade und
Completion-Reihenfolge sind nicht enthalten. `run_receipt.json` verwendet
Receipt-Schema 2 und enthält `executed_at_utc`, Dauer, Status, tatsächliche
Hashes, Mismatches, `verification_scope` (`run` oder `trial`), einen nur im
Trial-Scope gesetzten `trial_index` und die Angabe, ob ein Dataset-Override
verwendet wurde. Ein Trial-Replay quittiert ausschließlich Event-, Lifecycle-
und Result-Hash dieses Trials sowie bei aktiviertem Synthetic L2 dessen
Sidecar-Hash; es erzeugt weder scheinbare Kampagnenhashes noch einen
Kampagnenreport. Ein Receipt ist wegen seiner Observability-Felder
absichtlich nicht bitidentisch zwischen Runs.

Ein kleines schema-validiertes Beispiel liegt unter
[`docs/examples/run_manifest.v1.json`](../examples/run_manifest.v1.json).

### Source- und Build-Identität

Der Buildblock erfasst vollständige Git-SHA, Dirty-State und bei Dirty-State
einen stabilen SHA-256 über tracked Diff plus sortierte untracked Inhalte. Er
enthält Buildtyp, CMake-Profil, CMake-/Generator- und effektive Buildflags,
Compiler, Standardbibliothek, libc, Architektur, `-march`, Floating-Point-Flags,
konfigurationsspezifische C-/C++- und Executable-Linkerflags sowie SHA-256 von
C-/C++-Compiler, Linker, Archiver, Ranlib und gegebenenfalls Toolchain-Datei.
Profil v1 ist auf direkte GNU-ELF-Treiber begrenzt und hasht zusätzlich
`cc1`, `cc1plus`, `collect2` sowie den vom Driver aufgelösten Assembler und
Linker; dieselben Auflösungen werden unmittelbar vor dem Build erneut geprüft.
Auch die
kanonischen `/usr/bin/cmake`, `/usr/bin/git` und `/usr/bin/ninja` werden gehasht
und vor dem Build erneut validiert. Der SHA-256 des tatsächlich gestarteten
Executable ist Bestandteil der Build-Identität.
FetchContent-Abhängigkeiten sind auf unveränderliche Commit-SHAs gepinnt und
werden beim Configure sowie vor jedem First-Party-Build auf exakten HEAD, einen
sauberen Worktree und verbotene `assume-unchanged`-/`skip-worktree`-Bits
geprüft. Das Manifest hasht außerdem jedes tatsächlich geladene ELF-Objekt
sowie den sortierten Gesamtsatz; im Profil v1 sind genau `libstdc++`, `libm`,
`libc`, `libgcc_s` und der x86-64-Loader zulässig. Loader-Interposition und
Laufzeit-Tunables (`LD_PRELOAD`, `LD_AUDIT`, `LD_LIBRARY_PATH`,
`LD_HWCAP_MASK`, `GLIBC_TUNABLES`) führen vor Run-Beginn zum Fehler. Hinzu
kommt eine Laufzeitprüfung auf `FE_TONEAREST`, deaktiviertes DAZ/FTZ und die
erwartete x86-MXCSR-/x87-Steuerung. Ein Container-/Toolchain-Digest wird
gespeichert, falls einer tatsächlich gesetzt wurde.
Dieselben Loader-Suchpfad-/HWCAP-Variablen werden für Profil v1 schon beim
Configure und erneut unmittelbar vor dem Build abgewiesen. Der generierte
Build-Header enthält dort anstelle der Configure-Wandzeit einen festen Marker;
Ausführungszeitpunkte gehören ausschließlich ins Receipt.
Die Git-Identität wird vor jedem First-Party-Build neu erzeugt; ein normaler
Source-Edit mit anschließendem Build kann deshalb keinen veralteten Dirty-Diff-
Hash im Binary behalten. `not-hermetic` ist eine ehrliche Kennzeichnung und
kein erfundener Digest.

### Dataset-Identität

Jede Eingabe besitzt eine stabile logische ID, Bytegröße und SHA-256. Die
logischen IDs werden sortiert; der Dataset-Gesamthash ist SHA-256 über diese
kanonische Deskriptorliste. Lokale Mehrdatei-Runs laden dieselbe normalisierte
Reihenfolge. Das Manifest speichert außerdem:

- Dataset-ID und Schema-Version,
- Start und Ende in UTC (`unix-ns:<wert>:UTC` bei lokalen Daten),
- Event-Ordering und Tie-Breaking,
- Warmup-Angabe.

Vor Replay werden Existenz, Größe und SHA-256 geprüft. Noch vor Provider-
Erzeugung kopiert der lokale deterministische Pfad jede normalisierte Eingabe
in `artifacts/dataset/input_<index>`, vergleicht Original und Snapshot per
Größe/SHA-256 und lässt den Parser ausschließlich diesen Snapshot konsumieren.
Nach dem Parsen werden Snapshot-Bytes, Zeitraum, Schema und die vollständige
Ordering-/Tie-Breaking-Identity erneut gegen das Manifest geprüft. Damit kann
eine Mutation des ursprünglichen Pfads während des Runs nicht zu anderen
konsumierten als gehashten Bytes führen. Parser-Rejects und Symbole außerhalb
des einen vollständigen Instrument-Snapshots sind harte Fehler. Tick-Replay
ist bis zu einer echten Multi-File-Tick-Ingestion auf genau eine Datei begrenzt.

Synthetische Kampagnen besitzen kein veränderliches Eingabefile. Stattdessen
wird der vollständige kanonische Generatordeskriptor als eingebettetes Dataset
gehasht.

`--allow-dataset-mismatch` ist nur bei einem Manifest-Replay zulässig. Der
Override steht laut im Receipt, nennt die konkreten Größen-/SHA-/Missing-
Abweichungen, setzt `exact_reproduction` auf `false` und kann weiterhin zu
Output-Hash-Mismatches führen. Eine fehlende Datei bleibt trotz Override ein
Run-Fehler, weil keine Bytes ausgeführt werden können. Ohne Override bricht
Replay vor der Engine ab.

### Vollständige effektive Konfiguration

Das Manifest speichert aufgelöste Defaults und Overrides, nicht nur die vom
Nutzer genannten Werte. Enthalten sind insbesondere:

- Target, Run-Modus, Threading und Workerzahl,
- Strategie-ID, Source-Contract-Version und alle effektiven Parameter,
- Simulator-, Queue-, Fill-, Latency-, Impact-, Fee-, GBM- und L2-Versionen,
- Realism-, Execution-, Exit- und Risk-Konfiguration,
- Instrument-Snapshot mit Venue, Typ, Tick/Lot und Mindestgrenzen,
- explizite Preis-/Mengenrundung und Mindestwert-Validierung,
- vollständiger Fee-Snapshot und verwendete Funding-Konfiguration,
- Master-, Trial- und Komponenten-Seeds.

Lokale Runs erfassen außerdem die vollständige effektive Exit-Konfiguration
einschließlich Stop-Loss, Take-Profit und Trailing-Prozentwert; explizite,
modefremde CLI-Optionen werden auch dann abgelehnt, wenn ihr Wert zufällig dem
normalen Default entspricht.

Eine fehlende Modellversion oder ein Widerspruch zwischen Manifestabschnitten
ist ein Konfigurationsfehler. Ein exakt replaybares Manifest akzeptiert nur die
exakte, aus Run-Modus und Trial-Anzahl abgeleitete Hash-Keymenge; unbekannte
scheinbare Artefakthashes werden abgelehnt. Replay akzeptiert keine
konkurrierenden deterministischen CLI-/Config-Overrides.

JSON-Run-Konfigurationen können die wiederholbaren Felder `params` und
`instrument` als String-Arrays sowie `risk.max_gross_leverage` und
`risk.unwind` angeben. CLI-Werte behalten die bestehende höhere Priorität.

## Kanonische Serialisierung

Für alle gehashten JSON-Artefakte gilt Canonical JSON v1:

- UTF-8 und keine insignifikanten Whitespaces oder Pretty-Print-Abhängigkeit,
- Objektfelder lexikographisch sortiert,
- Arrays in ihrer fachlich definierten Reihenfolge,
- Dezimaldarstellung von Integern,
- finite Binary64-Roundtrip-Darstellung in klassischer Locale,
- `-0.0` wird `0`, NaN und Infinity werden abgelehnt,
- fehlend und `null` sind verschiedene Werte,
- Enums werden als stabile symbolische Werte geschrieben,
- Zeitformate sind explizit UTC,
- binäre Eventfelder folgen der versionierten Event-Log-Spezifikation:
  little-endian Integer und IEEE-754 Binary64. Auf einer inkompatiblen
  Big-Endian- oder Nicht-Binary64-Plattform kompiliert dieser Vertrag nicht.

`unordered_map`-Reihenfolgen werden durch die kanonische Objektstruktur
neutralisiert. Ein Artefakt enthält seinen eigenen Hash nie in den zu hashenden
Bytes.

SHA-256 wird für Dataset, Run-Fingerprint, Event-Log, Lifecycle, optionales
Synthetic-L2-Sidecar, Trial-Result, Economic Result und Report verwendet.
Trial-, Economic-Result- und Report-Preimages tragen jeweils eine explizite
Schema-Version. Wandzeitbasierte
Tick-to-Trade-Observability-Felder werden im lokalen deterministischen Report
explizit als ausgeschlossen markiert und nicht in dessen Hash aufgenommen.

## Artefakte und Trial-Lifecycle

Eine Monte-Carlo-Kampagne schreibt:

```text
run_manifest.json
artifacts/
  report.json
  run_receipt.json
  trials/
    trial_000000/
      events.zst
      lifecycle.jsonl
      synthetic_l2.jsonl  # nur bei emit_l2=true
      result.json
      trial_manifest.json
```

Der Trial-Manifestblock enthält stabilen Index, Run-Fingerprint, Trial-Seed,
alle Komponenten-Seeds, effektive Trial-Konfiguration, Status/Fehler und die
einzelnen Hashes. Bei aktiviertem Synthetic L2 enthält er zusätzlich den
SHA-256 des kanonischen `synthetic_l2.jsonl`-Sidecars. `IOrderAuditSink`
zeichnet Intent/Create, Submit,
Statusübergänge (einschließlich ACK/Working/Open/Expire, soweit die bestehende
Order-State-Semantik sie emittiert), Partial Fill, Fill, Cancel Request,
Cancelled, Reject und Amend auf. Eine Preallocation verhindert zusätzliche
Heap-Allokationen in den Audit-Callbacks; Kapazitätsüberlauf ist laut und
fail-closed. Schema v1 reserviert dafür explizit und im Manifest sichtbar
131072 Records pro Run/Trial; es gibt weder Runtime-Growth noch einen
automatischen Hash-only-Fallback.

Dateien entstehen zunächst als `.partial` und werden erst nach erfolgreicher
Finalisierung atomar umbenannt. Verschiedene Trial-Indizes überschreiben sich
nicht. Ein vorhandenes komplettes oder partielles Ziel wird nicht still
fortgesetzt. Fehlgeschlagene Trials erhalten ein `status: failed`-Manifest;
ein verbliebenes `.partial` kennzeichnet einen Prozessabbruch vor sauberer
Finalisierung.

Lokale deterministische Backtests verwenden analog `events.zst`,
`lifecycle.jsonl`, `report.json` und `run_receipt.json` direkt im
Artefaktverzeichnis; unter `dataset/` liegen außerdem exakt die Bytes, die der
Parser konsumiert hat. Ein früher Fehler schreibt ein `status: failed`-Receipt,
publiziert aber kein finales `run_manifest.json`.

## Runs und Replays

### Lokaler Backtest

Der lokale deterministische Modus verlangt einen vollständigen
Instrument-/Fee-Snapshot und läuft ausschließlich im technisch nicht
schreibfähigen `engine_backtest`-Target:

```bash
./out/build/linux-deterministic/engine_backtest \
  --provider local --path data/bars.csv --format bar \
  --symbol BTCUSDT \
  --instrument BTCUSDT:tick=0.01,lot=0.000001,minq=0.000001,minn=5,maker=0.0002,taker=0.0004 \
  --strategy sma --seed 424242 --thread-preset logging \
  --write-run-manifest artifacts/run_manifest.json \
  --artifacts-dir artifacts/run --no-pin --no-tui --status-format off
```

Private/networkseitige Provider, externe Persistence, Checkpoints, Desk/Web,
ungebundene Depth-Datasets, `--walked-book-impact` ohne gehashtes L2-Dataset
und zusätzliche Ergebnisexporte werden in diesem Modus abgelehnt statt still
aus dem Fingerprint zu fallen.

`--log-file` bleibt als reiner operationaler Text-Observer zulässig. Seine
Wandzeitzeilen sind ausdrücklich weder Engine-Input noch Bestandteil von
Event-, Lifecycle-, Report- oder Economic-Hash. QuestDB-Persistenz wird für
deterministische Manifest-Runs vollständig abgelehnt.

Die vollständige deterministische Persistenz verwendet absichtlich das
`logging`-Preset mit genau einem Logging-Worker pro aktivem Trial,
deaktiviertem Pinning und `block`-Drop-Policy. Der Worker schreibt Event-Log
und zstd außerhalb des Engine-Event-Loops; ein voller Ring erzeugt
deterministischen Backpressure und verliert kein Event. `inline` und `light`
werden für diesen Modus abgelehnt. Bei parallelen Kampagnen kommen deshalb zu
den expliziten Trial-Workern genauso viele isolierte Logging-Worker hinzu;
diese Ausführungshülle ist im Manifest erfasst.

### Monte Carlo

```bash
./out/build/linux-deterministic/engine_backtest \
  --monte-carlo --mc-trials 100 \
  --mc-params n_steps=2000,sigma=0.4 \
  --strategy mean-reversion --seed 424242 \
  --write-run-manifest artifacts/run_manifest.json \
  --artifacts-dir artifacts/campaign --no-pin --no-tui
```

Ein manifestgestützter Standalone-Aufruf mit `--provider synthetic` wird
sichtbar auf den kanonischen Ein-Trial-Monte-Carlo-Envelope (Trial 0)
abgebildet. Er ist deshalb nicht als byte- oder ergebnisgleiche Variante eines
älteren Synthetic-Provider-Aufrufs ohne Manifest zu verstehen. Für einen
vergleichbaren Ausgangslauf muss bereits der erste Lauf im Manifest-Modus
erzeugt werden; das Manifest ist danach die Replay-Autorität.

Unbekannte oder ungültige MC-Parameter führen zu einem Fehler; es gibt keinen
Fallback auf einen Defaultwert. Ein `seed` innerhalb von `--mc-params` ist
verboten; einzig der explizite Master Seed ist Seed-Autorität. Der aktuelle
ökonomische Trial-Consumer verarbeitet Bars. Darum werden `--format tick`,
externe Provider-/Path-/Depth-Eingaben, `--queue-model` und
`--walked-book-impact` bis zu einem passenden Consumer hart abgelehnt. Gleiches
gilt für eine Wire-Latenz, die der lokale Execution-Adapter nicht konsumiert,
eine Latenz-Stddev ohne positive Mean-Latenz sowie L2-Kalibrierungswerte bei
deaktiviertem L2.

`--mc-params emit_l2=true` führt dagegen den vorhandenen stylisierten
Synthetic-L2-Generator reproduzierbar aus. Jeder Trial schreibt die zeit- und
levelstabil kanonisierten Snapshots nach `synthetic_l2.jsonl`; Full- und
Einzeltrial-Replay prüfen den SHA-256 als `trial_<index>.synthetic_l2`. Das
Manifest kennzeichnet diesen Vertrag als
`observational-artifact-only-not-consumed-by-engine` und
`affects_economic_execution: false`. Dadurch ist die L2-Modellausgabe technisch
reproduzierbar, ohne vorzutäuschen, dass sie bereits Fill-/Queue-Ökonomie
beeinflusst.

Manifest-Runs verwenden das feste `logging`-Preset; explorative MC-Runs das
tatsächlich verwendete `inline`-Preset. Andere Thread-/Spin-Vorgaben,
providerseitige Live-/Backfill-/DMS-/Reconciliation-Optionen, der alte
`--fill-rng-seed`, `--spread-step` und Replay-only `--verify-hashes` sind im
MC-Erstlauf Fehler statt stille No-ops.

### Vollständiges Replay

```bash
./out/build/linux-deterministic/engine_backtest \
  --replay-run-manifest artifacts/run_manifest.json \
  --artifacts-dir artifacts/replay-01 --verify-hashes
```

### Einzelnen Trial reproduzieren

```bash
./out/build/linux-deterministic/engine_backtest \
  --replay-run-manifest artifacts/run_manifest.json \
  --artifacts-dir artifacts/replay-trial-42 \
  --trial 42 --verify-hashes
```

Die Ausgabe nennt je Hash-Art `MATCH` oder `MISMATCH`, erwarteten und
tatsächlichen SHA-256. Build-/Dataset-Fehler brechen vor dem Run ab; ein
Output-Mismatch liefert ebenfalls einen Exit-Code ungleich null. Jedes Replay
benötigt ein neues Artefaktverzeichnis, damit keine ältere Evidenz überschrieben
wird. Die Replay-CLI ist eine Positivliste: Neben Manifest, neuem
Artefaktverzeichnis, `--verify-hashes`, `--allow-dataset-mismatch` und
gegebenenfalls `--trial` wird keine weitere explizite Option akzeptiert;
insbesondere werden `--dry-run`, Config- oder Modell-Overrides nicht still
ignoriert.

## Deterministische Parallelisierung

Parallele Trials besitzen isolierte RNGs und Order-ID-Scopes. Ergebnisse werden
in indexstabilen Slots veröffentlicht, vor Aggregation nach Trial-Index sortiert
und in fester Reihenfolge mit kompensierter Summation reduziert. Object-Reuse
setzt Datenserie, Strategie und RNG einschließlich Sample-Cache zurück.

Damit liefern ein Worker und mehrere Worker dieselben Trial- und
Economic-Result-Hashes für die unterstützten Kampagnen. Die konfigurierte
Workerzahl bleibt als deterministischer Konfigurationseingang im Manifest;
dadurch ändern sich Run-Fingerprint und der ihn enthaltende Report-Hash bewusst
zwischen einer 1-Worker- und einer N-Worker-Konfiguration. Wiederholungen
desselben Manifests liefern dagegen auch denselben Report-Hash. Paralleles
Object-Reuse ist wegen gemeinsamem mutable Zustand verboten.

## Deterministisches Buildprofil und Guard

```bash
cmake --preset linux-deterministic
cmake --build --preset linux-deterministic \
  --target engine_backtest truetest_tests truetest_cli_tests
ctest --preset linux-deterministic --output-on-failure
```

Das Ninja-basierte Profil deaktiviert LTO, Unity-Builds und `-march=native` und
setzt `-fno-fast-math -ffp-contract=off`. Beim ersten Configure dürfen fehlende
FetchContent-Quellen ausschließlich an ihren eingecheckten, unveränderlichen
Commit-SHAs bezogen werden; Updates vorhandener Checkouts bleiben abgeschaltet.
Vorhandene und neu bezogene Dependency-Checkouts müssen anschließend exakt den
gepinnten, sauberen Commits entsprechen. So funktioniert das Profil auch aus
einem sauberen Checkout, ohne Versions-Fallback oder stilles Upgrade. Optionale
Provider-, UI-, Debug-, Sanitizer-, Shared-Library- und Benchmark-Features sind
für Profil v1 fest ausgeschaltet. CMake verweigert Launcher, alternative
Linker-/Toolchain-Flags, Sprachregel-Injektionen, externe Include-/Response-/
Object-Dateien in globalen Flags, lokale Dependency-Overrides und jede unter dem
Profilnamen `linux-deterministic-v1` abgeschwächte Konfiguration. CTest prüft
Preset, Cache, reale Compile-Commands und die Build-Time-Source-Identity
einschließlich eines stale-Diff-Negativfalls. Pre-Build-Guards prüfen zudem
Toolchain-Binaries und die sauberen, exakten Dependency-Commits erneut.
CMake, Git, Ninja, C-/C++-Compiler, geladene Laufzeitobjekte, Architektur,
Flags und Dependency-Identitäten definieren den lokalen Envelope; Configure-
Wandzeit ist davon explizit ausgeschlossen. Ohne gesetzten Container-Digest
ist er bewusst kein hermetischer Container und keine Cross-Host-Garantie.

Der interne Schalter `TRUETEST_DETERMINISTIC_BUILD` ist allein absichtlich
nicht ausreichend: Er wird ohne das vollständige Profil fail-closed
abgewiesen. Reproduzierbare Builds werden ausschließlich über das
`linux-deterministic`-Preset konfiguriert.

Der Source-Guard läuft mit:

```bash
./scripts/check-deterministic-entropy.sh
```

Er prüft die expliziten stochastic-owner-Pfade und lehnt direkte Standard-RNGs,
Standardverteilungen, `random_device`, Clock-Seeding und `std::hash` ab. Eng
allowlistete Clock-/Entropy-Nutzung für die Live-Bestätigungs-Challenge und rein
beobachtende QuestDB-Run-Tags beziehungsweise Tick-to-Trade-Latenzmessung
bleibt getrennt von Simulation und Seedlogik. Der Guard-Selbsttest beweist,
dass zusätzliche Clock- oder `std::hash`-Nutzung auch in den lokalen
Execution-Adaptern abgewiesen wird.

## Bekannte Grenzen

- **Identischer vollständig erfasster Build-Envelope:** getestetes Ziel ist
  Bitidentität der beschriebenen Artefakte. Ein tatsächlich hermetischer Build
  setzt zusätzlich einen realen Container-/Toolchain-Digest voraus; Profil v1
  kennzeichnet dessen Fehlen ausdrücklich als `not-hermetic`.
- **Andere CPU:** nicht zugesichert; FP-/libm- und Architekturunterschiede
  können letzte Bits ändern.
- **Anderer Compiler oder andere Standardbibliothek:** nicht zugesichert und
  durch Build-Verifikation standardmäßig abgelehnt.
- **Statistische Reproduzierbarkeit:** feste Streams sind zusätzlich mit
  statistischen Sanity-Tests abgesichert; das ersetzt keine Modellkalibrierung.
- **Fachliche Modellvalidität:** Queue-, Fill-, GBM-, L2-, Latency- und
  Impact-Versionierung macht ein Modell reproduzierbar, aber nicht automatisch
  realistisch oder profitabel.
- **Synthetic L2:** Generator, domänenseparierter Zufallsstrom, kanonisches
  Sidecar und Replay-Hash sind deterministisch getestet. Der heutige
  Monte-Carlo-Engine-Consumer nimmt weiterhin nur Bars an; deshalb ist L2 im
  Manifest ausdrücklich nicht ökonomisch wirksam. Eine Fill-/Queue-wirksame
  L2-Ingestion wäre Fidelity-Arbeit (R2), nicht Teil von R6.
- **Lifecycle-Speicher:** Vollpersistenz reserviert pro gleichzeitig aktivem
  Trial die im Manifest festgehaltene, begrenzte Lifecycle-Kapazität. Der
  konservative Standard von 131072 Records entspricht auf dem aktuellen ABI
  ungefähr 113 MiB Adressraum je aktivem Trial; hohe Parallelität muss daher
  bewusst innerhalb des verfügbaren Speichers gewählt werden. Überlauf bleibt
  ein lauter Trial-Fehler und führt nie zu einem unvollständig als exakt
  markierten Artefakt.
- **Event-Log-Portabilität:** das vorhandene binäre Format und zstd-Artefakt
  sind innerhalb der gespeicherten Toolchain-/Dependency-Grenze geprüft, nicht
  als universelles plattformneutrales Wireformat zertifiziert.
