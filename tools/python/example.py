"""Minimal example of using the TrueTest Python bindings.

Run from the repository root after building the shared library:

    cmake -B build -DBUILD_SHARED_LIB=ON
    cmake --build build --target truetest_shared
    python python/example.py
"""

from __future__ import annotations

from pathlib import Path

from truetest import Engine, version


def main() -> None:
    repo_root = Path(__file__).resolve().parent.parent
    csv_path = repo_root / "market_data.csv"

    config = {
        "data_path": str(csv_path),
        "strategy":  "sma",
        "initial_balance": 10_000.0,
        "seed":      1,
        "params":    {"period": 10},
    }

    print(f"TrueTest native library: {version()}")
    print(f"Running backtest on {csv_path} ...")

    with Engine(config) as eng:
        results = eng.run()

    print(f"Sharpe ratio      : {results['sharpe_ratio']:.4f}")
    print(f"Final equity      : {results['final_equity']:.2f}")
    print(f"Max drawdown      : {results['max_drawdown']:.4f}")
    print(f"Total trades      : {results['total_trades']}")
    print(f"Equity curve points: {len(results['equity_curve'])}")


if __name__ == "__main__":
    main()
