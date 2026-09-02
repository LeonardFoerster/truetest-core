import assert from "node:assert/strict";
import { readFileSync } from "node:fs";

import { adaptReport, adaptReportJson, decimateCurve, REPORT_LIMITS } from "../src/adapters/report.ts";
import {
  histogramCountScale,
  normalizedPosition,
  normalizedRange,
} from "../src/chartMath.ts";
import { fmt } from "../src/format.ts";
import { readBoundedResponseText } from "../src/data/readBoundedResponse.ts";

const fixture = JSON.parse(
  readFileSync(new URL("../src/fixtures/report.json", import.meta.url), "utf8"),
);

function cloneFixture() {
  const report = structuredClone(fixture);
  report.trades = report.trades.map((trade, i) => ({
    ...trade,
    order_id: String(trade.order_id),
    fill_id: String(i + 1),
    venue_execution_id: "",
    commission_currency: trade.commission === 0 ? "" : "USD",
  }));
  report.gross_realized_pnl = report.final_equity - report.initial_equity;
  report.realized_pnl = report.gross_realized_pnl;
  report.funding_pnl = 0;
  report.unrealized_pnl = 0;
  report.total_commission = 0;
  report.reconciliation_residual = 0;
  report.accounting_reconciled = true;
  report.accounting_reconciliation_reason =
    "reconciled_within_floating_tolerance";
  report.total_win = 200;
  report.total_loss = 100;
  report.profit_factor = report.total_win / report.total_loss;
  report.profit_factor_valid = true;
  report.profit_factor_unbounded = false;
  report.profit_factor_reason = "computed_from_gross_win_and_loss";
  for (const counter of ["duplicate_fill_replays_ignored",
    "conflicting_fill_replays_rejected", "missing_fill_identities_rejected",
    "invalid_fill_payloads_rejected", "unreconciled_funding_events_rejected",
    "duplicate_funding_replays_ignored",
    "conflicting_funding_replays_rejected", "late_fill_events_rejected",
    "late_funding_events_rejected"]) report[counter] = 0;
  for (const breakdown of [report.per_symbol, report.per_strategy]) {
    for (const entry of Object.values(breakdown)) {
      entry.total_win = entry.total_pnl >= 0 ? 200 : 50;
      entry.total_loss = 100;
      entry.total_pnl = entry.total_win - entry.total_loss;
      entry.profit_factor = entry.total_win / entry.total_loss;
      entry.profit_factor_valid = entry.total_loss > 0;
      entry.profit_factor_unbounded =
        entry.total_loss === 0 && entry.total_win > 0;
      entry.profit_factor_reason = entry.profit_factor_valid
        ? "computed_from_gross_win_and_loss"
        : entry.profit_factor_unbounded
          ? "no_losses_unbounded" : "no_winning_or_losing_trades";
    }
  }
  report.portfolio_time_series_valid = true;
  report.return_observation_basis = "market_marks_excluding_cash_settlements";
  report.equity_curve_sample_stride = 1;
  report.annualized_return_valid = true;
  report.sharpe_ratio_valid = true;
  report.sortino_ratio_valid = true;
  report.calmar_ratio_valid = true;
  report.benchmark_valid = true;
  report.benchmark_symbol = "BTCUSDT";
  report.benchmark_equity_curve_sample_stride = 1;
  report.benchmark_curve_observation_basis = "selected_symbol_market_marks";
  report.trade_rows_kind = "physical_fill_legs";
  report.trade_returns_kind = "closing_fill_legs";
  report.win_rate = report.winning_trades / report.total_trades * 100;
  return report;
}

assert.throws(() => adaptReportJson('"é"', 3), /results_report_too_large/);
assert.ok(adaptReportJson(JSON.stringify(cloneFixture())).backtest);

{
  const report = cloneFixture();
  report.reconciliation_residual = 1_000_000_000;
  assert.throws(() => adaptReport(report), /invalid_results_report/);
  report.final_equity += report.reconciliation_residual;
  report.cumulative_return = report.final_equity / report.initial_equity - 1;
  report.accounting_reconciled = false;
  report.accounting_reconciliation_reason =
    "accounting_identity_outside_floating_tolerance";
  const adapted = adaptReport(report);
  assert.equal(adapted.backtest.headline.valuationComplete, false);
  assert.equal(adapted.backtest.headline.valuationReason,
    "accounting_identity_outside_floating_tolerance");
}

