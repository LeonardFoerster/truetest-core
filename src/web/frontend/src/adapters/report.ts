/* =========================================================================
   TrueTest — report adapter.

   Maps the engine's ResultsReport (./wire.ts, faithful to AnalyticsReport)
   onto the Backtest Review component prop shapes (./types.ts). Reconciles:
   max_drawdown %→negative fraction, win_rate 0..100→0..1, [ts,equity] pair
   curves → indexed {i,v,dd} series (recomputing drawdown), trades→blotter
   (buy/sell→long/short, slippage bps), trades→entry/exit markers, and builds
   the P&L histogram + per-symbol/strategy breakdowns the engine doesn't
   pre-aggregate for the chart.
   ========================================================================= */
import type { ResultsReport, WireSubAnalytics, WireTrade } from "../wire";
import type { Backtest, BTPoint, BTMarker, BlotterRow, HistBin, BreakdownRow } from "../types";

export interface ReportData {
  backtest: Backtest;
  btCurve: { strat: BTPoint[]; bench: BTPoint[] };
  btMarkers: BTMarker[];
  curveDisclosure: string;
  blotter: BlotterRow[];
  hist: HistBin[];
  bySymbol: BreakdownRow[];
  byStrategy: BreakdownRow[];
}

export const REPORT_LIMITS = Object.freeze({
  curvePoints: 100_000,
  tradeReturns: 100_000,
  tradeRows: 20_000,
  breakdownRows: 512,
  visualCurvePoints: 2_048,
  visibleBreakdownRows: 32,
  identifierChars: 256,
  reasonChars: 1_024,
  reportBytes: 32 * 1024 * 1024,
});

const REQUIRED_FINITE_FIELDS = ["rolling_sharpe", "rolling_max_drawdown",
  "avg_win", "avg_loss", "largest_winner", "largest_loser",
  "time_in_market_pct", "avg_holding_period_ms", "avg_slippage",
  "avg_slippage_signed", "avg_adverse_slippage", "avg_favorable_slippage",
  "avg_tick_to_trade_ns", "min_tick_to_trade_ns", "max_tick_to_trade_ns"] as const;
const REQUIRED_COUNT_FIELDS = ["winning_trades", "total_orders", "total_fills",
  "adverse_slippage_count", "favorable_slippage_count",
  "tick_to_trade_samples", "duplicate_fill_replays_ignored",
  "conflicting_fill_replays_rejected", "missing_fill_identities_rejected",
  "invalid_fill_payloads_rejected", "unreconciled_funding_events_rejected",
  "duplicate_funding_replays_ignored",
  "conflicting_funding_replays_rejected", "late_fill_events_rejected",
  "late_funding_events_rejected"] as const;
const OPTIONAL_COUNT_FIELDS = ["ambiguous_portfolio_mark_sequences_rejected",
  "late_market_events_rejected",
  "duplicate_market_marks_ignored", "conflicting_market_marks_rejected"] as const;
const MAX_JAVASCRIPT_DATE_MS = 8_640_000_000_000_000;

function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === "object" && value !== null && !Array.isArray(value);
}

function isFiniteNumber(value: unknown): value is number {
  return typeof value === "number" && Number.isFinite(value);
}

function isNonNegativeSafeInteger(value: unknown): value is number {
  return Number.isSafeInteger(value) && (value as number) >= 0;
}

function rateMatches(count: number, total: number, rate: number): boolean {
  if (total === 0) return count === 0 && rate === 0;
  const expected = count / total * 100;
  return Math.abs(rate - expected) <= 1e-8 * Math.max(1, Math.abs(expected));
}

function expectedProfitFactorReason(totalWin: number, totalLoss: number): string {
  if (totalLoss > 0) return "computed_from_gross_win_and_loss";
  if (totalWin > 0) return "no_losses_unbounded";
  return "no_winning_or_losing_trades";
}

