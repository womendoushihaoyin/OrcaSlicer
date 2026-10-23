#!/usr/bin/env python3
"""Collect real-host evidence and compare contracts; missing runs stay pending."""
from __future__ import annotations
import argparse
import collections
import hashlib
import json
from pathlib import Path
import re
import shutil

from prepare_plugin_dualhost import CAPABILITIES, FORMS, OPERATIONS, UPSTREAM


def read(path):
    return json.loads(path.read_text(encoding="utf-8"))


def write(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def gcode_summary(path):
    commands = hashlib.sha256()
    counts = collections.Counter()
    markers = []
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith("; ORCA_DUALHOST contract-v1 token="):
            markers.append(line)
        command = line.split(";", 1)[0].strip()
        if not command:
            continue
        command = " ".join(command.split())
        commands.update((command + "\n").encode("utf-8"))
        counts[command.split()[0]] += 1
    return {"bytes": path.stat().st_size, "sha256": digest(path), "command_sha256": commands.hexdigest(),
            "commands": dict(sorted(counts.items())), "markers": markers}


def evidence_file(value, base):
    path = Path(value)
    return (path if path.is_absolute() else base / path).resolve(strict=True)



def validated_manifest(path):
    manifest = read(path)
    if manifest.get("protocol") != "local-dualhost-v1" or manifest.get("upstream_commit") != UPSTREAM:
        raise ValueError("Unexpected package protocol or upstream revision")
    if manifest.get("required_operations") != OPERATIONS:
        raise ValueError("Run protocol operation sequences changed")
    expected = {label + "/" + capability + "/" + form
                for label, kinds in [(kind, [kind]) for kind in CAPABILITIES] + [("multi", CAPABILITIES)]
                for capability in kinds for form in FORMS}
    cases = manifest["cases"]
    if len(cases) != len(expected) or {case["id"] for case in cases} != expected:
        raise ValueError("Manifest must contain the complete 56-case local protocol")
    packages = {package["filename"]: package for package in manifest["packages"]}
    if len(packages) != 5:
        raise ValueError("Expected four single-capability wheels and one multi-capability wheel")
    for package in packages.values():
        name = package["filename"]
        if Path(name).name != name or digest(path.parent / name) != package["sha256"]:
            raise ValueError("Input wheel changed or invalid filename: " + name)
    for case in cases:
        label, capability, form = case["id"].split("/")
        if (case["capability"] != capability or case["form"] != form or
                case["package"] != "orca_dualhost_" + label + "-1.0.0-py3-none-any.whl" or
                capability not in packages[case["package"]]["capabilities"]):
            raise ValueError("Case identity/package mismatch: " + case["id"])
    mesh = manifest["reference_mesh"]
    if Path(mesh["filename"]).name != mesh["filename"] or digest(path.parent / mesh["filename"]) != mesh["sha256"]:
        raise ValueError("Reference input mesh changed")
    return manifest


def scaffold(args):
    manifest = validated_manifest(args.manifest)
    args.output.mkdir(parents=True, exist_ok=False)
    plan = {"schema": 1, "cases": []}
    binary_hash = digest(args.binary)
    for case in manifest["cases"]:
        folder = args.output / case["id"]
        folder.mkdir(parents=True)
        observation = {"execution": "gui-host", "binary_sha256": binary_hash,
            "operator_note": "Template only: mark outcomes and observations after the real actions, not before.",
            "operations": [{"action": action, "outcome": "pending", "pid": None}
                           for action in OPERATIONS[case["form"]]],
            "expected_token": 91 if case["form"] == "config-restart" else 37,
            "capability_enabled": None, "configuration": {}, "errors": [], "returns": [],
            "resource_consumer": {}, "artifacts": {}}
        write(folder / "host.json", observation)
        plan["cases"].append({"id": case["id"], "status": "pending", "traces": [],
                              "host_observation": case["id"] + "/host.json", "gcode": None})
    write(args.output / "plan.json", plan)
    print(json.dumps({"cases": len(plan["cases"]), "execution": "not-run", "plan": str(args.output / "plan.json")}))


def operations_complete(observation, form):
    operations = observation.get("operations", [])
    if [op.get("action") for op in operations] != OPERATIONS[form]:
        raise ValueError("Record the complete ordered operations from the local protocol")
    if any(op.get("outcome") not in ("ok", "error") for op in operations):
        return False
    for index, op in enumerate(operations):
        if type(op.get("pid")) is not int or op["pid"] <= 0:
            raise ValueError("Each completed operation must identify the actual host process")
        if op["action"] == "restart" and op["pid"] == operations[index - 1]["pid"]:
            raise ValueError("Restart must use a different real host process")
    return True


def read_traces(items, plan_dir, folder, module_hash):
    events, origins = [], []
    for index, item in enumerate(items):
        path = evidence_file(item["path"], plan_dir)
        lines = path.read_text(encoding="utf-8").splitlines()
        first, last = item.get("first_line", 1), item.get("last_line", len(lines))
        if (type(first) is not int or type(last) is not int or first < 1 or last < first - 1 or
                first > len(lines) + 1 or last > len(lines)):
            raise ValueError("Invalid inclusive trace line range")
        selected = lines[first - 1:last]
        copied = folder / ("trace-" + str(index + 1) + ".jsonl")
        copied.write_text("\n".join(selected) + ("\n" if selected else ""), encoding="utf-8")
        origins.append({"path": str(path), "sha256": digest(path), "first_line": first, "last_line": last})
        for line in selected:
            if not line.strip():
                continue
            event = json.loads(line)
            if (event.get("schema") != 1 or event.get("module_sha256") != module_hash or
                    event.get("capability") not in CAPABILITIES or type(event.get("pid")) is not int or event["pid"] <= 0 or
                    type(event.get("sequence")) is not int or event["sequence"] <= 0 or
                    not isinstance(event.get("event"), str) or not isinstance(event.get("payload"), dict) or
                    type(event.get("thread", {}).get("main")) is not bool):
                raise ValueError("Trace does not identify the expected installed module/process/schema")
            events.append(event)
    return events, origins


def contract_failures(entry, observation, events):
    failures = []
    for op in observation["operations"]:
        if op["outcome"] == "error" and op["action"] != "exercise_error":
            failures.append("Host operation failed: " + op["action"])
        if op["action"] == "exercise_error" and op["outcome"] != "error":
            failures.append("Deliberate error was accepted as success")
    names = {event["event"] for event in events}
    capability, form = entry["capability"], entry["form"]
    functional = {"return", "argument_error", "page_ui", "page_message", "page_ack", "slice", "gcode", "error"}
    if form == "installed-disabled":
        if observation.get("capability_enabled") is not False or any(event["event"] in functional for event in events):
            failures.append("Disabled capability remained usable or invoked functional callbacks")
        # Discovery may call ResourceTexture.get_texture_dir before a disabled
        # package is removed from active consumers. A directory query is not use.
        return failures
    if not events:
        failures.append("Exercise produced no capability callbacks")
    if capability == "script" and not {"return", "argument_error"}.issubset(names):
        failures.append("Missing execute result or invalid-enum TypeError")
    if capability == "pages" and (observation["execution"] != "gui-host" or "page_ack" not in names):
        failures.append("Missing real WebView round-trip acknowledgement")
    if capability == "texture":
        required = ("image_decoded", "preview_rendered", "mesh_changed", "undo_restored", "project_roundtrip")
        if observation["execution"] != "gui-host" or not all(observation.get("resource_consumer", {}).get(key) is True for key in required):
            failures.append("Resource decode/preview/apply/Undo/3MF consumption failed or was not demonstrated")
    if capability == "slicing" and not {"posSlice", "psGCodePostProcess"}.issubset(
            {event["payload"].get("step") for event in events if event["event"] == "slice"}):
        failures.append("Missing actual geometry/final-file callbacks")
    if form == "config-restart":
        if observation.get("configuration", {}).get("after_restart") != {"token": 91, "mode": "success"}:
            failures.append("Changed configuration was not restored after restart")
        if capability != "texture":
            restart_pid = next(op["pid"] for op in observation["operations"] if op["action"] == "restart")
            if not any(event["pid"] == restart_pid and event["event"] == "configuration" and
                       event["payload"].get("values", {}).get("token") == 91 for event in events):
                failures.append("Restarted host did not use the persisted token")
    if form == "error-recovery":
        if not observation.get("errors"):
            failures.append("No host-visible error recorded")
        if capability == "texture":
            if observation.get("resource_consumer", {}).get("invalid_image_rejected") is not True:
                failures.append("Invalid image rejection was not demonstrated")
        else:
            errors = [index for index, event in enumerate(events) if event["event"] == "error"]
            success = [index for index, event in enumerate(events) if event["event"] == "page_ack" or
                       (event["event"] == "return" and event["payload"].get("status") == "Success")]
            if not errors or not any(index > errors[-1] for index in success):
                failures.append("No successful use after the deliberate error")
    return failures

def collect(args):
    manifest = validated_manifest(args.manifest)
    plan = read(args.plan)
    if args.host == "upstream" and args.revision != UPSTREAM:
        raise ValueError("Upstream observations require the fixed commit, not another Orca build")
    if not args.binary.is_file():
        raise ValueError("Record the actual binary used, not a source tree or stub module")
    packages = {p["filename"]: p for p in manifest["packages"]}
    cases = {item["id"]: item for item in manifest["cases"]}
    supplied = plan.get("cases", [])
    if len({item["id"] for item in supplied}) != len(supplied) or any(item["id"] not in cases for item in supplied):
        raise ValueError("Duplicate or unknown case ID in run plan")
    args.output.mkdir(parents=True, exist_ok=False)
    result = {"schema": 1, "protocol": manifest["protocol"], "host": args.host,
              "revision_attested_by_operator": args.revision, "binary_sha256": digest(args.binary),
              "binary": str(args.binary.resolve()), "upstream_commit": UPSTREAM,
              "package_manifest_sha256": digest(args.manifest), "cases": [],
              "evidence_authority": "Recorded host artifacts and explicit operator observations; not source parity"}
    supplied = {item["id"]: item for item in supplied}
    for case_id, expected in cases.items():
        entry = dict(expected)
        entry["package_sha256"] = packages[entry["package"]]["sha256"]
        actual = supplied.get(case_id, {})
        entry["status"] = "pending"
        entry["reason"] = "No executed host observation"
        if actual.get("status") == "executed":
            folder = args.output / case_id
            folder.mkdir(parents=True)
            observation_path = evidence_file(actual["host_observation"], args.plan.parent)
            observation = read(observation_path)
            if observation.get("execution") not in ("gui-host", "native-host"):
                raise ValueError(case_id + ": source/static/fake records are not host execution")
            if observation.get("expected_token", 37) != (91 if entry["form"] == "config-restart" else 37):
                raise ValueError(case_id + ": token differs from the local acceptance protocol")
            if observation.get("binary_sha256") != result["binary_sha256"]:
                raise ValueError(case_id + ": observation belongs to a different host binary")
            if not isinstance(observation.get("errors", []), list) or any(
                    not isinstance(error, dict) or not error.get("type") for error in observation.get("errors", [])):
                raise ValueError(case_id + ": errors must record their actual type/status and diagnostic")
            shutil.copy2(observation_path, folder / "host.json")
            complete = operations_complete(observation, entry["form"])
            events, origins = read_traces(actual.get("traces", []), args.plan.parent, folder,
                                          packages[entry["package"]]["module_sha256"])
            events = [event for event in events if event["capability"] == entry["capability"]]
            entry.update(events=events, trace_sources=origins, host_observation=observation, status="observed", reason="")
            if not complete or not actual.get("traces"):
                entry.update(status="incomplete", reason="Ordered operations or captured trace window are incomplete")
            entry["contract_failures"] = contract_failures(entry, observation, events) if complete else []
            artifacts = {}
            for label, value in observation.get("artifacts", {}).items():
                if not re.fullmatch(r"[A-Za-z0-9_-]+", label):
                    raise ValueError("Invalid artifact label")
                path = evidence_file(value, observation_path.parent)
                target = folder / ("artifact-" + label + path.suffix)
                shutil.copy2(path, target)
                artifacts[label] = {"source": str(path), "sha256": digest(path), "bytes": path.stat().st_size}
            entry["artifacts"] = artifacts
            disabled = entry["form"] == "installed-disabled"
            if not disabled and entry["capability"] in ("slicing", "texture"):
                required = ("mesh", "resolved_config") if entry["capability"] == "slicing" else ("mesh", "preview", "project")
                if any(key not in artifacts for key in required):
                    entry.update(status="incomplete", reason="Missing actual input/consumer artifacts: " + ", ".join(required))
                elif artifacts["mesh"]["sha256"] != manifest["reference_mesh"]["sha256"]:
                    raise ValueError(case_id + ": the agreed reference mesh was not used")
            if not disabled and entry["capability"] == "slicing" and not actual.get("gcode"):
                entry.update(status="incomplete", reason="Missing actual exported G-code")
            if actual.get("gcode"):
                gcode = evidence_file(actual["gcode"], args.plan.parent)
                shutil.copy2(gcode, folder / "output.gcode")
                entry["gcode_summary"] = gcode_summary(gcode)
                if not disabled and entry["gcode_summary"]["markers"] != ["; ORCA_DUALHOST contract-v1 token=" +
                        str(observation.get("expected_token", 37))]:
                    entry["contract_failures"].append("Expected exactly one plugin footer in the real export")
        result["cases"].append(entry)
    result["counts"] = dict(collections.Counter(case["status"] for case in result["cases"]))
    write(args.output / "observations.json", result)
    print(json.dumps(result["counts"]))


def dimensions(case):
    result = collections.defaultdict(list)
    for event in case.get("events", []):
        name, payload = event["event"], dict(event["payload"])
        if name in ("load", "unload", "cancelled", "lifecycle"):
            # Preserve raw IDs/paths in evidence, compare callback order, code and
            # thread relation rather than host-generated identities or locations.
            for key in ("name", "id", "source"):
                payload.pop(key, None)
            result["lifecycle"].append({"event": name, "payload": payload, "main_thread": event["thread"]["main"]})
        elif name in ("error", "argument_error"):
            result["errors"].append({"event": name, "type": payload["type"]})
        else:
            category = {"return": "returns", "configuration": "configuration", "slice": "slicing",
                        "gcode": "gcode_callback", "resource_provider": "resource_provider"}.get(name, "pages")
            result[category].append({"event": name, "payload": payload})
    host = case.get("host_observation") or {}
    for key in ("returns", "resource_consumer", "capability_enabled", "configuration"):
        if key in host:
            result["host_" + key] = host[key]
    if "errors" in host:
        result["host_errors"] = [{key: error[key] for key in ("type", "status", "operation") if key in error}
                                 for error in host["errors"]]
    result["contract_failures"] = case.get("contract_failures", [])
    if "gcode_summary" in case:
        summary = case["gcode_summary"]
        result["gcode_commands"] = {key: summary[key] for key in ("command_sha256", "commands", "markers")}
    return dict(result)


def compare(args):
    manifest = validated_manifest(args.manifest)
    left = read(args.snapmaker) if args.snapmaker else {"host": "snapmaker", "cases": []}
    right = read(args.upstream) if args.upstream else {"host": "upstream", "cases": []}
    for expected_host, supplied_path, data in (("snapmaker", args.snapmaker, left), ("upstream", args.upstream, right)):
        if data.get("host") != expected_host:
            raise ValueError("Supply each real-host observation in its named position")
        if supplied_path:
            if data.get("protocol") != manifest["protocol"] or data.get("package_manifest_sha256") != digest(args.manifest):
                raise ValueError("Host evidence does not use this deterministic package set/protocol")
            ids = [case["id"] for case in data["cases"]]
            if len(ids) != len(set(ids)) or any(case_id not in {case["id"] for case in manifest["cases"]} for case_id in ids):
                raise ValueError("Duplicate or unknown case in collected evidence")
            if expected_host == "upstream" and data.get("revision_attested_by_operator") != UPSTREAM:
                raise ValueError("Upstream evidence is not the fixed revision")
    if args.snapmaker and args.upstream and left["binary_sha256"] == right["binary_sha256"]:
        raise ValueError("Both records name the same binary; this is not a dual-host comparison")
    a, b = ({case["id"]: case for case in data["cases"]} for data in (left, right))
    report = {"schema": 1, "upstream_commit": UPSTREAM, "cases": [], "passed": False,
              "host_evidence": {"snapmaker": str(args.snapmaker) if args.snapmaker else None,
                                "upstream": str(args.upstream) if args.upstream else None}}
    expected = {label + "/" + capability + "/" + form
                for label, kinds in [(kind, [kind]) for kind in CAPABILITIES] + [("multi", CAPABILITIES)]
                for capability in kinds for form in FORMS}
    for case_id in sorted(expected | set(a) | set(b)):
        entry = {"id": case_id, "status": "pending", "differences": []}
        x, y = a.get(case_id, {}), b.get(case_id, {})
        if x.get("status") == y.get("status") == "observed":
            if x["package_sha256"] != y["package_sha256"]:
                raise ValueError("Package digest differs for " + case_id)
            da, db = dimensions(x), dimensions(y)
            entry["differences"] = [{"dimension": key, "snapmaker": da.get(key), "upstream": db.get(key)}
                                    for key in sorted(set(da) | set(db)) if da.get(key) != db.get(key)]
            entry["status"] = "different" if entry["differences"] else "matched"
            if x.get("contract_failures") or y.get("contract_failures"):
                entry["status"] = "contract-failed"
                entry["contract_failures"] = {"snapmaker": x.get("contract_failures", []), "upstream": y.get("contract_failures", [])}
        else:
            entry["pending"] = {"snapmaker": x.get("reason", "Missing case"), "upstream": y.get("reason", "Missing case")}
        report["cases"].append(entry)
    report["counts"] = dict(collections.Counter(case["status"] for case in report["cases"]))
    report["passed"] = bool(report["cases"]) and all(case["status"] == "matched" for case in report["cases"])
    args.output.mkdir(parents=True, exist_ok=False)
    write(args.output / "comparison.json", report)
    lines = ["# Dual-host behavior comparison", "", "Fixed upstream: `" + UPSTREAM + "`", "",
             "| Case | Status | Differing dimensions |", "| --- | --- | --- |"]
    for case in report["cases"]:
        lines.append("| " + case["id"] + " | " + case["status"] + " | " +
                     ", ".join(item["dimension"] for item in case["differences"]) + " |")
    lines += ["", "Pending/source-only cases are never counted as matches. Raw diagnostics and host identities remain in collected evidence.",
              "Exact G-code file hashes remain in evidence; comparison excludes comments and checks executable commands separately."]
    (args.output / "comparison.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(json.dumps({"passed": report["passed"], "counts": report["counts"]}))
    return 0 if report["passed"] else 2


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    setup = commands.add_parser("scaffold")
    setup.add_argument("--manifest", type=Path, required=True)
    setup.add_argument("--binary", type=Path, required=True)
    setup.add_argument("--output", type=Path, required=True)
    take = commands.add_parser("collect")
    take.add_argument("--host", choices=("snapmaker", "upstream"), required=True)
    take.add_argument("--revision", required=True)
    take.add_argument("--binary", type=Path, required=True)
    take.add_argument("--manifest", type=Path, required=True)
    take.add_argument("--plan", type=Path, required=True)
    take.add_argument("--output", type=Path, required=True)
    diff = commands.add_parser("compare")
    diff.add_argument("--manifest", type=Path, required=True)
    diff.add_argument("--snapmaker", type=Path)
    diff.add_argument("--upstream", type=Path)
    diff.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        return {"scaffold": scaffold, "collect": collect, "compare": compare}[args.command](args)
    except (OSError, ValueError, KeyError, TypeError) as error:
        parser.exit(1, f"error: {error}\n")


if __name__ == "__main__":
    raise SystemExit(main())
