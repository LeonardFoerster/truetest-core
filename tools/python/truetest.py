"""Python bindings for the TrueTest C API (P2).

Thin ctypes wrapper over ``libtruetest.so`` / ``truetest.dll``. The bindings
avoid numpy/pandas so they stay usable in minimal environments — callers
that want tabular results can convert the returned dicts themselves.

Example
-------
    from truetest import Engine

    results = Engine({
        "data_path": "market_data.csv",
        "strategy":  "sma",
        "params":    {"period": 20},
        "seed":      1,
    }).run()
    print(results["sharpe_ratio"])
"""

from __future__ import annotations

import ctypes
import json
import os
import platform
import sys
from ctypes import c_char_p, c_int, c_void_p
from pathlib import Path
from typing import Any, Dict, Optional


def _library_name() -> str:
    system = platform.system()
    if system == "Windows":
        return "truetest.dll"
    if system == "Darwin":
        return "libtruetest.dylib"
    return "libtruetest.so"


def _find_library() -> str:
    """Resolve the shared library path.

    Search order:
      1. ``TRUETEST_LIB`` environment variable (full path).
      2. Alongside this module (useful for wheels).
      3. ``<repo>/build/`` relative to this file (development checkout).
      4. The system loader's default search path.
    """
    env = os.environ.get("TRUETEST_LIB")
    if env:
        return env

    here = Path(__file__).resolve().parent
    name = _library_name()

    for candidate in (here / name, here.parent / "build" / name):
        if candidate.exists():
            return str(candidate)

    return name


_lib = ctypes.CDLL(_find_library())

_lib.tt_version.restype = c_char_p
_lib.tt_version.argtypes = []

_lib.tt_create_engine.restype = c_void_p
_lib.tt_create_engine.argtypes = [c_char_p]

_lib.tt_run.restype = c_int
_lib.tt_run.argtypes = [c_void_p]

_lib.tt_get_results.restype = c_void_p  # raw pointer so we can free it
_lib.tt_get_results.argtypes = [c_void_p]

_lib.tt_free_string.restype = None
_lib.tt_free_string.argtypes = [c_void_p]

_lib.tt_destroy.restype = None
_lib.tt_destroy.argtypes = [c_void_p]

_lib.tt_last_error.restype = c_char_p
_lib.tt_last_error.argtypes = []


class TrueTestError(RuntimeError):
    """Raised when the underlying C API reports an error."""


def _last_error() -> str:
    raw = _lib.tt_last_error()
    return raw.decode("utf-8", "replace") if raw else ""


def version() -> str:
    """Return the native library version string."""
    return _lib.tt_version().decode("utf-8")


class Engine:
    """High-level wrapper around a TrueTest backtest run.

    Parameters
    ----------
    config : dict
        Config dictionary; serialized to JSON before being passed to the
        C API. See ``truetest_api.h`` for the full schema.
    """

    def __init__(self, config: Dict[str, Any]):
        if not isinstance(config, dict):
            raise TypeError("config must be a dict")

        payload = json.dumps(config).encode("utf-8")
        self._handle: Optional[int] = _lib.tt_create_engine(payload)
        if not self._handle:
            raise TrueTestError(f"create_engine failed: {_last_error()}")

        self._results: Optional[Dict[str, Any]] = None

    def __enter__(self) -> "Engine":
        return self

    def __exit__(self, exc_type, exc_val, exc_tb) -> None:
        self.close()

    def close(self) -> None:
        """Release the native engine handle. Safe to call multiple times."""
        if self._handle is not None:
            _lib.tt_destroy(self._handle)
            self._handle = None

    def __del__(self) -> None:  # best-effort cleanup
        try:
            self.close()
        except Exception:
            pass

    @property
    def results(self) -> Dict[str, Any]:
        """Return the parsed results dict from the most recent run."""
        if self._results is None:
            raise TrueTestError("engine has not produced results yet (call run() first)")
        return self._results

    def run(self) -> Dict[str, Any]:
        """Execute the backtest and return the parsed results dict."""
        if self._handle is None:
            raise TrueTestError("engine has been closed")

        rc = _lib.tt_run(self._handle)
        if rc != 0:
            raise TrueTestError(f"run failed (code {rc}): {_last_error()}")

        raw_ptr = _lib.tt_get_results(self._handle)
        if not raw_ptr:
            raise TrueTestError(f"get_results failed: {_last_error()}")

        try:
            text = ctypes.string_at(raw_ptr).decode("utf-8")
        finally:
            _lib.tt_free_string(raw_ptr)

        self._results = json.loads(text)
        return self._results


__all__ = ["Engine", "TrueTestError", "version"]


if __name__ == "__main__":
    # `python -m truetest` prints the library version — useful as a smoke test.
    print(f"truetest python binding; native version = {version()}", file=sys.stderr)
