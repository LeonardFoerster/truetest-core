#!/usr/bin/env python3
"""
TrueTest plot.py — simple graphical analysis for backtest and Monte Carlo results.

Consumes the existing --output JSON (or the _equity.csv + _trades.csv pair)
produced by engine_backtest / engine_shadow.

Usage examples:
    # Single backtest (recommended)
    ./build/engine_backtest --provider local --path data/market_data.csv --strategy sma \
        --output results.json
    python scripts/plot.py results.json --show --outdir plots/

    # Using the CSV exports
    python scripts/plot.py results_equity.csv --title "SMA Backtest"

    # Monte Carlo campaign
    ./build/engine_backtest --monte-carlo --mc-trials 300 --strategy sma --output mc_campaign.json
    python scripts/plot.py mc_campaign.json --mc --outdir plots/ --title "SMA MC"

    # (The old "JSON Summary:" stdout block is still printed for convenience / copy-paste,
    #  but --output is the recommended way and produces a file directly consumable by this script.)

Requirements:
    pip install matplotlib numpy

The script is intentionally dependency-light and standalone. It works with
the data already exported by the engine (AnalyticsReport + McAggregate shapes).
"""

import argparse
import csv
import json
import sys
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

import numpy as np

try:
    import matplotlib.pyplot as plt
    from matplotlib.dates import DateFormatter
except ImportError:
    print("ERROR: matplotlib is required. Install with: pip install matplotlib numpy")
    sys.exit(2)

try:
    import matplotlib.dates as mdates
except Exception:
    mdates = None  # type: ignore


# ----------------------------- Data loading -----------------------------

@dataclass
class SingleRunData:
    title: str
    equity_ts: np.ndarray          # datetime64[ns] or float seconds
    equity: np.ndarray
    trades_ts: Optional[np.ndarray]
    trade_pnl: np.ndarray
    metrics: Dict[str, Any]


@dataclass
class MonteCarloData:
    title: str
    n_trials: int
    mean_pnl: float
    median_pnl: float
    p5_pnl: float
    p95_pnl: float
    mean_sharpe: float
    median_sharpe: float
    mean_max_dd: float
    worst_max_dd: float
    profitable_trials: int
    trials_pf_gt_1: int
    # Per-trial arrays (may be empty if only aggregate JSON was provided)
    trial_pnl: np.ndarray
    trial_sharpe: np.ndarray
    trial_max_dd: np.ndarray
    trial_win_rate: np.ndarray


def _parse_timestamp_ms(ms: int) -> datetime:
    return datetime.fromtimestamp(ms / 1000.0)


def load_single_from_json(path: Path) -> SingleRunData:
    data = json.loads(path.read_text())

    # equity_curve is [[ms, equity], ...] in the current export
    eq_curve = data.get("equity_curve", [])
    if not eq_curve:
        # Some older or minimal reports might use different key
        eq_curve = data.get("equity", [])

    if not eq_curve:
        raise ValueError(f"No equity_curve found in {path}")

    ts = np.array([_parse_timestamp_ms(int(p[0])) for p in eq_curve])
    equity = np.array([float(p[1]) for p in eq_curve], dtype=float)

    # trades
    trades = data.get("trades", [])
    if trades:
        t_ts = np.array([_parse_timestamp_ms(int(t.get("timestamp", t.get("ts", 0)))) for t in trades])
        t_pnl = np.array([float(t.get("pnl", 0.0)) for t in trades], dtype=float)
    else:
        t_ts = None
        t_pnl = np.array([], dtype=float)

    # Collect a few useful scalars for titles / annotations
    metrics = {
        "initial_equity": data.get("initial_equity"),
        "final_equity": data.get("final_equity"),
        "cumulative_return": data.get("cumulative_return"),
        "sharpe_ratio": data.get("sharpe_ratio"),
        "max_drawdown": data.get("max_drawdown"),
        "win_rate": data.get("win_rate"),
        "total_trades": data.get("total_trades"),
        "profit_factor": data.get("profit_factor"),
    }

    title = path.stem
    if data.get("strategy"):
        title = f"{data['strategy']} — {title}"

    return SingleRunData(
        title=title,
        equity_ts=ts,
        equity=equity,
        trades_ts=t_ts,
        trade_pnl=t_pnl,
        metrics=metrics,
    )