{
  const report = cloneFixture();
  report.initial_equity = 1e16;
  report.realized_pnl = -1e16;
  report.gross_realized_pnl = -1e16;
  report.funding_pnl = 0;
  report.unrealized_pnl = 0;
  report.final_equity = 1;
  report.cumulative_return = report.final_equity / report.initial_equity - 1;
  report.reconciliation_residual = 1;
  report.accounting_reconciled = true;
  assert.throws(() => adaptReport(report), /invalid_results_report/);
}

for (const field of ["gross_realized_pnl", "realized_pnl", "funding_pnl",
  "unrealized_pnl", "total_commission", "reconciliation_residual"]) {
  const report = cloneFixture();
  report[field] = null;
  assert.throws(() => adaptReport(report), /invalid_results_report/);
}

{
  const report = cloneFixture();
  report.total_commission = -1;
  assert.ok(adaptReport(report).backtest);
}

{
  const report = cloneFixture();
  report.trades[0].commission = -1;
  report.trades[0].commission_currency = "BNB";
  const adapted = adaptReport(report);
  assert.equal(adapted.blotter[0].comm, -1);
  assert.equal(adapted.blotter[0].commCurrency, "BNB");
  report.trades[0].commission_currency = "";
  assert.throws(() => adaptReport(report), /invalid_results_report/);
}

for (const counter of ["unreconciled_funding_events_rejected",
  "conflicting_funding_replays_rejected", "late_funding_events_rejected"]) {
  const report = cloneFixture();
  report[counter] = 1;
  assert.throws(() => adaptReport(report), /invalid_results_report/);
  report.accounting_reconciled = false;
  report.accounting_reconciliation_reason =
    "unreconciled_funding_settlement";
  assert.equal(adaptReport(report).backtest.headline.valuationComplete, false);
}

for (const counter of ["missing_fill_identities_rejected",
  "conflicting_fill_replays_rejected", "invalid_fill_payloads_rejected",
  "late_fill_events_rejected"]) {
  const report = cloneFixture();
  report[counter] = 1;
  assert.throws(() => adaptReport(report), /invalid_results_report/);
  report.accounting_reconciled = false;
  report.accounting_reconciliation_reason = "unreconciled_fill_event";
  assert.equal(adaptReport(report).backtest.headline.valuationComplete, false);
}

for (const counter of ["duplicate_fill_replays_ignored",
  "conflicting_fill_replays_rejected", "missing_fill_identities_rejected",
  "invalid_fill_payloads_rejected", "unreconciled_funding_events_rejected",
  "duplicate_funding_replays_ignored",
  "conflicting_funding_replays_rejected", "late_fill_events_rejected",
  "late_funding_events_rejected"]) {
  const report = cloneFixture();
  delete report[counter];
  assert.throws(() => adaptReport(report), /invalid_results_report/);
}

{
  const report = cloneFixture();
  report.profit_factor = 0;
  report.total_win = 100;
  report.total_loss = 0;
  report.profit_factor_valid = false;
  report.profit_factor_unbounded = true;
  report.profit_factor_reason = "no_losses_unbounded";
  const adapted = adaptReport(report);
  assert.equal(adapted.backtest.headline.profitFactor, null);
  assert.equal(adapted.backtest.headline.profitFactorReason,
    "no_losses_unbounded");
}

{
  const response = new Response(new ReadableStream({
    start(controller) {
      controller.enqueue(new Uint8Array([1, 2, 3]));
      controller.enqueue(new Uint8Array([4, 5, 6]));
      controller.close();
    },
  }));
  await assert.rejects(() => readBoundedResponseText(response, 5),
    /results_report_too_large/);
}

{
  let emitted = 0;
  const response = new Response(new ReadableStream({
    pull(controller) {
      if (emitted++ < 10_000) controller.enqueue(new Uint8Array());
      else {
        controller.enqueue(new TextEncoder().encode("{}"));
        controller.close();
      }
    },
  }));
  assert.equal(await readBoundedResponseText(response, 2), "{}");
}

{
  const response = new Response("valid", { headers: { "content-length": "1" } });
  await assert.rejects(() => readBoundedResponseText(response, 4),
    /results_report_too_large/);
}

