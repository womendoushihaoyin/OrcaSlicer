#!/usr/bin/env python3
"""Aggregate real Snapmaker collector reports without inventing a shared binary.

Exit 0 means complete *Snapmaker-only* coverage. Exit 2 means pending cases.
Neither result is a cross-host compatibility verdict; fixed upstream stays pending.
"""
from __future__ import annotations
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import re
import sys
import tempfile


def read(path):
    return json.loads(path.read_text(encoding="utf-8"))


def write(path, data):
    path.write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def sha(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def require(value, message):
    if not value:
        raise ValueError(message)


def canonical_hash(value):
    return hashlib.sha256(json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":")).encode()).hexdigest()


def validate_case(collector, expected, entry, report, report_path, package, manifest):
    case_id = expected["id"]
    prefix = case_id + ": "
    require(entry.get("contract_failures") == [], prefix + "missing or failed contract verdict")
    observation = entry.get("host_observation")
    require(isinstance(observation, dict), prefix + "missing host observation")
    require(observation.get("execution") in ("native-host", "gui-host"), prefix + "not a real host execution record")
    require(observation.get("binary_sha256") == report["binary_sha256"], prefix + "per-case binary differs from its source report")
    expected_token = 91 if expected["form"] == "config-restart" else 37
    require(observation.get("expected_token", 37) == expected_token, prefix + "configuration token differs from the protocol")
    require(collector.operations_complete(observation, expected["form"]), prefix + "ordered lifecycle operations are incomplete")
    folder = report_path.parent / case_id
    require(read(folder / "host.json") == observation, prefix + "copied host evidence changed")
    sources = entry.get("trace_sources")
    require(isinstance(sources, list) and sources, prefix + "missing captured callback trace windows")
    for source in sources:
        path = Path(source["path"])
        require(path.is_absolute() and path.is_file() and sha(path) == source["sha256"], prefix + "original callback trace changed")
    with tempfile.TemporaryDirectory(prefix="dualhost-trace-verify-") as temp:
        destination = Path(temp)
        events, origins = collector.read_traces(sources, report_path.parent, destination, package["module_sha256"])
        for index in range(len(sources)):
            name = "trace-" + str(index + 1) + ".jsonl"
            require((folder / name).read_bytes() == (destination / name).read_bytes(), prefix + "collector trace copy differs from its original window")
    events = [event for event in events if event["capability"] == expected["capability"]]
    require(events == entry.get("events"), prefix + "embedded callback events changed")
    require(origins == sources, prefix + "trace provenance is incomplete or changed")
    failures = collector.contract_failures(expected, observation, events)
    require(not failures, prefix + "contract revalidation failed: " + "; ".join(failures))
    artifacts = entry.get("artifacts")
    require(isinstance(artifacts, dict), prefix + "missing artifact inventory")
    for label, item in artifacts.items():
        require(re.fullmatch(r"[A-Za-z0-9_-]+", label), prefix + "invalid artifact label")
        source = Path(item["source"])
        copied = folder / ("artifact-" + label + source.suffix)
        require(source.is_file() and copied.is_file(), prefix + "artifact is missing: " + label)
        require(source.stat().st_size == item["bytes"] and copied.stat().st_size == item["bytes"] and
                sha(source) == item["sha256"] and sha(copied) == item["sha256"], prefix + "artifact changed: " + label)
    disabled = expected["form"] == "installed-disabled"
    if not disabled and expected["capability"] in ("texture", "slicing"):
        required = ("mesh", "preview", "project") if expected["capability"] == "texture" else ("mesh", "resolved_config")
        require(all(key in artifacts for key in required), prefix + "missing required consumer artifacts")
        require(artifacts["mesh"]["sha256"] == manifest["reference_mesh"]["sha256"], prefix + "canonical mesh changed")
    if "gcode_summary" in entry:
        actual = collector.gcode_summary(folder / "output.gcode")
        require(actual == entry["gcode_summary"], prefix + "actual G-code differs from its recorded summary")
    if not disabled and expected["capability"] == "slicing":
        require("gcode_summary" in entry, prefix + "missing actual exported G-code")
        require(entry["gcode_summary"]["markers"] == ["; ORCA_DUALHOST contract-v1 token=" + str(expected_token)],
                prefix + "canonical final-file callback footer is missing/duplicated")
    # Keep identity/provenance in the fingerprint: differing observations for a
    # repeated case require operator resolution, never silent last-writer-wins.
    return canonical_hash(entry)


