#!/usr/bin/env python3
"""Build and check prepared GUI-capable ON/OFF trees; never install, sign or publish.

Configuration and third-party dependencies are explicit prerequisites. Use
--no-build only to validate already-built artifacts, or --preflight-only to
check the cache contract without claiming a build or native-test result.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time

from package_plugin_runtime import fresh_output


NATIVE_TARGETS = (
    "orca_plugin_core_test", "orca_plugin_lock_test", "orca_plugin_preset_bundle_test",
    "orca_plugin_preset_view_test", "orca_plugin_precise_seam_test",
    "orca_plugin_observer_queue_test", "orca_plugin_unload_overlap_test", "orca_plugin_print_object_id_test",
    "orca_plugin_app_info_test", "orca_plugin_sdk_export", "orca_plugin_wheel_cache_recovery_test",
)

DEPENDENCY_RECOVERY_TARGET = "orca_plugin_dependency_recovery_test"
DEPENDENCY_RECOVERY_STEPS = (
    ("interrupted-publication", "seed-crash", 86), ("interrupted-publication", "resume", 0),
    ("legacy-empty-target", "seed-crash", 86), ("legacy-empty-target", "resume-empty", 0),
    ("blocked-target", "seed-crash", 86), ("blocked-target", "blocked-recovery", 0), ("blocked-target", "resume", 0),
    ("completed-publication", "published-crash", 86), ("completed-publication", "resume-published", 0),
)
DEPENDENCY_RECOVERY_SOURCES = (
    "src/slic3r/plugin/PluginDependencyInstaller.cpp", "src/slic3r/plugin/PluginDependencyInstaller.hpp",
    "src/slic3r/plugin/PythonInterpreter.cpp", "tests/plugin_core/dependency_recovery.cpp",
    "tests/plugin_core/dependency_crash_interpose.cpp", "tests/plugin_core/fixtures.py",
    "scripts/test_plugin_dependency_recovery.py", "cmake/OrcaPluginDependencyRecoveryTests.cmake",
    "scripts/test_plugin_host_ci.py", "scripts/test_plugin_host_ci_contracts.py", "cmake/OrcaPluginCore.cmake",
)


WHEEL_RECOVERY_TARGET = "orca_plugin_wheel_cache_recovery_test"
WHEEL_RECOVERY_CASES = tuple(f"{kind}_{fault}" for kind in ("entry", "dependency")
                            for fault in ("crc", "unsafe", "duplicate", "conflict", "legacy")) + ("entry_case_alias", "entry_restart",)
WHEEL_RECOVERY_SOURCES = (
    "src/slic3r/plugin/PluginFsUtils.cpp", "src/slic3r/plugin/PluginFsUtils.hpp",
    "src/slic3r/plugin/PythonInterpreter.cpp", "src/slic3r/plugin/PluginLoader.cpp",
    "tests/plugin_core/wheel_cache_recovery.cpp", "scripts/test_plugin_wheel_cache_recovery.py",
    "scripts/test_plugin_wheel_cache_recovery_contracts.py", "cmake/OrcaPluginWheelCacheTests.cmake",
    "scripts/test_plugin_host_ci.py", "scripts/test_plugin_host_ci_contracts.py", "cmake/OrcaPluginCore.cmake",
)


def native_targets(*, platform: str | None = None):
    return NATIVE_TARGETS + ((DEPENDENCY_RECOVERY_TARGET,) if (platform or sys.platform) == "darwin" else ())


def file_sha256(path: Path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def source_hashes(source: Path, names):
    result = {}
    for name in names:
        path = source / name
        if not path.is_file() or path.is_symlink() or not path.resolve().is_relative_to(source.resolve()):
            raise ValueError(f"Missing or external recovery source: {name}")
        result[name] = file_sha256(path)
    return result


def dependency_recovery_test_inputs(build: Path, config: str, binary: Path, output: Path, *, platform: str | None = None):
    if (platform or sys.platform) != "darwin":
        raise ValueError("Dependency crash interposition is macOS-specific; this platform is not verified")
    build = build.resolve(strict=True)
    suffix = Path(config) if cache_values(build).get("CMAKE_CONFIGURATION_TYPES", "") else Path()
    directory = build / "src/slic3r" / suffix
    expected_binary = directory / DEPENDENCY_RECOVERY_TARGET
    library = directory / "liborca_plugin_dependency_fault.dylib"
    if binary.resolve(strict=True) != expected_binary or expected_binary.is_symlink():
        raise ValueError("Dependency recovery executable must belong to the selected build/config")
    if not library.is_file() or library.is_symlink() or library.resolve() != library:
        raise ValueError("Missing or external dependency recovery fault library for the selected config")
    profile = output / "dependency-recovery-中文"
    if profile.exists() or profile.is_symlink():
        raise ValueError("Dependency recovery requires a fresh output; existing results were preserved")
    return ["--binary", str(expected_binary), "--fault-library", str(library), "--output", str(profile)], library


def dependency_recovery_passed_report(path: Path, binary: Path, library: Path):
    """Require the actual nine process reports, beyond a child driver's exit code."""
    value = json.loads(path.read_text(encoding="utf-8"))
    hashes = {"binary": file_sha256(binary), "fault_library": file_sha256(library)}
    if (value.get("schema") != 1 or value.get("passed") is not True or value.get("error") is not None or
        value.get("sha256_before") != hashes or value.get("sha256_after") != hashes or
        value.get("binary") != str(binary) or value.get("fault_library") != str(library)):
        raise RuntimeError("Dependency recovery report failed or its native artifact hashes changed")
    rows = value.get("runs", [])
    if len(rows) != len(DEPENDENCY_RECOVERY_STEPS):
        raise RuntimeError("Dependency recovery report does not contain all nine process steps")
    for row, (case, action, expected) in zip(rows, DEPENDENCY_RECOVERY_STEPS):
        if ((row.get("case"), row.get("action"), row.get("expected"), row.get("returncode")) !=
            (case, action, expected, expected) or row.get("matched") is not True):
            raise RuntimeError("Dependency recovery process steps are missing, duplicated or failed")
        for field, suffix in (("report", ".json"), ("log", ".log")):
            actual = path.parent / case / (action + suffix)
            if (row.get(field) != str(actual) or not actual.is_file() or actual.is_symlink() or
                not actual.resolve().is_relative_to(path.parent.resolve()) or
                row.get(field + "_sha256") != file_sha256(actual)):
                raise RuntimeError("Dependency recovery process evidence is missing, changed or external")
        record = json.loads(Path(row["report"]).read_text(encoding="utf-8"))
        if expected == 86:
            valid = record.get("real_std_filesystem_rename_completed") is True and record.get("forced_process_exit") == 86
        else:
            valid = record.get("passed") is True and record.get("interpreter_stopped") is True
        if not valid:
            raise RuntimeError("Dependency recovery lacks real rename or complete shutdown evidence")
    return {"status": "passed", "processes": len(rows), "report": str(path), "report_sha256": file_sha256(path)}