{
  let cancelled = false;
  const body = new ReadableStream({
    pull(controller) { controller.enqueue(new Uint8Array([1, 2, 3])); },
    cancel() {
      cancelled = true;
      return new Promise(() => {});
    },
  });
  const outcome = await Promise.race([
    readBoundedResponseText(new Response(body), 2).then(
      () => "accepted", () => "rejected"),
    new Promise((resolve) => setTimeout(() => resolve("timeout"), 100)),
  ]);
  assert.equal(outcome, "rejected");
  assert.equal(cancelled, true);
}

{
  let cancelled = false;
  const body = new ReadableStream({
    start(controller) { controller.enqueue(new Uint8Array([1])); },
    cancel() { cancelled = true; },
  });
  const response = new Response(body, { headers: { "content-length": "99" } });
  await assert.rejects(() => readBoundedResponseText(response, 2),
    /results_report_too_large/);
  await Promise.resolve();
  assert.equal(cancelled, true);
}

{
  const report = cloneFixture();
  report.trade_rows_kind = "round_trips";
  assert.throws(() => adaptReport(report), /invalid_results_report/);
}

{
  const report = cloneFixture();
  report.trade_returns_kind = "physical_fill_legs";
  assert.throws(() => adaptReport(report), /invalid_results_report/);
}

{
  const report = cloneFixture();
  report.trades = [];
  report.total_trades = 0;
  report.winning_trades = 0;
  report.win_rate = 0;
  const adapted = adaptReport(report);
  assert.ok(adapted.hist.length > 0);
  assert.ok(adapted.hist.every((bin) => bin.c === 0));
  const scale = histogramCountScale(adapted.hist);
  assert.equal(scale, 1);
  assert.ok(adapted.hist.every((bin) => Number.isFinite(bin.c / scale)));
  assert.equal(adapted.backtest.headline.avgTrade, 0);
}

{
  const adapted = adaptReport(cloneFixture());
  assert.deepEqual(adapted.btMarkers, []);
  assert.match(adapted.curveDisclosure, /source stride 1/);
  assert.match(adapted.curveDisclosure, /market marks excluding cash settlements/);
  assert.match(adapted.curveDisclosure, /selected-symbol market marks/);
  assert.equal(adapted.blotter[1].side, "sell");
  assert.equal(adapted.blotter[1].tsMs, cloneFixture().trades[1].ts_ms);
  assert.equal(adapted.hist.length, 15);
  assert.ok(adapted.hist.every((bin) => !(bin.x0 < 0 && bin.x1 > 0)));
  const expected = fixture.avg_win * (fixture.winning_trades / fixture.total_trades)
    - Math.abs(fixture.avg_loss) *
      ((fixture.total_trades - fixture.winning_trades) / fixture.total_trades);
  assert.equal(adapted.backtest.headline.avgTrade, expected);
}

{
  const report = cloneFixture();
  report.trades[0].order_id = "0";
  assert.throws(() => adaptReport(report), /invalid_results_report/);
}

{
  const report = cloneFixture();
  report.trades[0].order_id = "18446744073709551615";
  const adapted = adaptReport(report);
  assert.equal(adapted.blotter[0].orderId, "18446744073709551615");
  report.trades[0].order_id = "18446744073709551616";
  assert.throws(() => adaptReport(report), /invalid_results_report/);
}

{
  const report = cloneFixture();
  report.trades[0].ts_ms = 8_640_000_000_000_001;
  assert.throws(() => adaptReport(report), /invalid_results_report/);
}

{
  const report = cloneFixture();
  report.trades = [
    { ...report.trades[0], order_id: "42", fill_id: "1001" },
    { ...report.trades[1], order_id: "42", fill_id: "1002" },
  ];
  const adapted = adaptReport(report);
  assert.equal(adapted.blotter[0].orderId, "42");
  assert.equal(adapted.blotter[1].orderId, "42");
  assert.notEqual(adapted.blotter[0].id, adapted.blotter[1].id);
}

{
  const report = cloneFixture();
  report.trades = [
    { ...report.trades[0], order_id: "41", fill_id: "1001",
      venue_execution_id: "same-native" },
    { ...report.trades[1], order_id: "42", fill_id: "1002",
      symbol: report.trades[0].symbol, venue_execution_id: "same-native" },
  ];
  assert.throws(() => adaptReport(report), /invalid_results_report/);
}