def aggregate(args):
    sys.path.insert(0, str(args.repo.resolve(strict=True) / "scripts"))
    import compare_plugin_dualhost as collector
    manifest_path = args.manifest.resolve(strict=True)
    manifest = collector.validated_manifest(manifest_path)
    manifest_sha = sha(manifest_path)
    packages = {item["filename"]: item for item in manifest["packages"]}
    expected = {item["id"]: item for item in manifest["cases"]}
    rows = {key: {"id": key, "status": "pending", "provenance": [], "attempts": []} for key in expected}
    sources = []
    fingerprints = {}
    for supplied in args.report:
        path = supplied.resolve(strict=True)
        report_sha = sha(path)
        report = read(path)
        require(report.get("schema") == 1 and report.get("protocol") == manifest["protocol"], "Unexpected collector report schema/protocol")
        require(report.get("host") == "snapmaker", "This aggregate accepts Snapmaker reports only; upstream must remain separate")
        require(report.get("upstream_commit") == manifest["upstream_commit"] and report.get("package_manifest_sha256") == manifest_sha,
                "Collector report references a changed manifest/upstream revision")
        binary = Path(report["binary"])
        require(binary.is_absolute() and binary.is_file() and sha(binary) == report["binary_sha256"], "Recorded host executable changed or is missing")
        cases = report.get("cases", [])
        require(len(cases) == len(expected) and {item["id"] for item in cases} == set(expected), "Collector report has duplicate/missing/unknown case identities")
        require(report.get("counts") == dict(Counter(item["status"] for item in cases)), "Collector status counts are inconsistent")
        source = {"report": str(path), "report_sha256": report_sha, "binary": str(binary),
                  "binary_sha256": report["binary_sha256"], "revision_attested_by_operator": report.get("revision_attested_by_operator", "")}
        sources.append(source)
        for entry in cases:
            case_id = entry["id"]
            spec = expected[case_id]
            require(all(entry.get(key) == spec[key] for key in ("package", "capability", "form")), case_id + ": case identity changed")
            require(entry.get("package_sha256") == packages[spec["package"]]["sha256"], case_id + ": package hash changed")
            status = entry.get("status")
            require(status in ("pending", "incomplete", "observed"), case_id + ": unknown collector state")
            if status != "observed":
                if status == "incomplete":
                    rows[case_id]["attempts"].append({**source, "status": status, "reason": entry.get("reason", "")})
                continue
            fingerprint = validate_case(collector, spec, entry, report, path, packages[spec["package"]], manifest)
            if case_id in fingerprints:
                require(fingerprints[case_id] == (fingerprint, report["binary_sha256"]),
                        case_id + ": contradictory overlapping evidence; select/resolve the source reports explicitly")
            else:
                fingerprints[case_id] = (fingerprint, report["binary_sha256"])
            rows[case_id].update(status="observed", contract_failures=[], package_sha256=entry["package_sha256"],
                                 observation_sha256=fingerprint)
            rows[case_id]["provenance"].append({**source, "case": case_id})
        require(sha(path) == report_sha and sha(binary) == report["binary_sha256"], "Input report/binary changed during aggregation")
    require(sha(manifest_path) == manifest_sha, "Manifest changed during aggregation")
    counts = dict(Counter(row["status"] for row in rows.values()))
    complete = counts.get("observed", 0) == len(expected)
    result = {"schema": 1, "protocol": "local-dualhost-case-aggregate-v1", "package_protocol": manifest["protocol"],
              "host": "snapmaker", "package_manifest": str(manifest_path), "package_manifest_sha256": manifest_sha,
              "upstream_commit": manifest["upstream_commit"], "upstream_status": "pending", "cross_host_status": "pending",
              "coverage_complete": complete, "counts": counts, "sources": sources, "cases": list(rows.values()),
              "binary_policy": "Per-case report/binary provenance; no synthesized common executable hash",
              "scope": "Revalidated recorded Snapmaker executions only. This is not an upstream compatibility PASS."}
    args.output.mkdir(parents=True, exist_ok=False)
    write(args.output / "aggregate.json", result)
    lines = ["# Canonical Snapmaker case coverage", "",
             f"Observed {counts.get('observed', 0)}/{len(expected)}; pending {counts.get('pending', 0)}.", "",
             "Fixed upstream execution and cross-host comparison remain **pending**. Executables are recorded per case; no common binary is claimed.", "",
             "| Case | Status | Original report / executable SHA |", "| --- | --- | --- |"]
    for row in rows.values():
        evidence = "; ".join("[report](<" + item["report"] + ">) / `" + item["binary_sha256"] + "`" for item in row["provenance"]) or "—"
        lines.append("| " + row["id"] + " | " + row["status"] + " | " + evidence + " |")
    (args.output / "aggregate.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(json.dumps({"counts": counts, "coverage_complete": complete, "upstream": "pending", "output": str(args.output)}, ensure_ascii=False))
    return 0 if complete else 2


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--report", type=Path, action="append", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        return aggregate(args)
    except (ValueError, KeyError, TypeError, OSError) as error:
        parser.exit(1, "Canonical aggregate rejected: " + str(error) + "\n")


if __name__ == "__main__":
    sys.exit(main())
