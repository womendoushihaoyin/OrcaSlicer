#!/usr/bin/env python3
"""Synthetic evidence-tool contracts only; no fake host execution is claimed."""
import contextlib
import copy
import io
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest

import aggregate_plugin_dualhost as aggregate

REPO = Path(sys.argv.pop(1)).resolve() if len(sys.argv) > 1 and not sys.argv[1].startswith('-') else Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "scripts"))
import compare_plugin_dualhost as collector
from prepare_plugin_dualhost import prepare, OPERATIONS


class Contracts(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="aggregate-tool-contract-中文-")
        self.root = Path(self.temp.name)
        self.binary = self.root / "synthetic-tool-input.bin"
        self.binary.write_bytes(b"Evidence-tool test input; not a host binary.")
        with contextlib.redirect_stdout(io.StringIO()):
            prepare(self.root / "canonical")
            collector.scaffold(SimpleNamespace(manifest=self.root / "canonical/manifest.json", binary=self.binary, output=self.root / "plan"))
        self.manifest = aggregate.read(self.root / "canonical/manifest.json")
        self.case = "script/script/fresh-load"
        package = next(item for item in self.manifest["packages"] if item["capabilities"] == ["script"])
        self.trace = self.root / "synthetic-callbacks.jsonl"
        import json
        events = [{"schema": 1, "sequence": index + 1, "pid": 1234, "module_sha256": package["module_sha256"],
                   "capability": "script", "event": name, "thread": {"id": 1, "main": True}, "payload": payload}
                  for index, (name, payload) in enumerate([("argument_error", {"type": "TypeError", "message": "synthetic tool record"}),
                      ("return", {"status": "Success", "type": "ExecutionResult", "message": "synthetic", "data": ""})])]
        self.trace.write_text("".join(json.dumps(event) + "\n" for event in events), encoding="utf-8")
        host = aggregate.read(self.root / "plan" / self.case / "host.json")
        host.update(execution="native-host", capability_enabled=True,
                    operations=[{"action": action, "outcome": "ok", "pid": 1234} for action in OPERATIONS["fresh-load"]])
        aggregate.write(self.root / "plan" / self.case / "host.json", host)
        plan = aggregate.read(self.root / "plan/plan.json")
        entry = next(case for case in plan["cases"] if case["id"] == self.case)
        entry.update(status="executed", traces=[{"path": str(self.trace)}])
        aggregate.write(self.root / "plan/plan.json", plan)
        with contextlib.redirect_stdout(io.StringIO()):
            collector.collect(SimpleNamespace(host="snapmaker", revision="synthetic evidence-tool contract only", binary=self.binary,
                              manifest=self.root / "canonical/manifest.json", plan=self.root / "plan/plan.json", output=self.root / "report"))
        self.report_path = self.root / "report/observations.json"
        self.report = aggregate.read(self.report_path)
        self.entry = next(case for case in self.report["cases"] if case["id"] == self.case)

    def tearDown(self):
        self.temp.cleanup()

    def run_aggregate(self, reports=None):
        with contextlib.redirect_stdout(io.StringIO()):
            return aggregate.aggregate(SimpleNamespace(repo=REPO, manifest=self.root / "canonical/manifest.json",
                                       report=reports or [self.report_path], output=self.root / "aggregate"))

    def save(self):
        aggregate.write(self.report_path, self.report)

    def test_single_side_stays_pending_and_keeps_exact_provenance(self):
        self.assertEqual(self.run_aggregate(), 2)
        result = aggregate.read(self.root / "aggregate/aggregate.json")
        self.assertEqual(result["counts"], {"observed": 1, "pending": 55})
        self.assertFalse(result["coverage_complete"])
        self.assertEqual(result["cross_host_status"], "pending")
        self.assertNotIn("binary_sha256", result)
        row = next(case for case in result["cases"] if case["id"] == self.case)
        self.assertEqual(row["provenance"][0]["binary_sha256"], aggregate.sha(self.binary))
        self.assertEqual(row["provenance"][0]["report_sha256"], aggregate.sha(self.report_path))

    def test_identical_duplicate_is_not_counted_twice(self):
        self.assertEqual(self.run_aggregate([self.report_path, self.report_path]), 2)
        result = aggregate.read(self.root / "aggregate/aggregate.json")
        self.assertEqual(result["counts"]["observed"], 1)
        row = next(case for case in result["cases"] if case["id"] == self.case)
        self.assertEqual(len(row["provenance"]), 2)

    def test_contradictory_overlap_with_another_binary_rejected(self):
        second = self.root / "second-synthetic.bin"
        second.write_bytes(b"Another synthetic evidence-tool input, not a host.")
        host_path = self.root / "plan" / self.case / "host.json"
        host = aggregate.read(host_path)
        host["binary_sha256"] = aggregate.sha(second)
        aggregate.write(host_path, host)
        with contextlib.redirect_stdout(io.StringIO()):
            collector.collect(SimpleNamespace(host="snapmaker", revision="synthetic second record", binary=second,
                              manifest=self.root / "canonical/manifest.json", plan=self.root / "plan/plan.json", output=self.root / "second-report"))
        with self.assertRaisesRegex(ValueError, "contradictory overlapping"):
            self.run_aggregate([self.report_path, self.root / "second-report/observations.json"])
        self.assertFalse((self.root / "aggregate").exists())

    def test_disjoint_cases_keep_different_binary_provenance(self):
        import json
        second = self.root / "second-synthetic.bin"
        second.write_bytes(b"Second evidence-tool input; never executed as a host.")
        case_id = "multi/script/fresh-load"
        with contextlib.redirect_stdout(io.StringIO()):
            collector.scaffold(SimpleNamespace(manifest=self.root / "canonical/manifest.json", binary=second, output=self.root / "second-plan"))
        second_trace = self.root / "second-synthetic-trace.jsonl"
        module = next(item for item in self.manifest["packages"] if len(item["capabilities"]) == 4)["module_sha256"]
        events = [json.loads(line) for line in self.trace.read_text().splitlines()]
        for event in events:
            event["module_sha256"] = module
        second_trace.write_text("".join(json.dumps(event) + "\n" for event in events), encoding="utf-8")
        host = copy.deepcopy(self.entry["host_observation"])
        host["binary_sha256"] = aggregate.sha(second)
        aggregate.write(self.root / "second-plan" / case_id / "host.json", host)
        plan = aggregate.read(self.root / "second-plan/plan.json")
        next(case for case in plan["cases"] if case["id"] == case_id).update(status="executed", traces=[{"path": str(second_trace)}])
        aggregate.write(self.root / "second-plan/plan.json", plan)
        with contextlib.redirect_stdout(io.StringIO()):
            collector.collect(SimpleNamespace(host="snapmaker", revision="synthetic second tool record", binary=second,
                              manifest=self.root / "canonical/manifest.json", plan=self.root / "second-plan/plan.json", output=self.root / "second-report"))
        self.assertEqual(self.run_aggregate([self.report_path, self.root / "second-report/observations.json"]), 2)
        result = aggregate.read(self.root / "aggregate/aggregate.json")
        self.assertEqual(result["counts"]["observed"], 2)
        hashes = {case["provenance"][0]["binary_sha256"] for case in result["cases"] if case["status"] == "observed"}
        self.assertEqual(hashes, {aggregate.sha(second), aggregate.sha(self.binary)})
        self.assertNotIn("binary_sha256", result)

    def test_changed_canonical_wheel_rejected(self):
        wheel = self.root / "canonical" / self.entry["package"]
        with wheel.open("ab") as stream:
            stream.write(b"changed")
        with self.assertRaisesRegex(ValueError, "Input wheel changed"):
            self.run_aggregate()

    def test_changed_actual_artifact_rejected(self):
        source = self.root / "diagnostic.txt"
        source.write_text("synthetic tool diagnostic", encoding="utf-8")
        copied = self.root / "report" / self.case / "artifact-diagnostic.txt"
        copied.write_bytes(source.read_bytes())
        self.entry["host_observation"]["artifacts"]["diagnostic"] = str(source)
        aggregate.write(self.root / "report" / self.case / "host.json", self.entry["host_observation"])
        self.entry["artifacts"]["diagnostic"] = {"source": str(source), "bytes": source.stat().st_size, "sha256": aggregate.sha(source)}
        self.save()
        source.write_text("changed", encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "artifact changed"):
            self.run_aggregate()

    def test_changed_manifest_reference_rejected(self):
        self.report["package_manifest_sha256"] = "0" * 64
        self.save()
        with self.assertRaisesRegex(ValueError, "changed manifest"):
            self.run_aggregate()

    def test_missing_contract_verdict_rejected(self):
        del self.entry["contract_failures"]
        self.save()
        with self.assertRaisesRegex(ValueError, "missing or failed contract"):
            self.run_aggregate()

    def test_failed_contract_cannot_count_as_observed(self):
        self.entry["contract_failures"] = ["actual recorded failure"]
        self.save()
        with self.assertRaisesRegex(ValueError, "missing or failed contract"):
            self.run_aggregate()

    def test_changed_executable_rejected(self):
        self.binary.write_bytes(b"changed")
        with self.assertRaisesRegex(ValueError, "executable changed"):
            self.run_aggregate()

    def test_missing_lifecycle_operation_rejected(self):
        self.entry["host_observation"]["operations"].pop()
        self.save()
        with self.assertRaisesRegex(ValueError, "complete ordered operations"):
            self.run_aggregate()

    def test_original_callback_trace_change_rejected(self):
        self.trace.write_text("changed\n", encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "original callback trace changed"):
            self.run_aggregate()

    def test_embedded_callback_change_rejected(self):
        self.entry["events"][0]["payload"]["type"] = "ChangedError"
        self.save()
        with self.assertRaisesRegex(ValueError, "embedded callback events changed"):
            self.run_aggregate()

    def test_missing_case_identity_rejected(self):
        self.report["cases"].pop()
        self.save()
        with self.assertRaisesRegex(ValueError, "duplicate/missing/unknown"):
            self.run_aggregate()

    def test_incomplete_attempt_does_not_become_observed(self):
        self.entry["status"] = "incomplete"
        self.entry["reason"] = "synthetic missing consumer artifact"
        self.report["counts"] = {"pending": 55, "incomplete": 1}
        self.save()
        self.assertEqual(self.run_aggregate(), 2)
        result = aggregate.read(self.root / "aggregate/aggregate.json")
        self.assertEqual(result["counts"], {"pending": 56})
        row = next(case for case in result["cases"] if case["id"] == self.case)
        self.assertEqual(len(row["attempts"]), 1)

    def test_changed_package_hash_rejected(self):
        self.entry["package_sha256"] = "0" * 64
        self.save()
        with self.assertRaisesRegex(ValueError, "package hash changed"):
            self.run_aggregate()

    def test_missing_trace_windows_rejected(self):
        self.entry["trace_sources"] = []
        self.save()
        with self.assertRaisesRegex(ValueError, "missing captured callback trace"):
            self.run_aggregate()

    def test_upstream_cannot_be_mislabeled_as_snapmaker(self):
        self.report["host"] = "upstream"
        self.save()
        with self.assertRaisesRegex(ValueError, "Snapmaker reports only"):
            self.run_aggregate()


if __name__ == "__main__":
    unittest.main()
