#!/usr/bin/env python3
"""Native CI entry point. Inputs must already exist; this script never downloads."""
import argparse
import json
import platform
from pathlib import Path
import subprocess
import sys
import tarfile

from package_plugin_runtime import digest, fresh_output


def record(output, checks, entry):
    checks.append(entry)
    (output / "results.json").write_text(json.dumps(checks, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    if not entry["passed"]:
        raise RuntimeError(f'{entry["name"]} failed; see results.json')


def run_check(output, checks, name, command, expected=0, diagnostic=None, timeout=300):
    """A failed spawn or timeout must leave a failed result, never an all-green prefix."""
    try:
        result = subprocess.run(command, text=True, encoding="utf-8", errors="replace",
                                capture_output=True, timeout=timeout)
        entry = {"name": name, "exit_code": result.returncode,
                 "passed": result.returncode == expected and (diagnostic is None or diagnostic in result.stderr),
                 "stdout": result.stdout[-5000:], "stderr": result.stderr[-3000:]}
    except (OSError, subprocess.SubprocessError) as error:
        entry = {"name": name, "exit_code": None, "passed": False,
                 "error": type(error).__name__, "detail": "Process could not start or finish within its time limit"}
    record(output, checks, entry)


def archive_roundtrip(output, checks, runtime, offline):
    try:
        package = output / "runtime-and-wheelhouse.tar.gz"
        with tarfile.open(package, "w:gz") as archive:
            archive.add(runtime, arcname="runtime")
            archive.add(offline, arcname="offline-numpy")
        unpacked = output / "归档解包 with spaces"
        unpacked.mkdir()
        with tarfile.open(package, "r:gz") as archive:
            archive.extractall(unpacked, filter="data")
        entry = {"name": "archive-roundtrip", "passed": True, "sha256": digest(package)}
    except (OSError, ValueError, tarfile.TarError) as error:
        record(output, checks, {"name": "archive-roundtrip", "passed": False, "error": type(error).__name__})
        raise  # record raises; do not continue with an incomplete extraction
    record(output, checks, entry)
    return unpacked


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--python-root", type=Path, required=True)
    parser.add_argument("--uv", type=Path, required=True)
    parser.add_argument("--wheelhouse", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if sys.version_info < (3, 12):
        parser.error("Runtime validation requires Python 3.12 or newer (safe tar archive extraction)")
    output = fresh_output(args.output, args.python_root, args.wheelhouse)
    output.mkdir(parents=True, exist_ok=False)
    scripts = Path(__file__).resolve().parent
    checks = []

    summary = {"passed": False, "complete": False, "platform": sys.platform,
               "machine": platform.machine(), "scope": "native-runtime-overlay"}
    summary_path = output / "summary.json"
    summary_path.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")

    def check(name, script, arguments, expected=0, diagnostic=None):
        run_check(output, checks, name, [sys.executable, "-B", "-X", "utf8", str(scripts / script), *map(str, arguments)],
                  expected, diagnostic)

    check("packaging-contracts", "test_plugin_distribution.py", [])
    check("release-integration-contracts", "test_plugin_release.py", [])
    runtime = output / "runtime"
    check("stage-runtime", "package_plugin_runtime.py", ["stage", "--python-root", args.python_root,
          "--uv", args.uv, "--output", runtime])
    moved = output / "搬迁路径 with spaces" / "runtime"
    moved.parent.mkdir()
    runtime.rename(moved)
    check("relocated-native-runtime", "package_plugin_runtime.py", ["verify", moved, "--native"])
    requirements = output / "requirements.in"
    requirements.write_text("numpy==2.2.6\n", encoding="utf-8")
    offline = output / "offline-numpy"
    check("native-numpy-offline", "package_plugin_wheelhouse.py", ["--runtime", moved, "--wheelhouse", args.wheelhouse,
          "--requirements", requirements, "--output", offline, "--check-import", "numpy"])
    check("offline-integrity-reinstall", "package_plugin_wheelhouse.py", ["--runtime", moved, "--verify-package", offline])
    # Actual corruption must fail before an interpreter or installer runs.
    wheel = next((offline / "wheelhouse").glob("*.whl"))
    original_wheel = wheel.read_bytes()
    try:
        wheel.write_bytes(original_wheel + b"tampered")
        check("tampered-wheel-rejected", "package_plugin_wheelhouse.py", ["--runtime", moved, "--verify-package", offline], 1, "Wheelhouse inventory mismatch")
    finally:
        wheel.write_bytes(original_wheel)
    # Validate the exact archive downloaded by consumers, including links and modes.
    unpacked = archive_roundtrip(output, checks, moved, offline)
    check("archive-native-runtime", "package_plugin_runtime.py", ["verify", unpacked / "runtime", "--native"])
    check("archive-offline-reinstall", "package_plugin_wheelhouse.py",
          ["--runtime", unpacked / "runtime", "--verify-package", unpacked / "offline-numpy"])
    summary.update({"checks": len(checks), "passed": True, "complete": True})
    summary_path.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({**summary, "output": str(output)}, ensure_ascii=False))


if __name__ == "__main__":
    main()