def run_dependency_recovery(run, source: Path, arguments, binary: Path | None, library: Path | None,
                            output: Path, environment, *, platform: str | None = None):
    if (platform or sys.platform) != "darwin":
        return {"status": "not-run", "processes": 0,
                "reason": "macOS dyld/libc++ fault injection; Windows/Linux recovery is not verified"}
    run("native-dependency-recovery",
        [sys.executable, "-X", "utf8", str(source / "scripts/test_plugin_dependency_recovery.py"), *arguments],
        environment, timeout=480)
    return dependency_recovery_passed_report(output / "dependency-recovery-中文/results.json", binary, library)


def wheel_recovery_test_inputs(build: Path, config: str, binary: Path, output: Path, *, platform: str | None = None):
    platform = platform or sys.platform
    build = build.resolve(strict=True)
    suffix = Path(config) if cache_values(build).get("CMAKE_CONFIGURATION_TYPES", "") else Path()
    expected = build / "src/slic3r" / suffix / (WHEEL_RECOVERY_TARGET + (".exe" if platform.startswith("win") else ""))
    if binary.resolve(strict=True) != expected or expected.is_symlink():
        raise ValueError("Wheel recovery executable must belong to the selected build/config")
    evidence = output / "wheel-cache-recovery-中文"
    if evidence.exists() or evidence.is_symlink():
        raise ValueError("Wheel recovery requires a fresh output; existing results were preserved")
    return ["--native", str(expected), "--output", str(evidence)]