function accountingTolerance(r: Record<string, unknown>): number {
  // Match the engine's fail-closed rule. Scaling by gross components would
  // hide residuals that are material relative to the remaining equity after
  // large cancelling cashflows.
  const scale = Math.max(1, Math.abs(r.final_equity as number));
  return 64 * Number.EPSILON * scale;
}

function hasFundingReconciliationFailure(r: Record<string, unknown>): boolean {
  return [r.unreconciled_funding_events_rejected,
    r.conflicting_funding_replays_rejected,
    r.late_funding_events_rejected].some((count) =>
      isNonNegativeSafeInteger(count) && count > 0);
}

function hasFillReconciliationFailure(r: Record<string, unknown>): boolean {
  return [r.missing_fill_identities_rejected,
    r.conflicting_fill_replays_rejected,
    r.invalid_fill_payloads_rejected,
    r.late_fill_events_rejected].some((count) =>
      isNonNegativeSafeInteger(count) && count > 0);
}

function expectedAccountingReason(r: Record<string, unknown>): string {
  if (hasFillReconciliationFailure(r)) return "unreconciled_fill_event";
  if (hasFundingReconciliationFailure(r))
    return "unreconciled_funding_settlement";
  if (Math.abs(r.reconciliation_residual as number) > accountingTolerance(r))
    return "accounting_identity_outside_floating_tolerance";
  return "reconciled_within_floating_tolerance";
}

function isBoundedString(value: unknown, maxChars: number): value is string {
  return typeof value === "string" && value.length <= maxChars;
}

function isBoundedNonBlankString(value: unknown, maxChars: number): value is string {
  if (!isBoundedString(value, maxChars) || value.trim().length === 0)
    return false;
  for (let i = 0; i < value.length; ++i) {
    const code = value.charCodeAt(i);
    if (code < 0x20 || code === 0x7f) return false;
  }
  return true;
}

function hasValidSubAnalytics(value: unknown): boolean {
  if (!isRecord(value)) return false;
  let count = 0;
  for (const key in value) {
    if (!Object.prototype.hasOwnProperty.call(value, key)) continue;
    if (!isBoundedNonBlankString(key, REPORT_LIMITS.identifierChars)) return false;
    if (++count > REPORT_LIMITS.breakdownRows) return false;
    const entry = value[key];
    if (!isRecord(entry) || !isFiniteNumber(entry.total_pnl) ||
        !isNonNegativeSafeInteger(entry.trade_count) ||
        !isNonNegativeSafeInteger(entry.win_count) ||
        entry.win_count > entry.trade_count ||
        !isFiniteNumber(entry.win_rate) ||
        !rateMatches(entry.win_count, entry.trade_count, entry.win_rate) ||
        entry.win_rate < 0 || entry.win_rate > 100 ||
        !isFiniteNumber(entry.profit_factor) || entry.profit_factor < 0 ||
        typeof entry.profit_factor_valid !== "boolean" ||
        typeof entry.profit_factor_unbounded !== "boolean" ||
        !isBoundedNonBlankString(entry.profit_factor_reason,
          REPORT_LIMITS.reasonChars) ||
        !isFiniteNumber(entry.total_win) || entry.total_win < 0 ||
        !isFiniteNumber(entry.total_loss) || entry.total_loss < 0 ||
        Math.abs(entry.total_pnl - (entry.total_win - entry.total_loss)) >
          64 * Number.EPSILON * Math.max(1, Math.abs(entry.total_pnl)) ||
        entry.profit_factor_valid !== (entry.total_loss > 0) ||
        entry.profit_factor_unbounded !==
          (entry.total_loss === 0 && entry.total_win > 0) ||
        entry.profit_factor_reason !==
          expectedProfitFactorReason(entry.total_win, entry.total_loss) ||
        (entry.profit_factor_valid &&
          entry.profit_factor !== entry.total_win / entry.total_loss) ||
        (!entry.profit_factor_valid && entry.profit_factor !== 0)) return false;
  }
  return true;
}

