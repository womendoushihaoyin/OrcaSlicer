#!/usr/bin/env python3
"""Read-only prerequisite inventory for the fixed upstream host; no fetch/configure/build."""
import argparse
import json
from pathlib import Path
import re
import subprocess

from prepare_plugin_dualhost import UPSTREAM


def git(source, *arguments):
    result = subprocess.run(["git", "-C", str(source), *arguments], capture_output=True, text=True)
    return result.returncode, result.stdout.strip()


def inspect(source, prefix):
    source, prefix = source.resolve(), prefix.resolve()
    code, root = git(source, "rev-parse", "--show-toplevel")
    checkout = code == 0 and Path(root).resolve() == source
    head = git(source, "rev-parse", "HEAD")[1] if checkout else None
    has_commit = checkout and git(source, "cat-file", "-e", UPSTREAM + "^{commit}")[0] == 0
    dirty = bool(git(source, "status", "--porcelain", "--untracked-files=no")[1]) if checkout else None
    required = ["CMakeLists.txt", "src/slic3r/CMakeLists.txt", "src/libslic3r/CMakeLists.txt",
                "src/slic3r/GUI/GUI_App.cpp", "src/slic3r/plugin/PythonPluginBridge.cpp",
                "deps/CMakeLists.txt", "deps_src/pybind11/include/pybind11/pybind11.h"]
    missing_source = [name for name in required if not (source / name).is_file()]
    paths = {
        "boost_headers": ["include/boost/version.hpp"],
        "eigen_headers": ["include/eigen3/Eigen/src/Core/util/Macros.h", "include/Eigen/src/Core/util/Macros.h"],
        "bundled_python_header": ["libpython/include/python3.12/Python.h", "libpython/include/Python.h"],
        "bundled_python_executable": ["libpython/bin/python3.12", "libpython/python.exe"],
        "bundled_python_embed": ["libpython/lib/libpython3.12.dylib", "libpython/lib/libpython3.12.so", "libpython/libs/python312.lib"],
        "ffmpeg_headers": ["include/libavcodec/avcodec.h"],
        "ffmpeg_static_avcodec": ["lib/libavcodec.a"],
        "ffmpeg_static_avformat": ["lib/libavformat.a"],
        "ffmpeg_static_avutil": ["lib/libavutil.a"],
        "ffmpeg_static_swscale": ["lib/libswscale.a"],
        "libdatachannel_cmake": ["lib/cmake/LibDataChannel/LibDataChannelConfig.cmake", "share/cmake/LibDataChannel/LibDataChannelConfig.cmake"],
        "noise_cmake": ["lib/cmake/noise/noiseConfig.cmake", "share/noise/noiseConfig.cmake"],
    }
    files = {name: {"checked": candidates, "found": [relative for relative in candidates if (prefix / relative).is_file()]}
             for name, candidates in paths.items()}
    versions = {}
    for name, expression in (("boost_headers", r"#define\s+BOOST_VERSION\s+(\d+)"),
                             ("eigen_headers", r"#define\s+EIGEN_(?:WORLD|MAJOR|MINOR)_VERSION\s+(\d+)")):
        found = files[name]["found"]
        if found:
            versions[name] = re.findall(expression, (prefix / found[0]).read_text(encoding="utf-8", errors="replace"))
    for relative in ("libpython/include/python3.12/patchlevel.h", "libpython/include/patchlevel.h"):
        path = prefix / relative
        if path.is_file():
            match = re.search(r'#define\s+PY_VERSION\s+"([^"]+)"', path.read_text(encoding="utf-8", errors="replace"))
            versions["python"] = match.group(1) if match else "unknown"
            break
    return {"schema": 1, "upstream_commit": UPSTREAM, "execution": "read-only-inventory",
        "source": {"path": str(source), "is_checkout_root": checkout, "head": head,
                   "fixed_commit_available_locally": bool(has_commit), "tracked_changes": dirty,
                   "missing_required_paths": missing_source,
                   "fixed_source_candidate": head == UPSTREAM and dirty is False and not missing_source},
        "prefix": {"path": str(prefix), "required_locations_checked": files, "versions_observed": versions},
        "required_versions": {"Boost_minimum": "1.83.0", "Eigen3": "5.0.1", "Python_exact": "3.12.13"},
        "limitations": ["Named prerequisite locations only; absence here does not prove absence elsewhere.",
                        "No configure, ABI/link check or host execution was performed.",
                        "A fixed source candidate and matching headers are necessary but not sufficient for a working host.",
                        "Stubgen links libslic3r_gui and is not a substitute for real Pages/texture GUI acceptance."]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--prefix", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    result = inspect(args.source, args.prefix)
    with args.output.open("x", encoding="utf-8") as stream:
        json.dump(result, stream, ensure_ascii=False, indent=2)
        stream.write("\n")
    print(json.dumps({"fixed_source_candidate": result["source"]["fixed_source_candidate"],
                      "fixed_commit_available_locally": result["source"]["fixed_commit_available_locally"],
                      "missing_location_groups": [name for name, group in result["prefix"]["required_locations_checked"].items() if not group["found"]],
                      "host_execution": "not-run"}))


if __name__ == "__main__":
    main()