{
  const report = cloneFixture();
  report.trades = [
    { ...report.trades[0], fill_id: "0", symbol: "A:B", venue_execution_id: "C" },
    { ...report.trades[1], fill_id: "0", symbol: "A", venue_execution_id: "B:C" },
  ];
  const adapted = adaptReport(report);
  assert.notEqual(adapted.blotter[0].id, adapted.blotter[1].id);
}

{
  const report = cloneFixture();
  report.trades[0].venue_execution_id = "x".repeat(REPORT_LIMITS.identifierChars + 1);
  assert.throws(() => adaptReport(report), /invalid_results_report/);
}

{
  const report = cloneFixture();
  report.trades[0].venue_execution_id = "native\0execution";
  assert.throws(() => adaptReport(report), /invalid_results_report/);
}

{
  const report = cloneFixture();
  report.per_symbol = {
    ["x".repeat(REPORT_LIMITS.identifierChars + 1)]:
      report.per_symbol.BTCUSDT,
  };
  assert.throws(() => adaptReport(report), /invalid_results_report/);
}

{
  const report = cloneFixture();
  report.trades = [
    { ...report.trades[0], order_id: "42", fill_id: "1001" },
    { ...report.trades[1], order_id: "42", fill_id: "1001" },
  ];
  assert.throws(() => adaptReport(report), /invalid_results_report/);
}

{
  const report = cloneFixture();
  report.trades = [
    { ...report.trades[0], order_id: "41", fill_id: "1001" },
    { ...report.trades[1], order_id: "42", fill_id: "1001" },
  ];
  const adapted = adaptReport(report);
  assert.notEqual(adapted.blotter[0].id, adapted.blotter[1].id);
}

{
  const report = cloneFixture();
  report.win_rate = 100;
  assert.throws(() => adaptReport(report), /invalid_results_report/);
}

{
  const report = cloneFixture();
  report.per_symbol.BTCUSDT.win_rate = 100;
  assert.throws(() => adaptReport(report), /invalid_results_report/);
}

{
  const report = cloneFixture();
  report.trades = [{
    ...report.trades[0],
    fill_id: "0",
    venue_execution_id: "native-execution-1",
  }];
  const adapted = adaptReport(report);
  assert.match(adapted.blotter[0].id, /native-execution-1/);
}

{
  const report = cloneFixture();
  report.trades[0].fill_id = "0";
  report.trades[0].venue_execution_id = "";
  assert.throws(() => adaptReport(report), /invalid_results_report/);
}

{
  const report = cloneFixture();
  report.initial_equity = 100;
  report.final_equity = 100;
  report.cumulative_return = 0;
  report.gross_realized_pnl = 0;
  report.realized_pnl = 0;
  report.funding_pnl = 0;
  report.unrealized_pnl = 0;
  report.reconciliation_residual = 0;
  report.total_trades = 1;
  report.winning_trades = 0;
  report.win_rate = 0;
  report.avg_win = 0;
  report.avg_loss = 0;
  report.trades = [{ ...report.trades[0], pnl: 0 }];
  const adapted = adaptReport(report);
  assert.equal(adapted.backtest.headline.avgTrade, 0);
  assert.equal(adapted.hist[7].c, 1);
}

assert.doesNotMatch(fmt.pct(Number.MAX_VALUE), /Infinity|NaN/);
assert.doesNotMatch(fmt.pctP(-Number.MAX_VALUE), /Infinity|NaN/);

{
  const points = Array.from({ length: 10_000 }, (_, i) => ({
    i,
    tsMs: i + 1,
    v: i === 2_345 ? -1e9 : i === 7_654 ? 1e9 : Math.sin(i),
  }));
  const projected = decimateCurve(points, REPORT_LIMITS.visualCurvePoints);
  assert.ok(projected.length <= REPORT_LIMITS.visualCurvePoints);
  assert.equal(projected[0], points[0]);
  assert.equal(projected.at(-1), points.at(-1));
  assert.ok(projected.includes(points[2_345]));
  assert.ok(projected.includes(points[7_654]));
  assert.ok(projected.every((point, i) => i === 0 ||
    point.tsMs > projected[i - 1].tsMs));
}