function hasValidTrades(value: unknown): boolean {
  if (!Array.isArray(value) || value.length > REPORT_LIMITS.tradeRows)
    return false;
  const localIdentities = new Set<string>();
  const venueIdentities = new Set<string>();
  for (const trade of value) {
    if (!isRecord(trade) || typeof trade.order_id !== "string" ||
        !/^[1-9][0-9]{0,19}$/.test(trade.order_id) ||
        BigInt(trade.order_id) > 18_446_744_073_709_551_615n ||
        typeof trade.fill_id !== "string" ||
        !/^(0|[1-9][0-9]{0,19})$/.test(trade.fill_id) ||
        BigInt(trade.fill_id) > 18_446_744_073_709_551_615n ||
        !isBoundedString(trade.venue_execution_id, REPORT_LIMITS.identifierChars) ||
        (trade.venue_execution_id.length > 0 &&
          !isBoundedNonBlankString(trade.venue_execution_id,
            REPORT_LIMITS.identifierChars)) ||
        (trade.fill_id === "0" && trade.venue_execution_id.trim().length === 0) ||
        (trade.side !== "buy" && trade.side !== "sell") ||
        !isFiniteNumber(trade.quantity) || trade.quantity <= 0 ||
        !isFiniteNumber(trade.fill_price) || trade.fill_price <= 0 ||
        !isFiniteNumber(trade.commission) ||
        !isBoundedString(trade.commission_currency,
          REPORT_LIMITS.identifierChars) ||
        (trade.commission !== 0 &&
          !isBoundedNonBlankString(trade.commission_currency,
            REPORT_LIMITS.identifierChars)) ||
        !isFiniteNumber(trade.intended_price) || trade.intended_price < 0 ||
        !isNonNegativeSafeInteger(trade.ts_ms) || trade.ts_ms === 0 ||
        trade.ts_ms > MAX_JAVASCRIPT_DATE_MS ||
        !isFiniteNumber(trade.pnl) ||
        (trade.intended_price !== 0 &&
          !Number.isFinite((trade.fill_price / trade.intended_price - 1) * 1e4)) ||
        !isBoundedNonBlankString(trade.symbol, REPORT_LIMITS.identifierChars) ||
        !isBoundedNonBlankString(trade.strategy_name,
          REPORT_LIMITS.identifierChars)) return false;
    if (trade.fill_id !== "0") {
      const localIdentity = `${trade.order_id}\0${trade.fill_id}`;
      if (localIdentities.has(localIdentity)) return false;
      localIdentities.add(localIdentity);
    }
    if (trade.venue_execution_id.length > 0) {
      const venueIdentity = `${trade.symbol}\0${trade.venue_execution_id}`;
      if (venueIdentities.has(venueIdentity)) return false;
      venueIdentities.add(venueIdentity);
    }
  }
  return true;
}

