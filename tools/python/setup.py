"""Minimal setuptools config for ``pip install -e python/``.

The native library (``libtruetest.so``) must be built separately:

    cmake -B build -DBUILD_SHARED_LIB=ON
    cmake --build build --target truetest_shared

By default the Python module locates the library via ``TRUETEST_LIB``,
then falls back to ``<repo>/build/libtruetest.so`` — so an editable install
just works from a development checkout.
"""

from setuptools import setup


setup(
    name="truetest",
    version="0.1.0",
    description="Python bindings for the TrueTest C++17 backtesting engine",
    author="TrueTest Maintainers",
    py_modules=["truetest"],
    python_requires=">=3.8",
)