def load_single_from_csv_equity(equity_path: Path, trades_path: Optional[Path] = None) -> SingleRunData:
    # equity csv: timestamp_ms,equity
    eq_ts: List[datetime] = []
    eq_vals: List[float] = []
    with equity_path.open() as f:
        reader = csv.DictReader(f)
        for row in reader:
            ms = int(float(row["timestamp_ms"]))
            eq_ts.append(_parse_timestamp_ms(ms))
            eq_vals.append(float(row["equity"]))

    trade_pnl: List[float] = []
    trade_ts: Optional[List[datetime]] = None
    if trades_path and trades_path.exists():
        trade_ts = []
        with trades_path.open() as f:
            reader = csv.DictReader(f)
            for row in reader:
                ms = int(float(row.get("timestamp_ms", 0)))
                trade_ts.append(_parse_timestamp_ms(ms))
                trade_pnl.append(float(row.get("pnl", 0.0)))
        trade_ts_arr = np.array(trade_ts)
        trade_pnl_arr = np.array(trade_pnl, dtype=float)
    else:
        trade_ts_arr = None
        trade_pnl_arr = np.array(trade_pnl, dtype=float)

    eq_ts_arr = np.array(eq_ts)
    eq_vals_arr = np.array(eq_vals, dtype=float)

    return SingleRunData(
        title=equity_path.stem.replace("_equity", ""),
        equity_ts=eq_ts_arr,
        equity=eq_vals_arr,
        trades_ts=trade_ts_arr,
        trade_pnl=trade_pnl_arr,
        metrics={},
    )


def load_mc_from_json(path: Path) -> MonteCarloData:
    data = json.loads(path.read_text())

    n = int(data.get("n_trials", data.get("trials", 0) and len(data.get("trials", [])) or 0))

    # Current minimal reporter JSON has flat fields
    mean_pnl = float(data.get("mean_pnl", 0.0))
    median_pnl = float(data.get("median_pnl", 0.0))
    p5 = float(data.get("p5_pnl", data.get("p5", 0.0)))
    p95 = float(data.get("p95_pnl", data.get("p95", 0.0)))
    mean_sharpe = float(data.get("mean_sharpe", 0.0))
    median_sharpe = float(data.get("median_sharpe", 0.0))
    mean_dd = float(data.get("mean_max_dd", 0.0))
    worst_dd = float(data.get("worst_max_dd", 0.0))

    profitable = int(data.get("profitable_trials", data.get("trials_with_positive_pnl", 0)))
    pf_gt_1 = int(data.get("trials_with_pf_gt_1", data.get("trials_with_profit_factor_gt_1", 0)))

    # Richer format (future or when keep_full_reports + better reporter is used)
    trials = data.get("trials", [])
    if trials:
        pnl = np.array([float(t.get("total_pnl", t.get("pnl", 0.0))) for t in trials])
        sharpe = np.array([float(t.get("sharpe_ratio", t.get("sharpe", 0.0))) for t in trials])
        max_dd = np.array([float(t.get("max_drawdown", t.get("max_dd", 0.0))) for t in trials])
        win_rate = np.array([float(t.get("win_rate", 0.0)) for t in trials])
    else:
        # No per-trial data available in this JSON
        pnl = np.array([])
        sharpe = np.array([])
        max_dd = np.array([])
        win_rate = np.array([])

    title = data.get("title") or path.stem

    return MonteCarloData(
        title=title,
        n_trials=n or len(pnl) or 0,
        mean_pnl=mean_pnl,
        median_pnl=median_pnl,
        p5_pnl=p5,
        p95_pnl=p95,
        mean_sharpe=mean_sharpe,
        median_sharpe=median_sharpe,
        mean_max_dd=mean_dd,
        worst_max_dd=worst_dd,
        profitable_trials=profitable,
        trials_pf_gt_1=pf_gt_1,
        trial_pnl=pnl,
        trial_sharpe=sharpe,
        trial_max_dd=max_dd,
        trial_win_rate=win_rate,
    )