function assertResultsReport(value: unknown): asserts value is ResultsReport {
  const optionalBooleans = ["valuation_complete", "portfolio_time_series_valid",
    "annualized_return_valid", "sharpe_ratio_valid", "sortino_ratio_valid",
    "calmar_ratio_valid", "benchmark_valid"];
  const optionalStrings = ["valuation_reason", "portfolio_time_series_reason",
    "annualized_return_reason", "sharpe_ratio_reason", "sortino_ratio_reason",
    "calmar_ratio_reason", "benchmark_reason", "annualized_return_basis",
    "time_in_market_reason"];
  if (!isRecord(value) || value.schema_version !== 1 ||
      !isFiniteNumber(value.initial_equity) || value.initial_equity <= 0 ||
      !isFiniteNumber(value.final_equity) ||
      !isFiniteNumber(value.gross_realized_pnl) ||
      !isFiniteNumber(value.realized_pnl) ||
      !isFiniteNumber(value.funding_pnl) ||
      !isFiniteNumber(value.unrealized_pnl) ||
      !isFiniteNumber(value.total_commission) ||
      !isFiniteNumber(value.reconciliation_residual) ||
      typeof value.accounting_reconciled !== "boolean" ||
      !isBoundedNonBlankString(value.accounting_reconciliation_reason,
        REPORT_LIMITS.reasonChars) ||
      value.accounting_reconciliation_reason !==
        expectedAccountingReason(value) ||
      !isFiniteNumber(value.cumulative_return) ||
      !isFiniteNumber(value.win_rate) || value.win_rate < 0 || value.win_rate > 100 ||
      !isFiniteNumber(value.total_win) || value.total_win < 0 ||
      !isFiniteNumber(value.total_loss) || value.total_loss < 0 ||
      !isFiniteNumber(value.profit_factor) || value.profit_factor < 0 ||
      typeof value.profit_factor_valid !== "boolean" ||
      typeof value.profit_factor_unbounded !== "boolean" ||
      !isBoundedNonBlankString(value.profit_factor_reason,
        REPORT_LIMITS.reasonChars) ||
      value.profit_factor_valid !== (value.total_loss > 0) ||
      value.profit_factor_unbounded !==
        (value.total_loss === 0 && value.total_win > 0) ||
      value.profit_factor_reason !==
        expectedProfitFactorReason(value.total_win, value.total_loss) ||
      (value.profit_factor_valid &&
        value.profit_factor !== value.total_win / value.total_loss) ||
      (!value.profit_factor_valid && value.profit_factor !== 0) ||
      !isNonNegativeSafeInteger(value.total_trades) ||
      (value.total_trades === 0 && value.win_rate !== 0) ||
      !Array.isArray(value.equity_curve) ||
      value.equity_curve.length > REPORT_LIMITS.curvePoints ||
      !Array.isArray(value.benchmark_equity_curve) ||
      value.benchmark_equity_curve.length > REPORT_LIMITS.curvePoints ||
      !Array.isArray(value.trade_returns) ||
      value.trade_returns.length > REPORT_LIMITS.tradeReturns ||
      !value.trade_returns.every(isFiniteNumber) ||
      !hasValidSubAnalytics(value.per_symbol) ||
      !hasValidSubAnalytics(value.per_strategy) ||
      !hasValidTrades(value.trades) ||
      REQUIRED_FINITE_FIELDS.some((key) => !isFiniteNumber(value[key])) ||
      REQUIRED_COUNT_FIELDS.some((key) => !isNonNegativeSafeInteger(value[key])) ||
      OPTIONAL_COUNT_FIELDS.some((key) => value[key] !== undefined &&
        !isNonNegativeSafeInteger(value[key])) ||
      (isNonNegativeSafeInteger(value.winning_trades) &&
        (value.winning_trades > value.total_trades ||
          !rateMatches(value.winning_trades, value.total_trades, value.win_rate))) ||
      (isFiniteNumber(value.initial_equity) && isFiniteNumber(value.final_equity) &&
        isFiniteNumber(value.cumulative_return) &&
        (!Number.isFinite(value.final_equity / value.initial_equity - 1) ||
          Math.abs((value.final_equity / value.initial_equity - 1) -
            value.cumulative_return) > 1e-8 *
              Math.max(1, Math.abs(value.cumulative_return)))) ||
      (isFiniteNumber(value.initial_equity) && isFiniteNumber(value.final_equity) &&
        isFiniteNumber(value.realized_pnl) && isFiniteNumber(value.funding_pnl) &&
        isFiniteNumber(value.unrealized_pnl) &&
        isFiniteNumber(value.reconciliation_residual) &&
        (Math.abs((value.final_equity - (value.initial_equity +
          value.realized_pnl + value.funding_pnl + value.unrealized_pnl)) -
          value.reconciliation_residual) > accountingTolerance(value) ||
          value.accounting_reconciled !==
            (Math.abs(value.reconciliation_residual) <= accountingTolerance(value) &&
              !hasFundingReconciliationFailure(value) &&
              !hasFillReconciliationFailure(value)))) ||
      (hasFundingReconciliationFailure(value) &&
        !hasFillReconciliationFailure(value) &&
        (value.accounting_reconciled !== false ||
          value.accounting_reconciliation_reason !==
            "unreconciled_funding_settlement")) ||
      (hasFillReconciliationFailure(value) &&
        (value.accounting_reconciled !== false ||
          value.accounting_reconciliation_reason !==
            "unreconciled_fill_event")) ||
      (isFiniteNumber(value.time_in_market_pct) &&
        (value.time_in_market_pct < 0 || value.time_in_market_pct > 100)) ||
      (isFiniteNumber(value.avg_holding_period_ms) && value.avg_holding_period_ms < 0) ||
      (isFiniteNumber(value.min_tick_to_trade_ns) && value.min_tick_to_trade_ns < 0) ||
      (isFiniteNumber(value.avg_tick_to_trade_ns) && value.avg_tick_to_trade_ns < 0) ||
      (isFiniteNumber(value.max_tick_to_trade_ns) && value.max_tick_to_trade_ns < 0) ||
      (isFiniteNumber(value.min_tick_to_trade_ns) &&
        isFiniteNumber(value.avg_tick_to_trade_ns) &&
        isFiniteNumber(value.max_tick_to_trade_ns) &&
        (value.min_tick_to_trade_ns > value.avg_tick_to_trade_ns ||
          value.avg_tick_to_trade_ns > value.max_tick_to_trade_ns)) ||
      optionalBooleans.some((key) => value[key] !== undefined &&
        typeof value[key] !== "boolean") ||
      value.trade_rows_kind !== "physical_fill_legs" ||
      value.trade_returns_kind !== "closing_fill_legs" ||
      (value.benchmark_symbol !== undefined &&
        !isBoundedNonBlankString(value.benchmark_symbol,
          REPORT_LIMITS.identifierChars)) ||
      optionalStrings.some((key) => value[key] !== undefined &&
        !isBoundedString(value[key], REPORT_LIMITS.reasonChars))) {
    throw new TypeError("invalid_results_report");
  }
}

