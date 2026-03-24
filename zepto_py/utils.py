"""
ZeptoDB Python utilities — dependency checks, version info, etc.
"""
from __future__ import annotations
from typing import Dict


def check_dependencies() -> Dict[str, str]:
    """
    Check which Python libraries are available for ZeptoDB integration.

    Returns
    -------
    dict : {library_name: version_or_"not installed"}

    Example
    -------
    >>> import zepto_py
    >>> zepto_py.check_dependencies()
    {
        'numpy': '1.26.4',
        'pandas': '2.2.0',
        'polars': '0.20.3',
        'pyarrow': '15.0.0',
        'duckdb': 'not installed',
        ...
    }
    """
    results = {}

    libs = [
        "numpy", "pandas", "polars", "pyarrow",
        "duckdb", "matplotlib", "seaborn",
        "scikit-learn", "scipy",
    ]

    for lib in libs:
        try:
            mod = __import__(lib)
            results[lib] = getattr(mod, "__version__", "installed")
        except ImportError:
            results[lib] = "not installed"

    return results


def versions() -> None:
    """Print a dependency version table."""
    deps = check_dependencies()
    print("ZeptoDB Python Ecosystem")
    print("=" * 40)
    width = max(len(k) for k in deps)
    for lib, ver in deps.items():
        status = "✓" if ver != "not installed" else "✗"
        print(f"  {status} {lib:<{width}}  {ver}")
    print()


def install_missing() -> None:
    """Print pip install command for missing dependencies."""
    deps = check_dependencies()
    missing = [lib for lib, ver in deps.items() if ver == "not installed"]
    if not missing:
        print("All dependencies installed.")
        return

    core = [l for l in missing if l in ("numpy", "pandas", "polars", "pyarrow")]
    extra = [l for l in missing if l not in core]

    if core:
        print(f"Install core: pip install {' '.join(core)}")
    if extra:
        print(f"Install optional: pip install {' '.join(extra)}")
