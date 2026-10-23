"""Utility protocol tests. Synthetic records below are NOT host execution evidence."""
import base64
import contextlib
import csv
import hashlib
import io
import json
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
import prepare_plugin_dualhost as build
import compare_plugin_dualhost as compare


class DualhostTools(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="dualhost-tool-contract-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name) / "工具-α"
        self.root.mkdir()
        self.packages = self.root / "packages"
        with contextlib.redirect_stdout(io.StringIO()):
            build.prepare(self.packages)
        self.manifest = self.packages / "manifest.json"
        self.binary = self.root / "synthetic-binary-tool-test-only"
        self.binary.write_bytes(b"Synthetic tool contract input, never executable or host evidence")

    def event(self, name, payload=None, pid=101):
        package = compare.read(self.manifest)["packages"][0]
        return {"schema": 1, "sequence": 1, "pid": pid, "module_sha256": package["module_sha256"],
                "capability": "script", "event": name, "thread": {"id": 1, "main": True}, "payload": payload or {}}

    def observation(self, form="fresh-load"):
        ops, pid = [], 101
        for action in build.OPERATIONS[form]:
            if action == "restart": pid += 1
            ops.append({"action": action, "outcome": "error" if action == "exercise_error" else "ok", "pid": pid})
        return {"execution": "gui-host", "binary_sha256": compare.digest(self.binary), "operations": ops,
                "errors": [], "artifacts": {}, "expected_token": 37}

    def test_deterministic_wheels_and_record_integrity(self):
        other = self.root / "again"
        with contextlib.redirect_stdout(io.StringIO()): build.prepare(other)
        self.assertEqual(self.manifest.read_bytes(), (other / "manifest.json").read_bytes())
        manifest = compare.validated_manifest(self.manifest)
        self.assertEqual(len(manifest["cases"]), 56)
        for package in manifest["packages"]:
            path = self.packages / package["filename"]
            self.assertEqual(path.read_bytes(), (other / path.name).read_bytes())
            with zipfile.ZipFile(path) as archive:
                record = next(name for name in archive.namelist() if name.endswith("/RECORD"))
                for name, encoded, size in csv.reader(io.StringIO(archive.read(record).decode())):
                    if not encoded: continue
                    data = archive.read(name)
                    self.assertEqual(int(size), len(data))
                    self.assertEqual(encoded, "sha256=" + base64.urlsafe_b64encode(hashlib.sha256(data).digest()).rstrip(b"=").decode())
        self.assertEqual((self.packages / "reference-cube.stl").stat().st_size, 84 + 12 * 50)

    def test_reject_changed_package_and_path_identity(self):
        manifest = compare.read(self.manifest)
        path = self.packages / manifest["packages"][0]["filename"]
        original = path.read_bytes(); path.write_bytes(original + b"changed")
        with self.assertRaisesRegex(ValueError, "Input wheel changed"): compare.validated_manifest(self.manifest)
        path.write_bytes(original)
        manifest["cases"][0]["id"] = "../../outside"
        compare.write(self.manifest, manifest)
        with self.assertRaisesRegex(ValueError, "56-case"): compare.validated_manifest(self.manifest)

    def test_ordered_operations_require_real_restart_pid(self):
        observation = self.observation("config-restart")
        self.assertTrue(compare.operations_complete(observation, "config-restart"))
        restart = next(op for op in observation["operations"] if op["action"] == "restart")
        restart["pid"] = 101
        with self.assertRaisesRegex(ValueError, "different real host"): compare.operations_complete(observation, "config-restart")
        observation["operations"][0]["outcome"] = "pending"
        self.assertFalse(compare.operations_complete(observation, "config-restart"))
        observation["operations"].pop()
        with self.assertRaisesRegex(ValueError, "complete ordered"): compare.operations_complete(observation, "config-restart")

    def test_disabled_metadata_is_not_functional_use(self):
        entry = {"capability": "texture", "form": "installed-disabled"}
        obs = self.observation("installed-disabled"); obs["capability_enabled"] = False
        self.assertFalse(compare.contract_failures(entry, obs, [self.event("resource_provider")]))
        self.assertTrue(compare.contract_failures(entry, obs, [self.event("return")]))
        next(op for op in obs["operations"] if op["action"] == "verify_disabled")["outcome"] = "error"
        self.assertTrue(compare.contract_failures(entry, obs, [self.event("resource_provider")]))

    def test_error_recovery_requires_success_after_error(self):
        entry = {"capability": "script", "form": "error-recovery"}
        obs = self.observation("error-recovery"); obs["errors"] = [{"type": "RuntimeError"}]
        success = self.event("return", {"status": "Success"})
        error = self.event("error", {"type": "RuntimeError"})
        args_error = self.event("argument_error", {"type": "TypeError"})
        self.assertFalse(compare.contract_failures(entry, obs, [error, args_error, success]))
        self.assertTrue(compare.contract_failures(entry, obs, [success, args_error, error]))

    def test_multisegment_trace_checks_module_and_line_range(self):
        output = self.root / "traces"; output.mkdir()
        first, second = self.root / "first.jsonl", self.root / "second.jsonl"
        first.write_text(json.dumps(self.event("load")) + "\n" + json.dumps(self.event("unload")) + "\n")
        second.write_text(json.dumps(self.event("load", pid=102)) + "\n")
        parts = [{"path": str(first), "first_line": 2, "last_line": 2}, {"path": str(second)}]
        events, origins = compare.read_traces(parts, self.root, output, self.event("load")["module_sha256"])
        self.assertEqual([event["event"] for event in events], ["unload", "load"])
        self.assertEqual([event["pid"] for event in events], [101, 102])
        self.assertEqual(len(origins), 2)
        with self.assertRaisesRegex(ValueError, "expected installed"): compare.read_traces(parts, self.root, output, "bad")
        parts[0]["last_line"] = 100
        with self.assertRaisesRegex(ValueError, "line range"): compare.read_traces(parts, self.root, output, self.event("load")["module_sha256"])

    def test_scaffold_collect_and_missing_upstream_remain_pending(self):
        run = self.root / "run"
        with contextlib.redirect_stdout(io.StringIO()):
            compare.scaffold(SimpleNamespace(manifest=self.manifest, binary=self.binary, output=run))
            compare.collect(SimpleNamespace(manifest=self.manifest, plan=run / "plan.json", host="snapmaker",
                revision="tool-unit-test-only", binary=self.binary, output=self.root / "collected"))
            status = compare.compare(SimpleNamespace(manifest=self.manifest,
                snapmaker=self.root / "collected/observations.json", upstream=None, output=self.root / "comparison"))
        self.assertEqual(status, 2)
        report = compare.read(self.root / "comparison/comparison.json")
        self.assertEqual(report["counts"], {"pending": 56})
        self.assertFalse(report["passed"])
        self.assertIsNone(report["host_evidence"]["upstream"])

    def test_collect_executed_result_and_reject_source_only(self):
        obs = self.observation()
        compare.write(self.root / "host.json", obs)
        trace = self.root / "trace.jsonl"
        trace.write_text("\n".join(json.dumps(event) for event in [self.event("argument_error", {"type": "TypeError"}),
            self.event("return", {"type": "ExecutionResult", "status": "Success"})]) + "\n")
        plan = {"cases": [{"id": "script/script/fresh-load", "status": "executed",
                            "host_observation": "host.json", "traces": [{"path": "trace.jsonl"}]}]}
        compare.write(self.root / "plan.json", plan)
        args = SimpleNamespace(manifest=self.manifest, plan=self.root / "plan.json", host="snapmaker",
            revision="tool-unit-test-only", binary=self.binary, output=self.root / "collected")
        with contextlib.redirect_stdout(io.StringIO()): compare.collect(args)
        case = compare.read(args.output / "observations.json")["cases"][0]
        self.assertEqual(case["status"], "observed"); self.assertFalse(case["contract_failures"])
        obs["execution"] = "source-only"; compare.write(self.root / "host.json", obs)
        args.output = self.root / "reject"
        with self.assertRaisesRegex(ValueError, "not host execution"): compare.collect(args)

    def test_compare_never_accepts_same_binary_or_both_failing(self):
        manifest = compare.read(self.manifest)
        case = dict(manifest["cases"][0], status="observed", package_sha256=manifest["packages"][0]["sha256"],
                    contract_failures=["Missing execute result"])
        base = {"protocol": manifest["protocol"], "package_manifest_sha256": compare.digest(self.manifest),
                "binary_sha256": "a", "cases": [case]}
        left, right = self.root / "left.json", self.root / "right.json"
        compare.write(left, dict(base, host="snapmaker"))
        compare.write(right, dict(base, host="upstream", revision_attested_by_operator=build.UPSTREAM))
        args = SimpleNamespace(manifest=self.manifest, snapmaker=left, upstream=right, output=self.root / "comparison")
        with self.assertRaisesRegex(ValueError, "same binary"): compare.compare(args)
        record = compare.read(right); record["binary_sha256"] = "b"; compare.write(right, record)
        with contextlib.redirect_stdout(io.StringIO()): self.assertEqual(compare.compare(args), 2)
        self.assertEqual(compare.read(args.output / "comparison.json")["counts"], {"pending": 55, "contract-failed": 1})

    def test_gcode_command_comparison_keeps_raw_hash_and_real_marker(self):
        a, b = self.root / "one.gcode", self.root / "two.gcode"
        a.write_text("; host A\nG1 X1 E2 ; comment\n; ORCA_DUALHOST contract-v1 token=37\n")
        b.write_text("; host B\nG1   X1 E2\n; ORCA_DUALHOST contract-v1 token=37\n")
        x, y = compare.gcode_summary(a), compare.gcode_summary(b)
        self.assertNotEqual(x["sha256"], y["sha256"])
        self.assertEqual(x["command_sha256"], y["command_sha256"])
        b.write_text(b.read_text().replace("E2", "E3"))
        self.assertNotEqual(x["command_sha256"], compare.gcode_summary(b)["command_sha256"])


if __name__ == "__main__":
    unittest.main()