// The engine emits win-rate as a percent (0..100); components want 0..1.
const pctToFrac = (v: number) => v / 100;

function breakdown(m: Record<string, WireSubAnalytics>): BreakdownRow[] {
  return Object.entries(m)
    .map(([key, sa]) => ({
      key, pnl: sa.total_pnl, win: pctToFrac(sa.win_rate),
      pf: sa.profit_factor_valid ? sa.profit_factor : null,
      pfUnbounded: sa.profit_factor_unbounded,
      pfReason: sa.profit_factor_reason, trades: sa.trade_count,
    }))
    .sort((a, b) => b.pnl - a.pnl);
}

function stratSeries(curve: [number, number][]): BTPoint[] {
  let peak = -Infinity;
  return curve.map(([tsMs, v], i) => {
    peak = Math.max(peak, v);
    return { i, tsMs, v, dd: v / peak - 1 };
  });
}

export function decimateCurve<T extends { tsMs: number; v: number }>(
  points: readonly T[], maxPoints: number,
  additionalMinimum?: (point: T) => number,
): T[] {
  if (points.length <= maxPoints) return Array.from(points);
  if (maxPoints < 4) throw new RangeError("maxPoints must be at least 4");

  const result: T[] = [points[0]];
  const interiorCount = points.length - 2;
  const extremaPerBucket = additionalMinimum ? 3 : 2;
  const bucketCount = Math.floor((maxPoints - 2) / extremaPerBucket);
  for (let bucket = 0; bucket < bucketCount; ++bucket) {
    const begin = 1 + Math.floor(bucket * interiorCount / bucketCount);
    const end = 1 + Math.floor((bucket + 1) * interiorCount / bucketCount);
    let minIndex = begin;
    let maxIndex = begin;
    let additionalMinIndex = begin;
    for (let i = begin + 1; i < end; ++i) {
      if (points[i].v < points[minIndex].v) minIndex = i;
      if (points[i].v > points[maxIndex].v) maxIndex = i;
      if (additionalMinimum &&
          additionalMinimum(points[i]) < additionalMinimum(points[additionalMinIndex])) {
        additionalMinIndex = i;
      }
    }
    const indices = additionalMinimum
      ? [minIndex, maxIndex, additionalMinIndex]
      : [minIndex, maxIndex];
    indices.sort((a, b) => a - b);
    for (let i = 0; i < indices.length; ++i) {
      if (i === 0 || indices[i] !== indices[i - 1])
        result.push(points[indices[i]]);
    }
  }
  result.push(points[points.length - 1]);
  return result;
}