def auto_load(path: Path) -> Tuple[str, Any]:
    """
    Returns ("single", SingleRunData) or ("mc", MonteCarloData)
    Tries to be smart about the input type.
    """
    suffix = path.suffix.lower()

    if suffix == ".json":
        text = path.read_text()
        try:
            data = json.loads(text)
        except Exception:
            data = {}

        # Heuristics for MC vs single run
        if "n_trials" in data or "trials" in data or "mean_pnl" in data:
            return "mc", load_mc_from_json(path)

        if "equity_curve" in data or "equity" in data or "cumulative_return" in data:
            return "single", load_single_from_json(path)

        # Fallback: treat as single
        return "single", load_single_from_json(path)

    if suffix in (".csv", ""):
        # Be lenient with extension (the engine's --output csv sometimes writes
        # a file with no extension for the equity side).
        # Detect by header content.
        try:
            with path.open() as f:
                header = f.readline().strip().lower()
        except Exception:
            header = ""

        is_equity_csv = "equity" in header and "timestamp" in header
        is_trades_csv = "pnl" in header and ("symbol" in header or "side" in header)

        equity_file = path
        trades_candidate: Optional[Path] = None

        stem = path.stem
        parent = path.parent

        if is_equity_csv or (not is_trades_csv and header):
            # Treat the given file as (or find) the equity file.
            candidates = [
                parent / f"{stem}_trades.csv",
                parent / f"{stem.replace('_equity', '')}_trades.csv",
                parent / f"{stem}_trades",
                parent / (stem + "_trades"),
                parent / f"{stem}_trades.csv",
            ]
            for c in candidates:
                if c.exists():
                    trades_candidate = c
                    break
        else:
            # Looks more like a trades file; hunt for equity sibling.
            equity_candidates = [
                parent / f"{stem.replace('_trades', '')}_equity.csv",
                parent / f"{stem.replace('_trades', '')}.csv",
                parent / stem.replace("_trades", ""),
                parent / f"{stem.replace('_trades', '')}",
            ]
            for c in equity_candidates:
                if c.exists() and c != path:
                    equity_file = c
                    break
            trades_candidate = path

        # Final fallback: if we still don't have strong signals, just try loading
        # the passed path as equity (many people will point at the equity file).
        if not header or is_equity_csv or not is_trades_csv:
            equity_file = equity_file or path

        return "single", load_single_from_csv_equity(equity_file, trades_candidate)

    raise ValueError(f"Unsupported input type: {path}. Use .json or .csv (extensionless base names from --output csv are also supported)")


# ----------------------------- Plotting -----------------------------

def plot_single(run: SingleRunData, outdir: Optional[Path], show: bool, fmt: str = "png") -> List[Path]:
    saved: List[Path] = []
    outdir = outdir or Path(".")
    outdir.mkdir(parents=True, exist_ok=True)

    # --- Figure 1: Equity curve + drawdown ---
    fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(11, 7), gridspec_kw={"height_ratios": [2, 1]}, sharex=True)

    # Equity
    ax1.plot(run.equity_ts, run.equity, color="#1f77b4", linewidth=1.5, label="Equity")
    ax1.set_ylabel("Equity")
    ax1.set_title(f"Equity Curve — {run.title}")
    ax1.grid(True, alpha=0.3)
    ax1.legend(loc="upper left")

    # Annotate start / end
    if len(run.equity) > 1:
        ax1.annotate(f"Start: {run.equity[0]:.2f}",
                     xy=(run.equity_ts[0], run.equity[0]),
                     xytext=(10, 10), textcoords="offset points",
                     fontsize=8, alpha=0.7)
        ax1.annotate(f"End: {run.equity[-1]:.2f}",
                     xy=(run.equity_ts[-1], run.equity[-1]),
                     xytext=(10, -15), textcoords="offset points",
                     fontsize=8, alpha=0.7)

    # Drawdown
    peak = np.maximum.accumulate(run.equity)
    dd = (run.equity - peak) / np.maximum(peak, 1e-9) * 100.0
    ax2.fill_between(run.equity_ts, dd, 0, color="#d62728", alpha=0.35, label="Drawdown %")
    ax2.plot(run.equity_ts, dd, color="#d62728", linewidth=0.8)
    ax2.set_ylabel("Drawdown %")
    ax2.set_xlabel("Time")
    ax2.grid(True, alpha=0.3)
    ax2.axhline(0, color="black", linewidth=0.5)
    ax2.legend(loc="lower left")

    # Format time axis nicely if possible
    if mdates is not None:
        ax2.xaxis.set_major_formatter(mdates.DateFormatter("%m-%d\n%H:%M"))

    fig.tight_layout()
    p = outdir / f"{run.title.replace(' ', '_')}_equity_dd.{fmt}"
    fig.savefig(p, dpi=150, bbox_inches="tight")
    saved.append(p)
    if show:
        plt.show()
    plt.close(fig)

    # --- Figure 2: Trade PnL distribution ---
    if len(run.trade_pnl) > 0:
        fig, ax = plt.subplots(figsize=(9, 5))
        wins = run.trade_pnl[run.trade_pnl > 0]
        losses = run.trade_pnl[run.trade_pnl < 0]

        bins = 40
        ax.hist(run.trade_pnl, bins=bins, color="#2ca02c", alpha=0.7, edgecolor="white", linewidth=0.3)
        ax.axvline(0, color="black", linewidth=1)
        ax.axvline(np.mean(run.trade_pnl), color="red", linestyle="--", linewidth=1.5,
                   label=f"Mean {np.mean(run.trade_pnl):.2f}")
        ax.axvline(np.median(run.trade_pnl), color="orange", linestyle="-.", linewidth=1.5,
                   label=f"Median {np.median(run.trade_pnl):.2f}")

        ax.set_xlabel("Trade PnL")
        ax.set_ylabel("Count")
        ax.set_title(f"Per-Trade PnL Distribution — {run.title} (n={len(run.trade_pnl)})")
        ax.legend()
        ax.grid(True, alpha=0.3, axis="y")

        fig.tight_layout()
        p = outdir / f"{run.title.replace(' ', '_')}_trades_pnl.{fmt}"
        fig.savefig(p, dpi=150, bbox_inches="tight")
        saved.append(p)
        if show:
            plt.show()
        plt.close(fig)

    # Print a compact text summary
    print(f"\n=== {run.title} ===")
    for k, v in run.metrics.items():
        if v is not None:
            if isinstance(v, float):
                print(f"  {k:20s}: {v:10.4f}")
            else:
                print(f"  {k:20s}: {v}")
    if len(run.trade_pnl) > 0:
        print(f"  {'win_rate (from trades)':20s}: {100.0 * (run.trade_pnl > 0).sum() / len(run.trade_pnl):.2f}%")
        print(f"  {'avg win':20s}: {np.mean(wins) if len(wins) else 0:.2f}")
        print(f"  {'avg loss':20s}: {np.mean(losses) if len(losses) else 0:.2f}")

    print(f"\nSaved {len(saved)} figure(s) to {outdir.resolve()}")
    return saved


