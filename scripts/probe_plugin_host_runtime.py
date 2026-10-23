#!/usr/bin/env python3
"""Run the real embedded SDK host inside a verified, relocated runtime overlay.

Requires orca_plugin_sdk_export, never the GUI executable. Output and profile are
new directories; the probe rejects CWD imports and development-prefix fallback.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import subprocess

from package_plugin_runtime import MANIFEST, clean_environment, digest, verify


def probe(exporter: Path, runtime: Path, output: Path):
    exporter = exporter.resolve(strict=True)
    runtime = runtime.resolve(strict=True)
    output = output.absolute()
    if exporter.name not in ("orca_plugin_sdk_export", "orca_plugin_sdk_export.exe"):
        raise ValueError("Use the SDK exporter, never the GUI executable")
    if not exporter.is_relative_to(runtime):
        raise ValueError("Exporter must be copied and relocated inside the runtime overlay")
    if output.exists() or output.is_relative_to(runtime):
        raise ValueError("Choose a fresh output directory outside the runtime overlay")
    before = verify(runtime, native=True)
    manifest = json.loads((runtime / MANIFEST).read_text(encoding="utf-8"))
    expected = runtime / manifest["layout"]["python"]
    output.mkdir(parents=True)
    cwd = output / "unrelated working directory"
    cwd.mkdir()
    (cwd / "orca_cwd_probe_untrusted.py").write_text("raise RuntimeError('Unexpected CWD import')\n", encoding="utf-8")
    profile = output / "isolated-profile"
    script = output / "probe.py"
    script.write_text(f'''import sys, json, pathlib, importlib.util, ssl, sqlite3, ctypes, zlib, bz2, lzma, orca
expected = pathlib.Path({str(expected)!r}).resolve()
profile = pathlib.Path({str(profile)!r}).resolve()
assert pathlib.Path(sys.prefix).resolve() == expected, sys.prefix
assert "" not in sys.path, "Embedded interpreter added CWD to sys.path"
assert importlib.util.find_spec("orca_cwd_probe_untrusted") is None, "Embedded interpreter imports from CWD"
assert all(pathlib.Path(p).is_absolute() and (pathlib.Path(p).resolve().is_relative_to(expected) or
           pathlib.Path(p).resolve().is_relative_to(profile)) for p in sys.path), sys.path
assert pathlib.Path(ssl.__file__).resolve().is_relative_to(expected), ssl.__file__
assert pathlib.Path(sqlite3.__file__).resolve().is_relative_to(expected), sqlite3.__file__
pathlib.Path(SDK_OUTPUT).write_text(json.dumps({{"python": sys.version.split()[0], "prefix": sys.prefix,
    "sys_path": sys.path, "orca_module": orca.__name__, "cwd_import": "rejected"}}, ensure_ascii=False,
    indent=2)+"\\n", encoding="utf-8")
''', encoding="utf-8")
    environment = clean_environment()
    # An isolated PyConfig must ignore even an explicitly polluted environment.
    environment["PYTHONPATH"] = str(cwd)
    process = subprocess.run([str(exporter), str(script), str(output / "embedded.json"), str(profile)],
                             cwd=cwd, env=environment, capture_output=True, text=True, encoding="utf-8",
                             errors="replace", timeout=120)
    (output / "host.log").write_text(process.stdout + process.stderr, encoding="utf-8")
    result = {"exit_code": process.returncode, "exporter_sha256": digest(exporter),
              "runtime_manifest_sha256": digest(runtime / MANIFEST), "before": before,
              "after": verify(runtime, native=False)}
    (output / "result.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    if process.returncode:
        raise ValueError(f"Embedded host probe failed ({process.returncode}); see {output / 'host.log'}")
    if not (output / "embedded.json").is_file():
        raise ValueError("Embedded host did not produce its assertion result")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exporter", type=Path, required=True)
    parser.add_argument("--runtime", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        print(json.dumps(probe(args.exporter, args.runtime, args.output)))
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        parser.exit(1, f"error: {error}\n")


if __name__ == "__main__":
    main()