function hasFiniteDrawdowns(curve: [number, number][]): boolean {
  let peak = -Infinity;
  for (const [, value] of curve) {
    peak = Math.max(peak, value);
    if (!Number.isFinite(value / peak - 1)) return false;
  }
  return true;
}

function isFiniteAscendingCurve(curve: unknown): curve is [number, number][] {
  if (!Array.isArray(curve) || curve.length === 0) return false;
  let previousTs = -Infinity;
  for (let i = 0; i < curve.length; ++i) {
    const point = curve[i];
    if (!Array.isArray(point) || point.length !== 2) return false;
    const [tsMs, value] = point;
    if (!Number.isSafeInteger(tsMs) || tsMs <= 0 || !Number.isFinite(value) ||
        (i === 0 ? value <= 0 : tsMs <= previousTs)) {
      return false;
    }
    previousTs = tsMs;
  }
  return true;
}

function histogram(trades: readonly WireTrade[]): HistBin[] {
  const sideBins = 7;
  const extent = 2500;
  const w = extent / sideBins;
  const bins: HistBin[] = [];
  for (let i = 0; i < sideBins; ++i)
    bins.push({ x0: -extent + i * w, x1: -extent + (i + 1) * w, c: 0 });
  bins.push({ x0: 0, x1: 0, c: 0 });
  for (let i = 0; i < sideBins; ++i)
    bins.push({ x0: i * w, x1: (i + 1) * w, c: 0 });
  for (const trade of trades) {
    const p = trade.pnl;
    let idx = sideBins;
    if (p < 0)
      idx = Math.max(0, Math.min(sideBins - 1,
        Math.floor((p + extent) / w)));
    else if (p > 0)
      idx = sideBins + 1 + Math.max(0, Math.min(sideBins - 1,
        Math.floor(p / w)));
    bins[idx].c++;
  }
  return bins;
}

