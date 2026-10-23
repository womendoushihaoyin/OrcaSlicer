#!/usr/bin/env python3
"""Configure, build and test the real CPython S02 host without building the GUI."""

import argparse
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--python-root", required=True, type=Path,
                        help="CPython 3.12.13 prefix with headers, shared library and stdlib")
    parser.add_argument("--build-dir", required=True, type=Path)
    parser.add_argument("--pybind-source", type=Path,
                        help="Optional pre-extracted pybind11 3.0.1 tree for offline builds")
    parser.add_argument("--cmake", default="cmake")
    parser.add_argument("--ctest", default="ctest")
    parser.add_argument("--config", default="Release")
    args = parser.parse_args()

    root = Path(__file__).resolve().parents[1]
    build = args.build_dir.resolve()
    if build == root or root in build.parents:
        parser.error("Choose a build directory outside the source checkout")
    configure = [args.cmake, "-S", str(root / "tests/plugin_runtime_spike"),
                 "-B", str(build), f"-DCMAKE_BUILD_TYPE={args.config}",
                 f"-DORCA_PLUGIN_PYTHON_ROOT={args.python_root.resolve()}"]
    if args.pybind_source:
        configure.append(f"-DFETCHCONTENT_SOURCE_DIR_ORCA_PYBIND11={args.pybind_source.resolve()}")
    subprocess.run(configure, check=True)
    subprocess.run([args.cmake, "--build", str(build), "--config", args.config, "--parallel", "2"], check=True)
    subprocess.run([args.ctest, "--test-dir", str(build), "-C", args.config, "--output-on-failure"], check=True)


if __name__ == "__main__":
    main()