def wheel_recovery_passed_report(path: Path, binary: Path):
    # Reuse the native fixture's phase/callback contract, not merely its summary.
    from test_plugin_wheel_cache_recovery import verify_observation
    value = json.loads(path.read_text(encoding="utf-8"))
    if (value.get("schema") != 1 or value.get("ok") is not True or value.get("native") != str(binary) or
        value.get("native_sha256") != file_sha256(binary)):
        raise RuntimeError("Wheel recovery report failed or its native artifact changed")
    rows = value.get("cases", [])
    if [row.get("scenario") for row in rows] != list(WHEEL_RECOVERY_CASES):
        raise RuntimeError("Wheel recovery report does not contain the twelve ordered process cases")
    manifest = path.parent / "fixtures/manifest.json"
    if (not manifest.is_file() or manifest.is_symlink() or
        not manifest.resolve().is_relative_to(path.parent.resolve()) or
        value.get("fixture_manifest_sha256") != file_sha256(manifest)):
        raise RuntimeError("Wheel recovery fixture manifest changed or is external")
    fixture_rows = json.loads(manifest.read_text(encoding="utf-8"))
    names = [row["path"] for row in fixture_rows]
    if not names or len(set(names)) != len(names):
        raise RuntimeError("Wheel recovery fixture manifest is empty or duplicated")
    for row in fixture_rows:
        relative = Path(row["path"])
        fixture = manifest.parent / relative
        if (relative.is_absolute() or ".." in relative.parts or not fixture.is_file() or fixture.is_symlink() or
            not fixture.resolve().is_relative_to(manifest.parent.resolve()) or
            row.get("sha256") != file_sha256(fixture) or row.get("bytes") != fixture.stat().st_size):
            raise RuntimeError("Wheel recovery fixture is missing, changed or external")
    records = []
    for row, scenario in zip(rows, WHEEL_RECOVERY_CASES):
        actual = path.parent / (scenario + ".json")
        log = path.parent / (scenario + ".log")
        profile = path.parent / (("entry_crc" if scenario == "entry_restart" else scenario) + "-profile-中文")
        if (row.get("ok") is not True or row.get("returncode") != 0 or row.get("error") or
            row.get("result") != str(actual) or row.get("profile") != str(profile)):
            raise RuntimeError("Wheel recovery case failed or reused an unexpected profile")
        for record in (actual, log):
            if (not record.is_file() or record.is_symlink() or not record.resolve().is_relative_to(path.parent.resolve())):
                raise RuntimeError("Wheel recovery process evidence is missing or external")
        if row.get("result_sha256") != file_sha256(actual):
            raise RuntimeError("Wheel recovery native result changed")
        verify_observation(scenario, json.loads(actual.read_text(encoding="utf-8")))
        records.append({"scenario": scenario, "result_sha256": file_sha256(actual), "log_sha256": file_sha256(log)})
    return {"status": "passed", "processes": len(rows), "report": str(path), "report_sha256": file_sha256(path),
            "fixture_manifest_sha256": file_sha256(manifest), "records": records}