export function adaptReport(input: unknown): ReportData {
  assertResultsReport(input);
  const r = input;
  const lossCount = r.total_trades - r.winning_trades;
  const avgTrade = r.total_trades
    ? r.avg_win * (r.winning_trades / r.total_trades)
      - Math.abs(r.avg_loss) * (lossCount / r.total_trades)
    : 0;
  if (!Number.isFinite(avgTrade)) throw new TypeError("invalid_results_report");

  const strategyCurveValid = isFiniteAscendingCurve(r.equity_curve) &&
    hasFiniteDrawdowns(r.equity_curve);
  const strategySamplingMetadataValid =
    Number.isSafeInteger(r.equity_curve_sample_stride) &&
    (r.equity_curve_sample_stride as number) >= 1 &&
    r.return_observation_basis === "market_marks_excluding_cash_settlements";
  const portfolioTimeSeriesValid = strategyCurveValid &&
    strategySamplingMetadataValid &&
    r.portfolio_time_series_valid === true;
  const portfolioTimeSeriesReason = !strategyCurveValid
    ? "invalid_strategy_equity_curve"
    : (!strategySamplingMetadataValid
      ? "missing_strategy_curve_sampling_metadata"
      : (r.portfolio_time_series_reason ?? ""));
  const annualizedValid = portfolioTimeSeriesValid &&
    r.annualized_return_valid === true && Number.isFinite(r.annualized_return);
  const sharpeValid = portfolioTimeSeriesValid &&
    r.sharpe_ratio_valid === true && Number.isFinite(r.sharpe_ratio);
  const sortinoValid = portfolioTimeSeriesValid &&
    r.sortino_ratio_valid === true && Number.isFinite(r.sortino_ratio);
  const calmarValid = portfolioTimeSeriesValid &&
    r.calmar_ratio_valid === true && Number.isFinite(r.calmar_ratio);
  const maxDrawdownValid = portfolioTimeSeriesValid &&
    Number.isFinite(r.max_drawdown) && r.max_drawdown >= 0;
  const benchmarkMetricsValid = [r.alpha, r.beta, r.information_ratio,
    r.tracking_error, r.strategy_vs_benchmark,
    r.buy_and_hold_return].every(isFiniteNumber) &&
    r.tracking_error >= 0 &&
    (r.tracking_error !== 0 || r.information_ratio === 0);
  const benchmarkPathValid = portfolioTimeSeriesValid &&
    r.benchmark_valid === true &&
    typeof r.benchmark_symbol === "string" &&
    r.benchmark_symbol.trim().length > 0 &&
    isFiniteAscendingCurve(r.benchmark_equity_curve);
  const benchmarkSamplingMetadataValid =
    Number.isSafeInteger(r.benchmark_equity_curve_sample_stride) &&
    (r.benchmark_equity_curve_sample_stride as number) >= 1 &&
    r.benchmark_curve_observation_basis === "selected_symbol_market_marks";
  const benchmarkCurveValid = benchmarkPathValid && benchmarkMetricsValid &&
    benchmarkSamplingMetadataValid;
  const backtest: Backtest = {
    name: "Backtest run",
    range: "—",
    bars: "—",
    universe: Object.keys(r.per_symbol).join(" · ") || "—",
    headline: {
      valuationComplete: r.valuation_complete === true && r.accounting_reconciled,
      valuationReason: r.accounting_reconciled
        ? (r.valuation_reason ?? "")
        : r.accounting_reconciliation_reason,
      portfolioTimeSeriesValid,
      portfolioTimeSeriesReason,
      totalReturn: r.cumulative_return,
      sharpe: sharpeValid ? r.sharpe_ratio : null,
      sharpeReason: sharpeValid ? (r.sharpe_ratio_reason ?? "")
        : (portfolioTimeSeriesValid
          ? (r.sharpe_ratio_reason || "invalid_sharpe_ratio")
          : portfolioTimeSeriesReason),
      sortino: sortinoValid ? r.sortino_ratio : null,
      sortinoReason: sortinoValid ? (r.sortino_ratio_reason ?? "")
        : (portfolioTimeSeriesValid
          ? (r.sortino_ratio_reason || "invalid_sortino_ratio")
          : portfolioTimeSeriesReason),
      calmar: calmarValid ? r.calmar_ratio : null,
      calmarReason: calmarValid ? (r.calmar_ratio_reason ?? "")
        : (portfolioTimeSeriesValid
          ? (r.calmar_ratio_reason || "invalid_calmar_ratio")
          : portfolioTimeSeriesReason),
      maxDD: maxDrawdownValid ? -Math.abs(r.max_drawdown) / 100 : null,
      maxDDReason: maxDrawdownValid ? ""
        : (portfolioTimeSeriesValid
          ? "invalid_max_drawdown"
          : portfolioTimeSeriesReason),
      winRate: pctToFrac(r.win_rate),
      profitFactor: r.profit_factor_valid ? r.profit_factor : null,
      profitFactorUnbounded: r.profit_factor_unbounded,
      profitFactorReason: r.profit_factor_reason,
      totalTrades: r.total_trades,
      finalEquity: r.final_equity,
      startEquity: r.initial_equity,
      cagr: annualizedValid ? r.annualized_return : null,
      cagrReason: annualizedValid ? (r.annualized_return_reason ?? "")
        : (portfolioTimeSeriesValid
          ? (r.annualized_return_reason || "invalid_annualized_return")
          : portfolioTimeSeriesReason),
      avgTrade,
    },
    bench: {
      valid: benchmarkCurveValid,
      reason: benchmarkCurveValid
        ? (r.benchmark_reason ?? "")
        : (benchmarkPathValid && !benchmarkMetricsValid
          ? "invalid_benchmark_metrics"
          : (benchmarkPathValid && !benchmarkSamplingMetadataValid
            ? "missing_benchmark_sampling_metadata"
          : (r.benchmark_valid === true
            ? "invalid_benchmark_curve"
            : (r.benchmark_reason ?? "")))),
      symbol: benchmarkCurveValid ? (r.benchmark_symbol ?? "") : "",
      alpha: isFiniteNumber(r.alpha) ? r.alpha : 0,
      beta: isFiniteNumber(r.beta) ? r.beta : 0,
      infoRatio: isFiniteNumber(r.information_ratio) ? r.information_ratio : 0,
      vsBH: isFiniteNumber(r.strategy_vs_benchmark) ? r.strategy_vs_benchmark : 0,
      bhReturn: isFiniteNumber(r.buy_and_hold_return) ? r.buy_and_hold_return : 0,
    },
  };

  const stratFull = portfolioTimeSeriesValid ? stratSeries(r.equity_curve) : [];
  const benchFull: BTPoint[] = benchmarkCurveValid
    ? r.benchmark_equity_curve.map(([tsMs, v], i) => ({ i, tsMs, v }))
    : [];
  const strat = decimateCurve(
    stratFull, REPORT_LIMITS.visualCurvePoints, (point) => point.dd ?? 0);
  const bench = decimateCurve(benchFull, REPORT_LIMITS.visualCurvePoints);
  const btCurve = { strat, bench };
  const projection = (rendered: number, source: number) => rendered === source
    ? `${source}/${source} source points`
    : `${rendered}/${source} points, client temporal-bucket extrema projection`;
  const curveDisclosure = [
    `strategy ${projection(strat.length, r.equity_curve.length)}`,
    `source stride ${r.equity_curve_sample_stride ?? "missing"}, market marks excluding cash settlements`,
    benchmarkCurveValid
      ? `benchmark ${projection(bench.length, r.benchmark_equity_curve.length)}`
      : "benchmark unavailable",
    benchmarkCurveValid
      ? `benchmark source stride ${r.benchmark_equity_curve_sample_stride}, selected-symbol market marks`
      : "",
  ].filter(Boolean).join(" · ");

  // Execution side alone cannot prove position effect (open/close) or
  // position side (long/short). Keep lifecycle markers unsupported until the
  // wire contract carries that information explicitly.
  const btMarkers: BTMarker[] = [];

  const blotter: BlotterRow[] = r.trades.map((t) => ({
    id: t.fill_id !== "0"
      ? `fill:${t.order_id}:${t.fill_id}`
      : `venue:${t.symbol.length}:${t.symbol}:${t.venue_execution_id.length}:${t.venue_execution_id}`,
    orderId: t.order_id,
    tsMs: t.ts_ms,
    sym: t.symbol,
    strat: t.strategy_name,
    side: t.side,
    qty: t.quantity,
    fill: t.fill_price,
    intended: t.intended_price,
    intendedValid: t.intended_price > 0,
    slipBps: t.intended_price ? (t.fill_price / t.intended_price - 1) * 1e4 : 0,
    slippageValid: t.intended_price > 0,
    comm: t.commission,
    commCurrency: t.commission_currency,
    pnl: t.pnl,
  }));

  const hist = histogram(r.trades);

  return {
    backtest,
    btCurve,
    btMarkers,
    curveDisclosure,
    blotter,
    hist,
    bySymbol: breakdown(r.per_symbol),
    byStrategy: breakdown(r.per_strategy),
  };
}

export function adaptReportJson(
  text: string,
  maxBytes = REPORT_LIMITS.reportBytes,
): ReportData {
  if (!Number.isSafeInteger(maxBytes) || maxBytes < 0 ||
      text.length > maxBytes ||
      new TextEncoder().encode(text).byteLength > maxBytes) {
    throw new RangeError("results_report_too_large");
  }
  return adaptReport(JSON.parse(text) as unknown);
}
