#!/usr/bin/env python3
"""Driver input/logging contracts; these do not pretend to execute the host."""
from pathlib import Path
import json
import os
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

from test_plugin_host_ci import (artifact, safe_log, validate_trees, run_lifecycle_scenarios,
                                 standalone_native_commands, run_logged_check, run_gate, NATIVE_TARGETS,
                                 APP_INFO_VARIANTS, APP_INFO_CHECKS, app_info_test_inputs, app_info_passed_checks,
                                 DEPENDENCY_RECOVERY_TARGET, DEPENDENCY_RECOVERY_STEPS, native_targets,
                                 dependency_recovery_test_inputs, dependency_recovery_passed_report, run_dependency_recovery,
                                 file_sha256, source_hashes, WHEEL_RECOVERY_TARGET, WHEEL_RECOVERY_CASES,
                                 wheel_recovery_test_inputs, wheel_recovery_passed_report, run_wheel_recovery)


class HostGateContracts(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="plugin-host-ci-中文-")
        self.root = Path(self.temporary.name).resolve()
        self.source = self.root / "source"
        self.source.mkdir()
        self.on, self.off = self.root / "on", self.root / "off"
        self.python, self.uv = self.root / "python", self.root / "uv"
        self.python.mkdir()
        self.uv.touch()
        self.values = {"CMAKE_HOME_DIRECTORY": str(self.source), "CMAKE_BUILD_TYPE": "Release",
                       "SLIC3R_GUI": "ON", "ORCA_LOCAL_PLUGIN_SPIKE": "OFF",
                       "ORCA_PLUGIN_PYTHON_ROOT": str(self.python), "ORCA_PLUGIN_UV_EXECUTABLE": str(self.uv)}
        self.write(self.on, "ON")
        self.write(self.off, "OFF")

    def tearDown(self):
        self.temporary.cleanup()

    def write(self, path, mode, changes=None):
        path.mkdir(exist_ok=True)
        values = dict(self.values, ORCA_PYTHON_PLUGINS=mode)
        values.update(changes or {})
        (path / "CMakeCache.txt").write_text("\n".join(k + ":STRING=" + v for k, v in values.items()), encoding="utf-8")

    def test_real_source_and_explicit_feature_pair_required(self):
        validate_trees(self.source, self.on, self.off, "Release")
        for changes in ({"CMAKE_HOME_DIRECTORY": str(self.root / "another-checkout")},
                        {"CMAKE_HOME_DIRECTORY": ""}, {"ORCA_PYTHON_PLUGINS": "OFF"},
                        {"SLIC3R_GUI": "OFF"}, {"ORCA_LOCAL_PLUGIN_SPIKE": "ON"},
                        {"ORCA_PLUGIN_UV_EXECUTABLE": ""}, {"CMAKE_BUILD_TYPE": "Debug"}):
            with self.subTest(changes=changes):
                self.write(self.on, "ON", changes)
                with self.assertRaises(ValueError):
                    validate_trees(self.source, self.on, self.off, "Release")

    def test_same_tree_cannot_supply_both_results(self):
        with self.assertRaises(ValueError):
            validate_trees(self.source, self.on, self.on, "Release")

    def test_multiconfig_must_contain_requested_config(self):
        self.write(self.on, "ON", {"CMAKE_BUILD_TYPE": "", "CMAKE_CONFIGURATION_TYPES": "Debug;Release"})
        validate_trees(self.source, self.on, self.off, "Release")
        self.write(self.on, "ON", {"CMAKE_BUILD_TYPE": "", "CMAKE_CONFIGURATION_TYPES": "Debug"})
        with self.assertRaises(ValueError):
            validate_trees(self.source, self.on, self.off, "Release")

    def test_artifact_lookup_stays_inside_selected_build(self):
        import os
        binary = self.on / "src/slic3r/Release" / ("orca_plugin_sdk_export.exe" if os.name == "nt" else "orca_plugin_sdk_export")
        binary.parent.mkdir(parents=True)
        binary.touch()
        self.assertEqual(artifact(self.on, "orca_plugin_sdk_export", "Release"), binary.resolve())
        with self.assertRaises(ValueError):
            artifact(self.off, "orca_plugin_sdk_export", "Release")

    def test_compile_definitions_are_not_saved_as_evidence(self):
        for line in ('clang++ -DPRIVATE_VALUE="example" -c file.cpp\n', 'cl /DEXAMPLE=value file.cpp\n',
                     'const char * API_KEY = "example";\n'):
            self.assertEqual(safe_log(line), "[build definition or sensitive line omitted]\n")
        self.assertEqual(safe_log("file.cpp:42: error: missing declaration\n"), "file.cpp:42: error: missing declaration\n")

    def test_lifecycle_gate_runs_every_boundary_in_a_separate_unicode_profile(self):
        calls = []
        binary, output = self.root / "native test", self.root / "证据"
        environment = {"PATH": "test-path"}
        def run(label, command, env, timeout):
            self.assertIs(env, environment)
            self.assertEqual(command[0], str(binary))
            self.assertEqual(timeout, 90)
            self.assertTrue(Path(command[2]).is_relative_to(output))
            self.assertIn("中文", command[2])
            calls.append((label, command))
        run_lifecycle_scenarios(run, binary, output, environment)
        self.assertEqual({command[1] for _, command in calls}, {
            "overlap", "shutdown", "bulk_overlap", "bulk_shutdown", "replacement", "delete",
            "reentrant", "destructor", "shutdown_gil"})
        self.assertEqual(len({command[2] for _, command in calls}), 9)
        self.assertEqual(len({label for label, _ in calls}), 9)

    def test_lifecycle_failure_cannot_be_reported_as_a_complete_gate(self):
        seen = []
        def fail(label, command, env, timeout):
            seen.append(command[1])
            if command[1] == "bulk_shutdown":
                raise RuntimeError("native regression failed")
        with self.assertRaisesRegex(RuntimeError, "native regression failed"):
            run_lifecycle_scenarios(fail, self.root / "native", self.root / "out", {})
        self.assertEqual(seen[-1], "bulk_shutdown")
        self.assertNotIn("replacement", seen)

    def test_allocator_poison_executable_keeps_its_no_argument_contract(self):
        binaries = {"orca_plugin_" + label + "_test": self.root / label for label in
                    ("preset_bundle", "preset_view", "precise_seam", "observer_queue", "print_object_id")}
        commands = dict(standalone_native_commands(binaries, self.root / "证据"))
        self.assertEqual(commands.pop("native-print-object-id"), [str(self.root / "print_object_id")])
        self.assertEqual(len(commands), 4)
        self.assertTrue(all(len(command) == 2 and "中文" in command[1] for command in commands.values()))

    def app_info_artifacts(self, platform, multiconfig=False):
        self.write(self.on, "ON", {"CMAKE_CONFIGURATION_TYPES": "Debug;Release" if multiconfig else ""})
        suffix = Path("Release") if multiconfig else Path()
        binary = self.on / "src/slic3r" / suffix / ("orca_plugin_app_info_test.exe" if platform == "win32" else "orca_plugin_app_info_test")
        binary.parent.mkdir(parents=True, exist_ok=True)
        binary.touch()
        directory = self.on / "plugin-app-info-fixtures" / suffix
        directory.mkdir(parents=True, exist_ok=True)
        prefix, extension = ("", ".dll") if platform == "win32" else ("lib", ".dylib" if platform == "darwin" else ".so")
        for variant in APP_INFO_VARIANTS:
            (directory / (prefix + "app_info_" + variant + extension)).write_bytes(variant.encode("ascii"))
        return binary, directory

    def test_app_info_target_and_platform_config_artifacts(self):
        self.assertIn("orca_plugin_app_info_test", NATIVE_TARGETS)
        for platform in ("win32", "darwin", "linux"):
            for multi in (False, True):
                with self.subTest(platform=platform, multiconfig=multi):
                    binary, directory = self.app_info_artifacts(platform, multi)
                    self.assertEqual(artifact(self.on, "orca_plugin_app_info_test", "Release", platform=platform), binary.resolve())
                    # Merely construct the Windows command: no host execution or
                    # writes outside this test's disposable directory occur.
                    output = Path(self.root.anchor) / ("appinfo-" + self.root.name[-6:]) if platform == "win32" else self.root / "evidence"
                    command, libraries = app_info_test_inputs(self.on, "Release", binary, output, platform=platform)
                    self.assertEqual(command, [str(binary.resolve()), str(output / "app-info-中文"), str(directory.resolve())])
                    self.assertEqual(set(libraries), {"app_info_fixture_" + variant for variant in APP_INFO_VARIANTS})
                    self.assertTrue(all(path.parent == directory.resolve() for path in libraries.values()))

    def test_app_info_multiconfig_rejects_stale_flat_binary_and_fixtures(self):
        binary, directory = self.app_info_artifacts("darwin")
        self.write(self.on, "ON", {"CMAKE_CONFIGURATION_TYPES": "Debug;Release"})
        with self.assertRaises(ValueError):
            artifact(self.on, "orca_plugin_app_info_test", "Release", platform="darwin")
        selected = binary.parent / "Release" / binary.name
        selected.parent.mkdir()
        selected.touch()
        with self.assertRaisesRegex(ValueError, "Missing or external"):
            app_info_test_inputs(self.on, "Release", selected, self.root / "evidence", platform="darwin")
        self.assertTrue(directory.is_dir())  # Flat fixtures must not satisfy Release.
        with self.assertRaisesRegex(ValueError, "selected build/config"):
            app_info_test_inputs(self.on, "Release", binary, self.root / "evidence", platform="darwin")

    def test_app_info_requires_every_real_fixture_input(self):
        binary, directory = self.app_info_artifacts("linux")
        (directory / "libapp_info_no_version.so").unlink()
        with self.assertRaisesRegex(ValueError, "Missing or external"):
            app_info_test_inputs(self.on, "Release", binary, self.root / "evidence", platform="linux")

    def test_app_info_rejects_reused_profile_without_deleting_it(self):
        binary, _ = self.app_info_artifacts("darwin")
        output = self.root / "evidence"
        profile = output / "app-info-中文"
        profile.mkdir(parents=True)
        marker = profile / "keep.txt"
        marker.write_text("existing result", encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "fresh profile"):
            app_info_test_inputs(self.on, "Release", binary, output, platform="darwin")
        self.assertEqual(marker.read_text(encoding="utf-8"), "existing result")

    def test_app_info_rejects_windows_host_name_and_long_loader_path(self):
        binary, _ = self.app_info_artifacts("win32")
        host = binary.parent / "snapmaker-orca.exe"
        host.touch()
        with self.assertRaisesRegex(ValueError, "unchanged test executable"):
            app_info_test_inputs(self.on, "Release", host, self.root / "evidence", platform="win32")
        with self.assertRaisesRegex(ValueError, "128-wide-character"):
            app_info_test_inputs(self.on, "Release", binary, self.root / ("long-" * 30), platform="win32")

    def test_app_info_rejects_external_fixtures_and_dangling_profile_links(self):
        binary, directory = self.app_info_artifacts("darwin")
        library = directory / "libapp_info_good.dylib"
        outside = self.root / "external.dylib"
        outside.touch()
        library.unlink()
        try:
            library.symlink_to(outside)
        except OSError:
            self.skipTest("Creating symlinks requires platform permission")
        with self.assertRaisesRegex(ValueError, "Missing or external"):
            app_info_test_inputs(self.on, "Release", binary, self.root / "evidence", platform="darwin")
        library.unlink()
        library.touch()
        output = self.root / "evidence"
        output.mkdir()
        (output / "app-info-中文").symlink_to(self.root / "missing-target", target_is_directory=True)
        with self.assertRaisesRegex(ValueError, "fresh profile"):
            app_info_test_inputs(self.on, "Release", binary, output, platform="darwin")

    def test_app_info_exit_zero_cannot_replace_complete_check_output(self):
        log = self.root / "native-app-info.log"
        log.write_text("".join("PASS " + item + " (scope note)\n" for item in APP_INFO_CHECKS), encoding="utf-8")
        self.assertEqual(app_info_passed_checks(log), list(APP_INFO_CHECKS))
        for items in (APP_INFO_CHECKS[:-1], APP_INFO_CHECKS[:-1] + APP_INFO_CHECKS[:1], ()):
            log.write_text("".join("PASS " + item + "\n" for item in items), encoding="utf-8")
            with self.assertRaisesRegex(RuntimeError, "all eight"):
                app_info_passed_checks(log)
        log.write_text("PASS \n", encoding="utf-8")
        with self.assertRaisesRegex(RuntimeError, "all eight"):
            app_info_passed_checks(log)

    def dependency_artifacts(self, multiconfig=False):
        self.write(self.on, "ON", {"CMAKE_CONFIGURATION_TYPES": "Debug;Release" if multiconfig else ""})
        directory = self.on / "src/slic3r" / (Path("Release") if multiconfig else Path())
        directory.mkdir(parents=True, exist_ok=True)
        binary = directory / DEPENDENCY_RECOVERY_TARGET
        library = directory / "liborca_plugin_dependency_fault.dylib"
        binary.write_bytes(b"contract-only-executable")
        library.write_bytes(b"contract-only-library")
        return binary, library

    def dependency_report(self, binary, library):
        # Synthetic records validate the collector contract only, never host behavior.
        output = self.root / "dependency-evidence"
        output.mkdir(exist_ok=True)
        rows = []
        for case, action, expected in DEPENDENCY_RECOVERY_STEPS:
            directory = output / case
            directory.mkdir(exist_ok=True)
            report, log = directory / (action + ".json"), directory / (action + ".log")
            record = ({"real_std_filesystem_rename_completed": True, "forced_process_exit": 86}
                      if expected == 86 else {"mode": action, "passed": True, "interpreter_stopped": True})
            report.write_text(json.dumps(record), encoding="utf-8")
            log.write_text("synthetic collector contract only\n", encoding="utf-8")
            rows.append({"case": case, "action": action, "expected": expected, "returncode": expected, "matched": True,
                         "report": str(report), "report_sha256": file_sha256(report), "log": str(log), "log_sha256": file_sha256(log)})
        hashes = {"binary": file_sha256(binary), "fault_library": file_sha256(library)}
        value = {"schema": 1, "passed": True, "error": None, "binary": str(binary), "fault_library": str(library),
                 "sha256_before": hashes, "sha256_after": hashes, "runs": rows}
        report = output / "results.json"
        report.write_text(json.dumps(value), encoding="utf-8")
        return report, value

    def test_dependency_targets_and_no_nonmacos_pass_claim(self):
        self.assertIn(DEPENDENCY_RECOVERY_TARGET, native_targets(platform="darwin"))
        calls = []
        for platform in ("win32", "linux"):
            self.assertNotIn(DEPENDENCY_RECOVERY_TARGET, native_targets(platform=platform))
            result = run_dependency_recovery(lambda *args, **kwargs: calls.append(args), self.source, [], None, None,
                                             self.root, {}, platform=platform)
            self.assertEqual(result["status"], "not-run")
            self.assertEqual(result["processes"], 0)
            self.assertNotIn("passed", result)
        self.assertEqual(calls, [])

    def test_dependency_inputs_use_exact_config_and_unicode_argument_boundaries(self):
        for multi in (False, True):
            binary, library = self.dependency_artifacts(multi)
            output = self.root / "证据 with & spaces"
            arguments, actual = dependency_recovery_test_inputs(self.on, "Release", binary, output, platform="darwin")
            self.assertEqual(actual, library)
            self.assertEqual(arguments, ["--binary", str(binary), "--fault-library", str(library), "--output", str(output / "dependency-recovery-中文")])
            self.assertEqual(artifact(self.on, DEPENDENCY_RECOVERY_TARGET, "Release", platform="darwin"), binary)
            with self.assertRaisesRegex(ValueError, "macOS-specific"):
                dependency_recovery_test_inputs(self.on, "Release", binary, output, platform="win32")

    def test_dependency_rejects_stale_missing_external_or_reused_inputs(self):
        binary, library = self.dependency_artifacts()
        self.write(self.on, "ON", {"CMAKE_CONFIGURATION_TYPES": "Debug;Release"})
        with self.assertRaises(ValueError):
            artifact(self.on, DEPENDENCY_RECOVERY_TARGET, "Release", platform="darwin")
        with self.assertRaisesRegex(ValueError, "selected build/config"):
            dependency_recovery_test_inputs(self.on, "Release", binary, self.root, platform="darwin")
        binary, library = self.dependency_artifacts(True)
        library.unlink()
        with self.assertRaisesRegex(ValueError, "fault library"):
            dependency_recovery_test_inputs(self.on, "Release", binary, self.root, platform="darwin")
        external = self.root / "external-fault.dylib"
        external.touch()
        try:
            library.symlink_to(external)
        except OSError:
            self.skipTest("Creating symlinks requires platform permission")
        with self.assertRaisesRegex(ValueError, "fault library"):
            dependency_recovery_test_inputs(self.on, "Release", binary, self.root, platform="darwin")
        library.unlink()
        library.touch()
        existing = self.root / "dependency-recovery-中文"
        existing.mkdir()
        marker = existing / "keep"
        marker.write_text("previous evidence", encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "fresh output"):
            dependency_recovery_test_inputs(self.on, "Release", binary, self.root, platform="darwin")
        self.assertEqual(marker.read_text(encoding="utf-8"), "previous evidence")

    def test_dependency_report_requires_all_nine_ordered_real_outcomes(self):
        binary, library = self.dependency_artifacts()
        report, value = self.dependency_report(binary, library)
        self.assertEqual(dependency_recovery_passed_report(report, binary, library)["processes"], 9)
        for mutate in (lambda v: v["runs"].pop(), lambda v: v["runs"].reverse(),
                       lambda v: v["runs"][0].update(returncode=0), lambda v: v["runs"][2].update(matched=False),
                       lambda v: v["runs"].__setitem__(1, v["runs"][0])):
            changed = json.loads(json.dumps(value))
            mutate(changed)
            report.write_text(json.dumps(changed), encoding="utf-8")
            with self.assertRaises(RuntimeError):
                dependency_recovery_passed_report(report, binary, library)

    def test_dependency_report_rejects_changed_native_or_process_evidence(self):
        binary, library = self.dependency_artifacts()
        report, value = self.dependency_report(binary, library)
        library.write_bytes(b"changed library")
        with self.assertRaisesRegex(RuntimeError, "hashes changed"):
            dependency_recovery_passed_report(report, binary, library)
        binary, library = self.dependency_artifacts()
        report, value = self.dependency_report(binary, library)
        Path(value["runs"][0]["log"]).write_text("changed log", encoding="utf-8")
        with self.assertRaisesRegex(RuntimeError, "evidence"):
            dependency_recovery_passed_report(report, binary, library)
        report, value = self.dependency_report(binary, library)
        record = Path(value["runs"][0]["report"])
        record.write_text(json.dumps({"forced_process_exit": 86}), encoding="utf-8")
        value["runs"][0]["report_sha256"] = file_sha256(record)
        report.write_text(json.dumps(value), encoding="utf-8")
        with self.assertRaisesRegex(RuntimeError, "real rename"):
            dependency_recovery_passed_report(report, binary, library)
        report, value = self.dependency_report(binary, library)
        record = Path(value["runs"][1]["report"])
        record.write_text(json.dumps({"passed": True, "interpreter_stopped": False}), encoding="utf-8")
        value["runs"][1]["report_sha256"] = file_sha256(record)
        report.write_text(json.dumps(value), encoding="utf-8")
        with self.assertRaisesRegex(RuntimeError, "shutdown"):
            dependency_recovery_passed_report(report, binary, library)

    def test_dependency_driver_exit_zero_does_not_replace_report(self):
        binary, library = self.dependency_artifacts()
        calls = []
        def run(label, command, environment, timeout):
            calls.append((label, command, timeout))
        with self.assertRaises(FileNotFoundError):
            run_dependency_recovery(run, self.source, ["--binary", str(binary), "--fault-library", str(library)],
                                    binary, library, self.root, {}, platform="darwin")
        self.assertEqual(calls[0][0], "native-dependency-recovery")
        self.assertEqual(calls[0][1][:3], [sys.executable, "-X", "utf8"])
        self.assertEqual(calls[0][2], 480)

    def test_recovery_source_hashes_record_current_content_and_reject_missing_inputs(self):
        names = ("scripts/example.py", "src/example.cpp")
        for name in names:
            path = self.source / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("original", encoding="utf-8")
        before = source_hashes(self.source, names)
        self.assertEqual(set(before), set(names))
        (self.source / names[0]).write_text("changed", encoding="utf-8")
        self.assertNotEqual(before, source_hashes(self.source, names))
        (self.source / names[1]).unlink()
        with self.assertRaisesRegex(ValueError, "Missing or external"):
            source_hashes(self.source, names)


    def wheel_artifact(self, platform="darwin", multi=False):
        self.write(self.on, "ON", {"CMAKE_CONFIGURATION_TYPES": "Debug;Release" if multi else ""})
        binary = self.on / "src/slic3r" / ("Release" if multi else "") / (WHEEL_RECOVERY_TARGET + (".exe" if platform == "win32" else ""))
        binary.parent.mkdir(parents=True, exist_ok=True)
        binary.write_bytes(b"synthetic native artifact; not execution evidence")
        return binary

    def wheel_report(self, binary):
        # Synthetic records exercise the summary verifier only, never count as a native run.
        folder = self.root / "wheel-cache-recovery-中文"
        folder.mkdir(exist_ok=True)
        fixture = folder / "fixtures/input.whl"
        fixture.parent.mkdir(exist_ok=True)
        fixture.write_bytes(b"synthetic fixture content")
        manifest = fixture.parent / "manifest.json"
        manifest.write_text(json.dumps([dict(path=fixture.name, sha256=file_sha256(fixture), bytes=fixture.stat().st_size)]), encoding="utf-8")
        value = dict(schema=1, ok=True, native=str(binary), native_sha256=file_sha256(binary),
                     fixture_manifest_sha256=file_sha256(manifest), cases=[])
        for scenario in WHEEL_RECOVERY_CASES:
            restart, legacy = scenario == "entry_restart", scenario.endswith("_legacy")
            payload = "ORIGINAL" if legacy else "COMPLETE"
            good = {"cross_process_complete_cache_reuse": "UPDATED"} if restart else {
                "legacy_unmarked_cache_recovery" if legacy else "good_archive_repair": payload,
                "complete_cache_reuse": payload, "truncated_cached_resource_recovery": payload,
                "invalid_completion_record_recovery": payload, "missing_cached_resource_recovery": payload,
                "changed_archive_recovery": "UPDATED"}
            bad = [] if restart or legacy else ["first_corrupt_load", "same_corrupt_retry", "corrupt_replacement_rejected"]
            if bad:
                good["restored_archive_reuse"] = payload
            observations = [dict(phase=phase, loaded=True, actual_callback_data=data, error="", cache_exists=True,
                                 cache_files={".orca-wheel-cache.json": 50}) for phase, data in good.items()]
            observations += [dict(phase=phase, loaded=False, error="rejected", cache_exists=phase == "corrupt_replacement_rejected") for phase in bad]
            record = folder / (scenario + ".json")
            data = dict(scenario=scenario, passed=True, interpreter_stopped=True, failures=[], observations=observations)
            if scenario == "entry_case_alias":
                data["filesystem_case_insensitive"] = True
            record.write_text(json.dumps(data), encoding="utf-8")
            (folder / (scenario + ".log")).write_text("synthetic validator input", encoding="utf-8")
            profile = folder / (("entry_crc" if restart else scenario) + "-profile-中文")
            value["cases"].append(dict(scenario=scenario, ok=True, returncode=0, result=str(record), profile=str(profile),
                                       result_sha256=file_sha256(record)))
        report = folder / "results.json"
        report.write_text(json.dumps(value), encoding="utf-8")
        return report, value

    def test_wheel_target_and_exact_config_unicode_arguments_on_each_platform(self):
        for platform in ("darwin", "win32", "linux"):
            self.assertIn(WHEEL_RECOVERY_TARGET, native_targets(platform=platform))
            for multi in (False, True):
                binary = self.wheel_artifact(platform, multi)
                output = self.root / "证据 & spaces"
                args = wheel_recovery_test_inputs(self.on, "Release", binary, output, platform=platform)
                self.assertEqual(args, ["--native", str(binary), "--output", str(output / "wheel-cache-recovery-中文")])
                self.assertEqual(artifact(self.on, WHEEL_RECOVERY_TARGET, "Release", platform=platform), binary)

    def test_wheel_rejects_old_configuration_external_binary_and_reused_evidence(self):
        binary = self.wheel_artifact()
        self.write(self.on, "ON", {"CMAKE_CONFIGURATION_TYPES": "Debug;Release"})
        with self.assertRaises(ValueError):
            artifact(self.on, WHEEL_RECOVERY_TARGET, "Release", platform="darwin")
        with self.assertRaisesRegex(ValueError, "selected build/config"):
            wheel_recovery_test_inputs(self.on, "Release", binary, self.root, platform="darwin")
        binary = self.wheel_artifact(multi=True)
        outside = self.root / binary.name
        outside.touch()
        with self.assertRaisesRegex(ValueError, "selected build/config"):
            wheel_recovery_test_inputs(self.on, "Release", outside, self.root, platform="darwin")
        folder = self.root / "wheel-cache-recovery-中文"
        folder.mkdir()
        (folder / "keep").touch()
        with self.assertRaisesRegex(ValueError, "fresh output"):
            wheel_recovery_test_inputs(self.on, "Release", binary, self.root, platform="darwin")
        self.assertTrue((folder / "keep").exists())

    def test_wheel_report_requires_twelve_ordered_cases_and_explicit_restart_profile(self):
        binary = self.wheel_artifact()
        report, value = self.wheel_report(binary)
        result = wheel_recovery_passed_report(report, binary)
        self.assertEqual(result["processes"], 12)
        self.assertEqual(len(result["records"]), 12)
        for mutate in (lambda v: v["cases"].pop(), lambda v: v["cases"].reverse(),
                       lambda v: v["cases"][0].update(ok=False), lambda v: v["cases"][0].update(returncode=1),
                       lambda v: v["cases"][-1].update(profile=str(self.root / "wrong-restart-profile"))):
            changed = json.loads(json.dumps(value))
            mutate(changed)
            report.write_text(json.dumps(changed), encoding="utf-8")
            with self.assertRaises(RuntimeError):
                wheel_recovery_passed_report(report, binary)

    def test_wheel_report_rechecks_raw_callback_shutdown_and_phase_contract(self):
        binary = self.wheel_artifact()
        for mutate in (lambda v: v.update(interpreter_stopped=False), lambda v: v["observations"].pop(),
                       lambda v: v["observations"][0].update(actual_callback_data="MISSING")):
            report, value = self.wheel_report(binary)
            record = Path(value["cases"][0]["result"])
            data = json.loads(record.read_text(encoding="utf-8"))
            mutate(data)
            record.write_text(json.dumps(data), encoding="utf-8")
            value["cases"][0]["result_sha256"] = file_sha256(record)
            report.write_text(json.dumps(value), encoding="utf-8")
            with self.assertRaises(ValueError):
                wheel_recovery_passed_report(report, binary)

    def test_wheel_alias_requires_actual_filesystem_probe_and_matching_observations(self):
        binary = self.wheel_artifact()
        report, value = self.wheel_report(binary)
        row = next(row for row in value["cases"] if row["scenario"] == "entry_case_alias")
        record = Path(row["result"])
        data = json.loads(record.read_text(encoding="utf-8"))
        def save():
            record.write_text(json.dumps(data), encoding="utf-8")
            row["result_sha256"] = file_sha256(record)
            report.write_text(json.dumps(value), encoding="utf-8")
        del data["filesystem_case_insensitive"]
        save()
        with self.assertRaisesRegex(ValueError, "case-sensitivity probe"):
            wheel_recovery_passed_report(report, binary)
        data["filesystem_case_insensitive"] = False
        save()
        with self.assertRaisesRegex(ValueError, "loading phases"):
            wheel_recovery_passed_report(report, binary)
        data["observations"] = [item for item in data["observations"] if item["phase"] not in
                               ("first_corrupt_load", "same_corrupt_retry", "corrupt_replacement_rejected", "restored_archive_reuse")]
        data["observations"].append(dict(phase="distinct_case_sensitive_entries", loaded=True,
                                        actual_callback_data="ORIGINAL", error="", cache_exists=True,
                                        cache_files={".orca-wheel-cache.json": 50}))
        save()
        self.assertEqual(wheel_recovery_passed_report(report, binary)["processes"], 12)

    def test_wheel_report_rejects_changed_binary_fixture_result_and_missing_log(self):
        binary = self.wheel_artifact()
        report, value = self.wheel_report(binary)
        binary.write_bytes(b"different binary")
        with self.assertRaisesRegex(RuntimeError, "artifact changed"):
            wheel_recovery_passed_report(report, binary)
        for relative in ("fixtures/input.whl", "fixtures/manifest.json", "entry_crc.json"):
            report, value = self.wheel_report(binary)
            (report.parent / relative).write_bytes(b"changed bytes")
            with self.assertRaises(RuntimeError):
                wheel_recovery_passed_report(report, binary)
        report, value = self.wheel_report(binary)
        (report.parent / "entry_crc.log").unlink()
        with self.assertRaisesRegex(RuntimeError, "process evidence"):
            wheel_recovery_passed_report(report, binary)

    def test_wheel_dispatch_preserves_arguments_and_requires_report_after_zero_exit(self):
        binary = self.wheel_artifact()
        calls = []
        arguments = ["--native", str(binary), "--output", str(self.root / "wheel-cache-recovery-中文")]
        def run(label, command, environment, timeout):
            calls.append((label, command, timeout))
        with self.assertRaises(FileNotFoundError):
            run_wheel_recovery(run, self.source, arguments, binary, self.root, {})
        self.assertEqual(calls[0], ("native-wheel-cache-recovery",
                         [sys.executable, "-X", "utf8", str(self.source / "scripts/test_plugin_wheel_cache_recovery.py"), *arguments], 720))

    def test_real_process_failures_leave_filtered_logs(self):
        for name, command, timeout, expected in [
            ("exit", [sys.executable, "-c", "raise SystemExit(7)"], 10, 7),
            ("timeout", [sys.executable, "-c", "import time; time.sleep(10)"], 0.1, "timeout"),
            ("missing", [str(self.root / "missing program 中文")], 10, None),
        ]:
            with self.subTest(name=name):
                entry = run_logged_check(self.root, name, command, self.root, timeout=timeout)
                self.assertEqual(entry["exit_code"], expected)
                self.assertFalse(entry["passed"])
                self.assertTrue((self.root / (name + ".log")).is_file())
        self.assertIn("FileNotFoundError", (self.root / "missing.log").read_text(encoding="utf-8"))
        self.assertIn("time limit", (self.root / "timeout.log").read_text(encoding="utf-8"))

    def test_unicode_and_shell_metacharacters_remain_single_arguments(self):
        arguments = [str(self.root / "路径 with spaces"), "α & $(literal); 'quoted'", r"C:\工具 目录\python.exe"]
        command = [sys.executable, "-X", "utf8", "-c",
                   "import json,sys; print(json.dumps(sys.argv[1:], ensure_ascii=False))", *arguments]
        result = run_logged_check(self.root, "unicode", command, self.root)
        self.assertTrue(result["passed"])
        self.assertEqual(json.loads((self.root / "unicode.log").read_text(encoding="utf-8")), arguments)

    def test_missing_build_tool_is_recorded_in_the_gate_report(self):
        source = Path(__file__).resolve().parents[1]
        for build, mode in ((self.on, "ON"), (self.off, "OFF")):
            self.write(build, mode, {"CMAKE_HOME_DIRECTORY": str(source)})
        wheels = self.root / "wheels"
        wheels.mkdir()
        (wheels / "numpy-2.2.6-placeholder.whl").touch()
        output = self.root / "evidence"
        args = SimpleNamespace(on_build=self.on, off_build=self.off, config="Release", output=output,
                               wheelhouse=wheels, preflight_only=False, no_build=False, jobs=1, build_timeout=10)
        empty_tools = self.root / "empty tools"
        empty_tools.mkdir()
        # An empty PATH means cwd on POSIX; the real checkout contains a cmake/
        # directory, so name resolution would fail with EACCES instead of ENOENT.
        with patch.dict(os.environ, {"PATH": str(empty_tools)}):
            with self.assertRaisesRegex(RuntimeError, "build-off failed"):
                run_gate(args)
        report = json.loads((output / "results.json").read_text(encoding="utf-8"))
        self.assertFalse(report["passed"])
        self.assertEqual(len(report["checks"]), 1)
        self.assertEqual(report["checks"][0]["error"], "FileNotFoundError")
        self.assertFalse(report["checks"][0]["passed"])


if __name__ == "__main__":
    unittest.main()