def run_wheel_recovery(run, source: Path, arguments, binary: Path, output: Path, environment):
    run("native-wheel-cache-recovery",
        [sys.executable, "-X", "utf8", str(source / "scripts/test_plugin_wheel_cache_recovery.py"), *arguments],
        environment, timeout=720)
    return wheel_recovery_passed_report(output / "wheel-cache-recovery-中文/results.json", binary)


APP_INFO_VARIANTS = ("good", "no_version", "debug_mismatch")
APP_INFO_CHECKS = (
    "identity_mapping_four_combinations", "missing_library_cleanup_retains_error",
    "corrupt_library_real_loader_error", "real_module_unicode_source_retry_and_unload",
    "missing_symbol_preserves_legacy_return_without_app_info_sentinel", "debug_mismatch_observed",
    "compatibility_diagnostic_cleanup_and_successful_retry", "concurrent_module_snapshot_lifecycle",
)

LIFECYCLE_SCENARIOS = (
    "overlap", "shutdown", "bulk_overlap", "bulk_shutdown", "replacement", "delete",
    "reentrant", "destructor", "shutdown_gil",
)


def run_lifecycle_scenarios(run, binary: Path, output: Path, environment):
    # Separate processes avoid a prior interpreter/config state hiding teardown defects.
    for scenario in LIFECYCLE_SCENARIOS:
        run("native-unload-" + scenario,
            [str(binary), scenario, str(output / ("unload-" + scenario + "-中文"))], environment, timeout=90)


def standalone_native_commands(binaries: dict[str, Path], output: Path):
    commands = [("native-" + label, [str(binaries["orca_plugin_" + label + "_test"]),
                                    str(output / (label + "-中文"))])
                for label in ("preset_bundle", "preset_view", "precise_seam", "observer_queue")]
    # This allocator-poisoning executable has no profile or command arguments.
    commands.append(("native-print-object-id", [str(binaries["orca_plugin_print_object_id_test"])]))
    return commands


def cache_values(build: Path) -> dict[str, str]:
    result = {}
    for line in (build / "CMakeCache.txt").read_text(encoding="utf-8").splitlines():
        if line and not line.startswith(("//", "#")) and "=" in line and ":" in line.split("=", 1)[0]:
            key, value = line.split("=", 1)
            result[key.split(":", 1)[0]] = value
    return result


def enabled(value: str) -> bool:
    return value.upper() in ("ON", "TRUE", "1", "YES", "Y")


def validate_trees(source: Path, on: Path, off: Path, config: str):
    if on == off:
        raise ValueError("ON and OFF builds must be separate directories")
    caches = {}
    for name, build, expected in (("on", on, True), ("off", off, False)):
        cache = cache_values(build)
        configured_source = Path(cache.get("CMAKE_HOME_DIRECTORY", ""))
        if not configured_source.is_absolute() or configured_source.resolve() != source:
            raise ValueError(f"{name}: build uses another source checkout")
        if not enabled(cache.get("SLIC3R_GUI", "")):
            raise ValueError(f"{name}: SLIC3R_GUI=ON is required for the full host")
        if "ORCA_PYTHON_PLUGINS" not in cache or enabled(cache["ORCA_PYTHON_PLUGINS"]) != expected:
            raise ValueError(f"{name}: wrong ORCA_PYTHON_PLUGINS value")
        if enabled(cache.get("ORCA_LOCAL_PLUGIN_SPIKE", "")):
            raise ValueError(f"{name}: the spike runtime is not a production host")
        configurations = cache.get("CMAKE_CONFIGURATION_TYPES", "").split(";")
        if configurations != [""]:
            if config not in configurations:
                raise ValueError(f"{name}: configuration {config} is unavailable")
        elif cache.get("CMAKE_BUILD_TYPE") != config:
            raise ValueError(f"{name}: single-config build is not {config}")
        caches[name] = cache
    runtime = Path(caches["on"].get("ORCA_PLUGIN_PYTHON_ROOT", ""))
    uv = Path(caches["on"].get("ORCA_PLUGIN_UV_EXECUTABLE", ""))
    if not runtime.is_absolute() or not runtime.is_dir() or not uv.is_absolute() or not uv.is_file():
        raise ValueError("ON build must name existing absolute Python and uv paths")
    return caches