def plot_mc(mc: MonteCarloData, outdir: Optional[Path], show: bool, fmt: str = "png") -> List[Path]:
    saved: List[Path] = []
    outdir = outdir or Path(".")
    outdir.mkdir(parents=True, exist_ok=True)

    has_trials = len(mc.trial_pnl) > 0

    # --- Summary text always ---
    print(f"\n=== Monte Carlo: {mc.title} ({mc.n_trials} trials) ===")
    print(f"  PnL mean/median/p5/p95 : {mc.mean_pnl:.2f} / {mc.median_pnl:.2f} / {mc.p5_pnl:.2f} / {mc.p95_pnl:.2f}")
    print(f"  Sharpe mean/median     : {mc.mean_sharpe:.3f} / {mc.median_sharpe:.3f}")
    print(f"  Max DD mean / worst    : {mc.mean_max_dd:.2f} / {mc.worst_max_dd:.2f}")
    print(f"  Profitable trials      : {mc.profitable_trials} / {mc.n_trials} "
          f"({100*mc.profitable_trials/mc.n_trials:.1f}%)")
    print(f"  PF > 1 trials          : {mc.trials_pf_gt_1} / {mc.n_trials}")

    if not has_trials:
        print("\n(Note: per-trial arrays not present in input JSON — only aggregate stats shown.")
        print("       Run with richer MC output or --keep-full-reports for distribution plots.)")
        return saved

    # --- Figure 1: PnL distribution ---
    fig, ax = plt.subplots(figsize=(10, 5))
    ax.hist(mc.trial_pnl, bins=30, color="#1f77b4", alpha=0.75, edgecolor="white")
    ax.axvline(mc.mean_pnl, color="red", linestyle="--", linewidth=2, label=f"Mean {mc.mean_pnl:.2f}")
    ax.axvline(mc.median_pnl, color="orange", linestyle="-.", linewidth=2, label=f"Median {mc.median_pnl:.2f}")
    ax.axvline(mc.p5_pnl, color="gray", linestyle=":", linewidth=1.5, label=f"5% {mc.p5_pnl:.2f}")
    ax.axvline(mc.p95_pnl, color="gray", linestyle=":", linewidth=1.5, label=f"95% {mc.p95_pnl:.2f}")
    ax.axvline(0, color="black", linewidth=1)
    ax.set_xlabel("Trial Total PnL")
    ax.set_ylabel("Number of trials")
    ax.set_title(f"Monte Carlo PnL Distribution — {mc.title}")
    ax.legend()
    ax.grid(True, alpha=0.3, axis="y")
    fig.tight_layout()
    p = outdir / f"{mc.title.replace(' ', '_')}_mc_pnl.{fmt}"
    fig.savefig(p, dpi=150, bbox_inches="tight")
    saved.append(p)
    if show:
        plt.show()
    plt.close(fig)

    # --- Figure 2: Risk metrics (Sharpe + Max DD) ---
    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(12, 4.5))

    ax1.hist(mc.trial_sharpe, bins=25, color="#2ca02c", alpha=0.75, edgecolor="white")
    ax1.axvline(mc.mean_sharpe, color="red", linestyle="--", linewidth=2, label=f"Mean {mc.mean_sharpe:.3f}")
    ax1.axvline(mc.median_sharpe, color="orange", linestyle="-.", linewidth=2)
    ax1.set_xlabel("Sharpe Ratio")
    ax1.set_title("Sharpe Distribution")
    ax1.legend()
    ax1.grid(True, alpha=0.3, axis="y")

    ax2.hist(mc.trial_max_dd, bins=25, color="#d62728", alpha=0.7, edgecolor="white")
    ax2.axvline(mc.mean_max_dd, color="red", linestyle="--", linewidth=2, label=f"Mean {mc.mean_max_dd:.2f}")
    ax2.axvline(mc.worst_max_dd, color="black", linestyle=":", linewidth=1.8, label=f"Worst {mc.worst_max_dd:.2f}")
    ax2.set_xlabel("Max Drawdown")
    ax2.set_title("Max Drawdown Distribution")
    ax2.legend()
    ax2.grid(True, alpha=0.3, axis="y")

    fig.suptitle(f"Monte Carlo Risk Metrics — {mc.title}", y=1.02)
    fig.tight_layout()
    p = outdir / f"{mc.title.replace(' ', '_')}_mc_risk.{fmt}"
    fig.savefig(p, dpi=150, bbox_inches="tight")
    saved.append(p)
    if show:
        plt.show()
    plt.close(fig)

    # --- Figure 3: PnL vs Max DD scatter (risk/return) ---
    if len(mc.trial_pnl) > 5:
        fig, ax = plt.subplots(figsize=(8, 6))
        sc = ax.scatter(mc.trial_max_dd, mc.trial_pnl, c=mc.trial_sharpe,
                        cmap="RdYlGn", alpha=0.7, edgecolors="none", s=28)
        ax.axhline(0, color="black", linewidth=0.8)
        ax.axvline(mc.mean_max_dd, color="gray", linestyle="--", alpha=0.6)
        ax.set_xlabel("Max Drawdown")
        ax.set_ylabel("Total PnL")
        ax.set_title(f"PnL vs. Max DD (colored by Sharpe) — {mc.title}")
        plt.colorbar(sc, ax=ax, label="Sharpe")
        ax.grid(True, alpha=0.3)
        fig.tight_layout()
        p = outdir / f"{mc.title.replace(' ', '_')}_mc_scatter.{fmt}"
        fig.savefig(p, dpi=150, bbox_inches="tight")
        saved.append(p)
        if show:
            plt.show()
        plt.close(fig)

    print(f"\nSaved {len(saved)} figure(s) to {outdir.resolve()}")
    return saved