{
  const points = Array.from({ length: 2_051 }, (_, i) => ({
    tsMs: i + 1,
    v: i,
    dd: i === 1_000 ? -0.05 : 0,
  }));
  const projected = decimateCurve(
    points, REPORT_LIMITS.visualCurvePoints, (point) => point.dd);
  assert.ok(projected.length <= REPORT_LIMITS.visualCurvePoints);
  assert.ok(projected.includes(points[1_000]));
  assert.equal(Math.min(...projected.map((point) => point.dd)), -0.05);
}

{
  const report = cloneFixture();
  report.benchmark_equity_curve_sample_stride = undefined;
  const adapted = adaptReport(report);
  assert.equal(adapted.backtest.bench.valid, false);
  assert.equal(adapted.backtest.bench.reason,
    "missing_benchmark_sampling_metadata");
}

{
  const report = cloneFixture();
  report.equity_curve_sample_stride = undefined;
  const adapted = adaptReport(report);
  assert.equal(adapted.backtest.headline.portfolioTimeSeriesValid, false);
  assert.equal(adapted.backtest.headline.portfolioTimeSeriesReason,
    "missing_strategy_curve_sampling_metadata");
  assert.deepEqual(adapted.btCurve.strat, []);
}

{
  const report = cloneFixture();
  report.trades = new Array(REPORT_LIMITS.tradeRows + 1).fill(report.trades[0]);
  assert.throws(() => adaptReport(report), /invalid_results_report/);
}

{
  const report = cloneFixture();
  report.tracking_error = null;
  const adapted = adaptReport(report);
  assert.equal(adapted.backtest.bench.valid, false);
  assert.equal(adapted.backtest.bench.reason, "invalid_benchmark_metrics");
}

for (const [trackingError, informationRatio] of [[-1, 7], [0, 7]]) {
  const report = cloneFixture();
  report.tracking_error = trackingError;
  report.information_ratio = informationRatio;
  const adapted = adaptReport(report);
  assert.equal(adapted.backtest.bench.valid, false);
  assert.equal(adapted.backtest.bench.reason, "invalid_benchmark_metrics");
}

{
  const report = cloneFixture();
  report.max_drawdown = -5;
  const adapted = adaptReport(report);
  assert.equal(adapted.backtest.headline.maxDD, null);
  assert.equal(adapted.backtest.headline.maxDDReason, "invalid_max_drawdown");
}

{
  const report = cloneFixture();
  report.trades[0].intended_price = 0;
  const adapted = adaptReport(report);
  assert.equal(adapted.blotter[0].intendedValid, false);
  assert.equal(adapted.blotter[0].slippageValid, false);
}

{
  const report = cloneFixture();
  report.total_trades = 0;
  report.winning_trades = 0;
  assert.throws(() => adaptReport(report), /invalid_results_report/);
}

{
  const report = cloneFixture();
  report.cumulative_return = 0.01;
  assert.throws(() => adaptReport(report), /invalid_results_report/);
}

{
  const report = cloneFixture();
  report.per_symbol = Object.fromEntries(
    Array.from({ length: REPORT_LIMITS.breakdownRows + 1 }, (_, i) =>
      [`S${i}`, report.per_symbol.BTCUSDT]),
  );
  assert.throws(() => adaptReport(report), /invalid_results_report/);
}

for (const invalidSubAnalytics of [
  { total_pnl: 0, trade_count: 1, win_count: 2, win_rate: 50, profit_factor: 1 },
  { total_pnl: 0, trade_count: 1, win_count: 1, win_rate: 100, profit_factor: -1 },
]) {
  const report = cloneFixture();
  report.per_symbol = { BTCUSDT: invalidSubAnalytics };
  assert.throws(() => adaptReport(report), /invalid_results_report/);
}

{
  const report = cloneFixture();
  report.trades[0].symbol = "   ";
  assert.throws(() => adaptReport(report), /invalid_results_report/);
}

