# A: Adaptive Hybrid Strategy (previous / lower-priority branch work)

**See**: docs/todos/00-OVERVIEW.md for full rules, reference format, maintenance, and mapping. High-level also in thin `docs/governance/03-todo.md`. See detailed spec in `docs/reference/06-adaptive-hybrid-strategy.md`; code in `src/strategy/adaptive_hybrid_*`; test in `tests/test_adaptive_hybrid.cpp`. MC-05 context. Active development focus has shifted to Monte Carlo simulation on the `monte-carlo` branch (now integrated). Items below remain for historical context or future resumption. The strategy is registered and L2 dispatch is present (with `LIVE_SAFETY_CCB_APPROVED` comment); usable for MC backtests/experiments with caveats.

- **A-01** Replace v1/demo placeholders in `AdaptiveHybridStrategy` / `RiskValidator` / `OnChainMonitor` (simplified decision logic, mock OnChain thread, equity proxies, omitted L2Snapshot handling, "real version" comments) with full hot-path deterministic implementation. (Visible in `src/strategy/adaptive_hybrid_strategy.cpp:467` "Simplified decision for compilable demo — real version uses full RiskValidator + L2Snapshot"; many "placeholder", "real impl would", "v1 simplified", "for paper/backtest; production wires real feed"; `enable_onchain_mock=true` default + mock producer thread.)
- **A-02** Add real on-chain data feed integration (TRON/Helius or equivalent) and proper spike detection instead of the current always-false stub. (Mock only; `inject_spike` test hook only; "No automatic spikes in mock".)
- **A-03** Implement `take_pending_exit_intents` for the strategy (consistent with breakout / other strategies that use `ExitManager`).
- **A-04** Exercise the full 9-step flow + RiskValidator gates in real backtests and shadow runs; remove "for compilable demo / harness only" limitations.
- **A-05** Add `adaptive-hybrid` to the strategy matrix / golden tests and TUI indicators as a first-class citizen.
- **A-06** Wire ATR (recently added) + other indicators cleanly into the adaptive regime detection.
- **A-07** Any L2 dispatch or safety-surface touches for adaptive hybrid must carry `LIVE_SAFETY_CCB_APPROVED`.

(See detailed spec in `docs/reference/06-adaptive-hybrid-strategy.md`; code in `src/strategy/adaptive_hybrid_*`; test in `tests/test_adaptive_hybrid.cpp`. On-chain mock + simplified gates are the main blockers.)

**Last updated**: 2026-07-03 (split from governance/03-todo.md per TODOS-SPLIT-SPEC; verbatim extraction + note on lower-pri/MC; see 00-OVERVIEW.md).