# ----------------------------- CLI -----------------------------

def main():
    parser = argparse.ArgumentParser(
        description="Plot TrueTest backtest / Monte Carlo results from exported JSON or CSV."
    )
    parser.add_argument("input", type=Path, help="Path to results.json, campaign.json, or *_equity.csv")
    parser.add_argument("--outdir", "-o", type=Path, default=None,
                        help="Directory to save figures (default: current dir)")
    parser.add_argument("--show", "-s", action="store_true",
                        help="Call plt.show() after saving (interactive)")
    parser.add_argument("--format", default="png", choices=["png", "pdf", "svg"],
                        help="Output image format")
    parser.add_argument("--title", default=None, help="Override plot title")
    parser.add_argument("--mc", action="store_true",
                        help="Force Monte Carlo mode (auto-detected otherwise)")

    args = parser.parse_args()

    inp = args.input
    if not inp.exists():
        print(f"ERROR: Input not found: {inp}")
        sys.exit(1)

    kind, payload = auto_load(inp)

    if args.mc:
        kind = "mc"

    if args.title:
        if kind == "single":
            payload.title = args.title
        else:
            payload.title = args.title

    if kind == "single":
        plot_single(payload, args.outdir, args.show, args.format)
    else:
        plot_mc(payload, args.outdir, args.show, args.format)


if __name__ == "__main__":
    main()