for (const invalidInitialEquity of [0, -1]) {
  const report = cloneFixture();
  report.equity_curve[0][1] = invalidInitialEquity;
  const adapted = adaptReport(report);
  assert.equal(adapted.backtest.headline.portfolioTimeSeriesValid, false);
  assert.equal(
    adapted.backtest.headline.portfolioTimeSeriesReason,
    "invalid_strategy_equity_curve",
  );
  assert.equal(adapted.backtest.headline.sharpe, null);
  assert.equal(adapted.backtest.headline.sortino, null);
  assert.equal(adapted.backtest.headline.calmar, null);
  assert.equal(adapted.backtest.headline.cagr, null);
  assert.equal(adapted.backtest.headline.maxDD, null);
  assert.deepEqual(adapted.btCurve.strat, []);
  assert.deepEqual(adapted.btCurve.bench, []);
  assert.deepEqual(adapted.btMarkers, []);
}

for (const malformedCurve of [[], [[1000]], [[1000, 100, 1]], [["1000", 100]]]) {
  const report = cloneFixture();
  report.equity_curve = malformedCurve;
  assert.doesNotThrow(() => adaptReport(report));
  assert.equal(adaptReport(report).backtest.headline.portfolioTimeSeriesValid, false);
}

for (const malformedReport of [null, {}, { ...cloneFixture(), equity_curve: null },
  { ...cloneFixture(), trades: null }, { ...cloneFixture(), profit_factor: null },
  { ...cloneFixture(), cumulative_return: null },
  { ...cloneFixture(), valuation_reason: {} },
  { ...cloneFixture(), benchmark_valid: "true" }]) {
  assert.throws(() => adaptReport(malformedReport), {
    name: "TypeError",
    message: "invalid_results_report",
  });
}

{
  const report = cloneFixture();
  report.trades[0].ts_ms = 0;
  assert.throws(() => adaptReport(report), /invalid_results_report/);
}

{
  const report = cloneFixture();
  report.trades[0].fill_price = Number.MAX_VALUE;
  report.trades[0].intended_price = Number.MIN_VALUE;
  assert.throws(() => adaptReport(report), /invalid_results_report/);
}

{
  const report = cloneFixture();
  report.portfolio_time_series_valid = false;
  report.portfolio_time_series_reason = "ambiguous_mark_order";
  const adapted = adaptReport(report);
  assert.deepEqual(adapted.btCurve.strat, []);
  assert.deepEqual(adapted.btCurve.bench, []);
  assert.deepEqual(adapted.btMarkers, []);
  assert.equal(adapted.backtest.headline.maxDD, null);
  assert.equal(adapted.backtest.headline.sharpe, null);
}

{
  const report = cloneFixture();
  report.beta = null;
  report.information_ratio = null;
  const adapted = adaptReport(report);
  assert.equal(adapted.backtest.bench.valid, false);
  assert.equal(adapted.backtest.bench.reason, "invalid_benchmark_metrics");
  assert.deepEqual(adapted.btCurve.bench, []);
}

{
  const report = cloneFixture();
  report.max_drawdown = Number.NaN;
  report.sharpe_ratio = Number.POSITIVE_INFINITY;
  const adapted = adaptReport(report);
  assert.equal(adapted.backtest.headline.maxDD, null);
  assert.equal(adapted.backtest.headline.maxDDReason, "invalid_max_drawdown");
  assert.equal(adapted.backtest.headline.sharpe, null);
  assert.equal(adapted.backtest.headline.sharpeReason, "invalid_sharpe_ratio");
}

{
  const report = cloneFixture();
  report.equity_curve = [[1000, Number.MAX_VALUE], [2000, -Number.MAX_VALUE]];
  report.benchmark_equity_curve = [[1000, 1], [2000, Number.MAX_VALUE]];
  const adapted = adaptReport(report);
  assert.equal(adapted.backtest.headline.portfolioTimeSeriesValid, true);
  assert.ok(adapted.btCurve.strat.every((point) => Number.isFinite(point.dd)));

  const values = [...adapted.btCurve.strat, ...adapted.btCurve.bench].map((p) => p.v);
  const range = normalizedRange(Math.min(...values), Math.max(...values));
  assert.ok(values.every((value) => Number.isFinite(normalizedPosition(value, range))));
}

{
  const report = cloneFixture();
  report.equity_curve = [[1000, Number.MIN_VALUE], [2000, -Number.MAX_VALUE]];
  const adapted = adaptReport(report);
  assert.equal(adapted.backtest.headline.portfolioTimeSeriesValid, false);
  assert.deepEqual(adapted.btCurve.strat, []);
}

console.log("report frontend contracts: PASS");