def artifact(build: Path, name: str, config: str, *, platform: str | None = None) -> Path:
    # Follow this repository's CMake output locations, never search unrelated trees.
    windows = (platform or sys.platform).startswith("win")
    directories = [build / "src/slic3r" / config, build / "src/slic3r",
                   build / "src" / config, build / "src"]
    if name in ("orca_plugin_app_info_test", DEPENDENCY_RECOVERY_TARGET, WHEEL_RECOVERY_TARGET):
        # Do not pick a stale flat Release executable in a multi-config build,
        # or a different config's executable beside the current fixtures.
        suffix = Path(config) if cache_values(build).get("CMAKE_CONFIGURATION_TYPES", "") else Path()
        directories = [build / "src/slic3r" / suffix, build / "src" / suffix]
    if name == "host":
        names = ["Snapmaker_Orca.dll"] if windows else ["Snapmaker_Orca", "snapmaker-orca"]
        directories += [directory / "Snapmaker_Orca.app/Contents/MacOS" for directory in directories]
    elif name == "launcher":
        names = ["snapmaker-orca.exe"]
    else:
        names = [name + (".exe" if windows else "")]
    for directory in directories:
        for filename in names:
            path = directory / filename
            if path.is_file():
                return path.resolve()
    raise ValueError(f"Missing {name} artifact under {build}")


def app_info_test_inputs(build: Path, config: str, binary: Path, output: Path, *, platform: str | None = None):
    """Resolve only this CMake target's libraries and reserve a fresh test profile."""
    platform = platform or sys.platform
    windows = platform.startswith("win")
    build = build.resolve(strict=True)
    suffix = Path(config) if cache_values(build).get("CMAKE_CONFIGURATION_TYPES", "") else Path()
    binary_directories = {build / "src/slic3r" / suffix, build / "src" / suffix}
    expected_binary = "orca_plugin_app_info_test" + (".exe" if windows else "")
    binary = binary.resolve(strict=True)
    if binary.name != expected_binary or binary.parent not in binary_directories:
        # On Windows get_libpath_in_current_directory enables a legacy sibling
        # DLL fallback only for snapmaker-orca.exe. Never rename this probe to
        # the host name or follow a symlink to the real host executable.
        raise ValueError("app_info requires the unchanged test executable name in the selected build/config")
    directory = build / "plugin-app-info-fixtures" / suffix
    extension = ".dll" if windows else ".dylib" if platform == "darwin" else ".so"
    prefix = "" if windows else "lib"
    fixtures = {}
    for variant in APP_INFO_VARIANTS:
        path = directory / (prefix + "app_info_" + variant + extension)
        if not path.is_file() or path.resolve() != path:
            raise ValueError(f"Missing or external app_info fixture for {config}: {path}")
        fixtures["app_info_fixture_" + variant] = path.resolve()
    profile = output / "app-info-中文"
    if profile.exists() or profile.is_symlink():
        raise ValueError("app_info requires a fresh profile; choose a new evidence output directory")
    if windows:
        # The existing Windows loader uses wchar_t[128]. Do not silently route
        # a long-path failure into its executable-directory fallback. This is
        # a prerequisite check, not a claimed fix to that legacy loader limit.
        longest_library = profile / "missing-version" / "plugins" / "bambu_networking.dll"
        if len(str(longest_library.resolve()).encode("utf-16-le")) // 2 + 1 > 128:
            raise ValueError("app_info exceeds the legacy Windows 128-wide-character library path; use a shorter output path")
    return [str(binary), str(profile), str(directory.resolve())], fixtures


def app_info_passed_checks(log: Path):
    checks = [line[5:].partition(" ")[0] for line in log.read_text(encoding="utf-8").splitlines() if line.startswith("PASS ")]
    if len(checks) != len(APP_INFO_CHECKS) or set(checks) != set(APP_INFO_CHECKS):
        raise RuntimeError("app_info output does not contain all eight native checks")
    return checks


def safe_log(line: str) -> str:
    # Build commands can contain private compile definitions. Keep diagnostics,
    # never echo raw compiler invocations or credential-bearing source lines.
    if re.search(r"(?:^|\s)(?:-D|/D)\S|(?:TOKEN|SECRET|PASSWORD|API_KEY|SENTRY_DSN|SIG_KEY)", line, re.I):
        return "[build definition or sensitive line omitted]\n"
    return line


def run_logged_check(output: Path, name: str, command, source: Path, environment=None, timeout=1800):
    """Record real process failures without exposing command arguments or build definitions."""
    started = time.monotonic()
    entry = {"name": name, "exit_code": None, "passed": False}
    with tempfile.TemporaryFile(mode="w+", encoding="utf-8", errors="replace") as raw:
        try:
            process = subprocess.Popen(command, cwd=source, env=environment, stdout=raw, stderr=subprocess.STDOUT,
                                       start_new_session=os.name != "nt",
                                       creationflags=subprocess.CREATE_NEW_PROCESS_GROUP if os.name == "nt" else 0)
            try:
                entry["exit_code"] = process.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                if os.name == "nt":
                    try:
                        subprocess.run(["taskkill", "/PID", str(process.pid), "/T", "/F"],
                                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
                    finally:
                        if process.poll() is None:
                            process.kill()
                        process.wait()
                else:
                    try:
                        os.killpg(process.pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
                process.wait()
                entry["exit_code"] = "timeout"
            entry["passed"] = entry["exit_code"] == 0
        except (OSError, subprocess.SubprocessError) as error:
            entry["error"] = type(error).__name__
            entry["detail"] = "Process could not start or complete; check the executable and its runtime dependencies"
        raw.seek(0)
        with (output / (name + ".log")).open("w", encoding="utf-8") as log:
            for line in raw:
                log.write(safe_log(line))
            if "error" in entry:
                log.write(entry["error"] + ": " + entry["detail"] + "\n")
            elif entry["exit_code"] == "timeout":
                log.write("Process exceeded its time limit and was terminated\n")
    entry["seconds"] = round(time.monotonic() - started, 3)
    return entry


def run_gate(args):
    source = Path(__file__).resolve().parents[1]
    on, off = args.on_build.resolve(strict=True), args.off_build.resolve(strict=True)
    caches = validate_trees(source, on, off, args.config)
    output = fresh_output(args.output, on, off, args.wheelhouse)
    if output.resolve() == source:
        raise ValueError("Choose a new evidence directory outside the source root")
    if not args.wheelhouse.is_dir() or not list(args.wheelhouse.glob("numpy-2.2.6-*.whl")):
        raise ValueError("Wheelhouse must contain the target platform's NumPy 2.2.6 wheel")
    output.mkdir(parents=True, exist_ok=False)
    report = {"platform": sys.platform, "config": args.config, "source": str(source),
              "on_build": str(on), "off_build": str(off),
              "mode": "preflight" if args.preflight_only else "existing-artifacts" if args.no_build else "build-and-test",
              "gui_interaction_tested": False, "packaged_relocation_tested": False,
              "passed": False, "checks": [], "binaries": {}}

    def save():
        (output / "results.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")

    def run(name, command, environment=None, timeout=1800):
        entry = run_logged_check(output, name, command, source, environment, timeout)
        report["checks"].append(entry)
        save()
        print(name, entry["exit_code"], flush=True)
        if not entry["passed"]:
            raise RuntimeError(f"{name} failed; see its filtered log")

    save()
    if args.preflight_only:
        report["preflight_passed"] = True
        save()
        print("PASS cache preflight only; no build or native result")
        return
    app_targets = ["Snapmaker_Orca"]
    if os.name == "nt":
        app_targets += ["Snapmaker_Orca_app_gui", "COPY_DLLS"]
    selected_native_targets = native_targets()
    if not args.no_build:
        for name, build, targets in (("build-off", off, app_targets), ("build-on", on, app_targets + list(selected_native_targets))):
            run(name, ["cmake", "--build", str(build), "--config", args.config,
                       "--parallel", str(args.jobs), "--target", *targets], timeout=args.build_timeout)
    binaries = {name: artifact(on, name, args.config) for name in selected_native_targets}
    app_info_command, app_info_fixtures = app_info_test_inputs(on, args.config, binaries["orca_plugin_app_info_test"], output)
    binaries.update(app_info_fixtures)
    dependency_arguments, dependency_library = [], None
    report["dependency_recovery"] = {"status": "pending" if sys.platform == "darwin" else "not-run", "processes": 0,
        "reason": "macOS dyld/libc++ fault injection; Windows/Linux recovery is not verified"}
    if sys.platform == "darwin":
        dependency_arguments, dependency_library = dependency_recovery_test_inputs(
            on, args.config, binaries[DEPENDENCY_RECOVERY_TARGET], output)
        binaries["dependency_recovery_fault_library"] = dependency_library
        report["dependency_recovery_sources"] = source_hashes(source, DEPENDENCY_RECOVERY_SOURCES)
    wheel_arguments = wheel_recovery_test_inputs(on, args.config, binaries[WHEEL_RECOVERY_TARGET], output)
    report["wheel_cache_recovery"] = {"status": "pending", "processes": 0}
    report["wheel_cache_recovery_sources"] = source_hashes(source, WHEEL_RECOVERY_SOURCES)
    binaries.update(on_host=artifact(on, "host", args.config), off_host=artifact(off, "host", args.config))
    if os.name == "nt":
        binaries.update(on_launcher=artifact(on, "launcher", args.config), off_launcher=artifact(off, "launcher", args.config))
    for name, path in binaries.items():
        digest = hashlib.sha256()
        with path.open("rb") as stream:
            for block in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(block)
        report["binaries"][name] = {"path": str(path), "sha256": digest.hexdigest()}
    save()
    environment = os.environ.copy()
    runtime = Path(caches["on"]["ORCA_PLUGIN_PYTHON_ROOT"])
    if os.name == "nt":
        # Build-tree DLL discovery only; packaged-loader relocation has its own gate.
        roots = [str(runtime), str(binaries["on_host"].parent)]
        environment["PATH"] = os.pathsep.join(roots + [environment.get("PATH", "")])
    elif sys.platform.startswith("linux"):
        environment["LD_LIBRARY_PATH"] = os.pathsep.join([str(runtime / "lib"), environment.get("LD_LIBRARY_PATH", "")])

    inspect = ["otool", "-L"] if sys.platform == "darwin" else ["dumpbin", "/DEPENDENTS"] if os.name == "nt" else ["ldd"]
    if not shutil.which(inspect[0]):
        raise ValueError(f"Required native dependency inspector is missing: {inspect[0]}")
    for label in ("on", "off"):
        run(label + "-host-dependencies", inspect + [str(binaries[label + "_host"])], environment)
        dependencies = (output / (label + "-host-dependencies.log")).read_text(encoding="utf-8")
        has_python = bool(re.search(r"(?:libpython3[.][0-9]+|python3[0-9]+(?:_d)?[.]dll)", dependencies, re.I))
        if has_python != (label == "on"):
            raise RuntimeError(f"{label} host Python shared-library dependency contradicts the cache flag")
        if "not found" in dependencies.lower():
            raise RuntimeError(f"{label} host has an unresolved shared library")
    scripts = source / "scripts"
    python = [sys.executable, "-X", "utf8"]
    run("native-core", python + [str(scripts / "test_plugin_core.py"), "--binary", str(binaries["orca_plugin_core_test"]),
                                 "--output-dir", str(output / "core-中文"), "--wheelhouse", str(args.wheelhouse.resolve()),
                                 "--jobs", str(args.test_jobs)], environment)
    core = json.loads((output / "core-中文/results.json").read_text(encoding="utf-8"))
    if not any(result["scenario"] == "dependency_environment" for result in core) or any(result["exit_code"] != 0 for result in core):
        raise RuntimeError("Core test output lacks a passing dependency_environment scenario")
    report["core_scenarios"] = len(core)
    for label, command in standalone_native_commands(binaries, output):
        run(label, command, environment)
    run_lifecycle_scenarios(run, binaries["orca_plugin_unload_overlap_test"], output, environment)
    report["lifecycle_scenarios"] = len(LIFECYCLE_SCENARIOS)
    save()
    run("native-app-info", app_info_command, environment, timeout=90)
    report["app_info_checks"] = app_info_passed_checks(output / "native-app-info.log")
    save()
    report["dependency_recovery"] = run_dependency_recovery(
        run, source, dependency_arguments, binaries.get(DEPENDENCY_RECOVERY_TARGET), dependency_library, output, environment)
    if sys.platform == "darwin":
        if report["dependency_recovery_sources"] != source_hashes(source, DEPENDENCY_RECOVERY_SOURCES):
            raise RuntimeError("Dependency recovery sources changed while the gate was running")
        for name in (DEPENDENCY_RECOVERY_TARGET, "dependency_recovery_fault_library"):
            if report["binaries"][name]["sha256"] != file_sha256(binaries[name]):
                raise RuntimeError("Dependency recovery native artifacts changed while the gate was running")
    save()
    report["wheel_cache_recovery"] = run_wheel_recovery(
        run, source, wheel_arguments, binaries[WHEEL_RECOVERY_TARGET], output, environment)
    if report["wheel_cache_recovery_sources"] != source_hashes(source, WHEEL_RECOVERY_SOURCES):
        raise RuntimeError("Wheel recovery sources changed while the gate was running")
    if report["binaries"][WHEEL_RECOVERY_TARGET]["sha256"] != file_sha256(binaries[WHEEL_RECOVERY_TARGET]):
        raise RuntimeError("Wheel recovery native artifact changed while the gate was running")
    save()
    run("native-lock", python + [str(scripts / "test_plugin_file_lock.py"), "--binary", str(binaries["orca_plugin_lock_test"])], environment)
    sdk = output / "SDK-中文-α"
    run("sdk-generate", python + [str(scripts / "generate_plugin_sdk.py"), "--exporter", str(binaries["orca_plugin_sdk_export"]),
                                  "--output", str(sdk)], environment)
    run("sdk-verify", python + [str(scripts / "verify_plugin_sdk.py"), str(sdk)], environment)
    run("sdk-contracts", python + [str(scripts / "test_plugin_sdk.py")], environment)
    report["passed"] = True
    save()
    print(json.dumps({"passed": True, "mode": report["mode"], "core_scenarios": len(core), "output": str(output)}, ensure_ascii=False))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--on-build", type=Path, required=True)
    parser.add_argument("--off-build", type=Path, required=True)
    parser.add_argument("--wheelhouse", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--config", default="Release")
    parser.add_argument("--jobs", type=int, default=2)
    parser.add_argument("--test-jobs", type=int, default=1)
    parser.add_argument("--build-timeout", type=int, default=14400)
    parser.add_argument("--no-build", action="store_true")
    parser.add_argument("--preflight-only", action="store_true")
    args = parser.parse_args()
    if min(args.jobs, args.test_jobs, args.build_timeout) < 1:
        parser.error("Job counts and build timeout must be positive")
    try:
        run_gate(args)
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        parser.exit(1, f"error: {error}\n")


if __name__ == "__main__":
    main()
