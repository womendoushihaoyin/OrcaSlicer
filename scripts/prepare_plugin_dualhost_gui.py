#!/usr/bin/env python3
"""Prepare/finalize canonical GUI cases. Never launch a GUI or fabricate host observations.

Run one fresh-load case first. A GUI operator launches each listed bundle and waits
for its actual OnExit marker before `finish`; restart uses a second real process.
"""
from __future__ import annotations
import argparse
from datetime import datetime
import os
import re
import time
import hashlib
import json
import plistlib
from pathlib import Path
import shutil
import subprocess
import sys
from types import SimpleNamespace


def read(path):
    return json.loads(Path(path).read_text(encoding="utf-8"))


def write(path, value):
    Path(path).write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def sha(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def tools(repo):
    sys.path.insert(0, str(repo / "scripts"))
    import compare_plugin_dualhost
    return compare_plugin_dualhost


def pid_present(pid):
    if os.name != "posix":
        raise ValueError("Read-only process-exit verification is not implemented on this platform")
    try:
        os.kill(pid, 0)  # POSIX existence probe only; never signals/terminates.
    except ProcessLookupError:
        return False
    except PermissionError as error:
        raise ValueError("Cannot verify this process has exited") from error
    return True


def crash_timestamp(value):
    if not isinstance(value, str):
        return None
    try:
        return datetime.fromisoformat(value.replace("Z", "+00:00")).timestamp()
    except ValueError:
        return None


def matching_crash(pid, start, end, directory):
    # Read only this executable's recent metadata. Do not deserialize or print
    # unrelated stack/thread/memory details from DiagnosticReports.
    for path in directory.glob("Snapmaker_Orca-*.ips"):
        if path.stat().st_mtime < start - 60:
            continue
        with path.open("r", encoding="utf-8", errors="replace") as stream:
            first = stream.readline(65536)
            try:
                header = json.loads(first)
            except ValueError as error:
                raise ValueError("Cannot verify recent crash metadata: " + path.name) from error
            captured = crash_timestamp(header.get("timestamp"))
            if captured is not None and not start - 60 <= captured <= end + 60:
                continue
            report_pid = header.get("pid")
            consumed = len(first)
            while report_pid is None and consumed < 65536:
                line = stream.readline(65536 - consumed)
                if not line:
                    break
                consumed += len(line)
                match = re.match(r'^ {0,2}"pid"\s*:\s*(\d+)', line)
                if match:
                    report_pid = int(match.group(1))
                match = re.match(r'^ {0,2}"captureTime"\s*:\s*"([^"\n]+)"', line)
                if match:
                    captured = crash_timestamp(match.group(1)) or captured
            if report_pid is None:
                raise ValueError("Cannot identify PID in recent crash metadata: " + path.name)
            if report_pid == pid and (captured is None or start - 60 <= captured <= end + 60):
                return path.name
    return None


def verify_process_exit(pid, start, end=None, crash_directory=None):
    if pid_present(pid):
        raise ValueError(f"Test PID {pid} is still present; OnExit is not proof of process exit")
    end = time.time() if end is None else end
    directory = crash_directory or Path.home() / "Library/Logs/DiagnosticReports"
    if sys.platform == "darwin":
        if not directory.is_dir():
            raise ValueError("macOS DiagnosticReports directory is unavailable; exit-crash verification is incomplete")
        crashed = matching_crash(pid, start, end, directory)
        if crashed:
            raise ValueError(f"Test PID {pid} has a macOS crash report: {crashed}")
    return {"pid_absent": True, "checked_at_unix": time.time(), "window_start_unix": start,
            "window_end_unix": end, "macos_crash_checked": sys.platform == "darwin"}


def safe_case(run, case_id):
    cases = {item["id"]: item for item in run["cases"]}
    if case_id not in cases:
        raise ValueError("Case was not prepared: " + case_id)
    return cases[case_id]


def group_gui_cases(eligible, selected):
    groups = {}
    for case in eligible:
        if case["id"] in selected:
            label, capability, form = case["id"].split("/")
            groups.setdefault((label, form), []).append(case)
    return groups


def prepare(args):
    if sys.platform != "darwin":
        raise ValueError("This preparer is for the local macOS GUI host; Windows/Linux GUI execution remains unverified")
    repo = args.repo.resolve(strict=True)
    collector = tools(repo)
    manifest_path = args.manifest.resolve(strict=True)
    manifest = collector.validated_manifest(manifest_path)
    eligible = [case for case in manifest["cases"] if case["capability"] in ("pages", "texture")]
    ids = {case["id"] for case in eligible}
    selected = ids if args.all else set(args.case or ["pages/pages/fresh-load"])
    if selected - ids:
        raise ValueError("Only canonical Pages/Texture cases are accepted: " + ", ".join(sorted(selected - ids)))
    out = args.output.absolute()
    out.mkdir(parents=True, exist_ok=False)
    packages = out / "canonical"
    packages.mkdir()
    for filename in ["manifest.json", manifest["reference_mesh"]["filename"]] + [item["filename"] for item in manifest["packages"]]:
        shutil.copy2(manifest_path.parent / filename, packages / filename)
    collector.validated_manifest(packages / "manifest.json")
    source_binary = args.binary.resolve(strict=True)
    source_sha = sha(source_binary)
    fixed = out / "source-binary"
    shutil.copy2(source_binary, fixed)
    if sha(fixed) != source_sha or sha(source_binary) != source_sha:
        raise ValueError("Source binary changed while copying; wait for the build to finish")
    seed_root = out / "seed"
    command = [sys.executable, str(args.smoke_preparer or repo / "scripts/prepare_local_plugin_gui_smoke.py"),
               "--binary", str(fixed), "--python-root", str(args.python_root.resolve(strict=True)),
               "--uv-executable", str(args.uv_executable.resolve(strict=True)), "--output-dir", str(seed_root),
               "--mode", "dualhost", "--fixture", str(packages / "manifest.json")]
    with (out / "prepare-seed.log").open("w", encoding="utf-8") as log:
        subprocess.run(command, check=True, stdout=log, stderr=subprocess.STDOUT)
    seed = seed_root / "LocalPluginSmoke-dualhost.app"
    binary = seed / "Contents/MacOS/Snapmaker_Orca"
    binary_sha = sha(binary)
    # Sign once, then copy the exact executable bytes to every disposable app.
    # Signing each bundle ID separately would create incompatible evidence hashes.
    collector.scaffold(SimpleNamespace(manifest=packages / "manifest.json", binary=binary, output=out / "plan"))
    run = {"schema": 1, "protocol": "local-dualhost-v1", "repo": str(repo), "cases": [], "launch_groups": [],
           "source_binary": str(source_binary), "source_binary_sha256": source_sha,
           "binary": str(binary), "binary_sha256": binary_sha,
           "manifest": str(packages / "manifest.json"), "manifest_sha256": sha(packages / "manifest.json"),
           "upstream": "pending", "execution": "not-run", "gui_launched_by_preparer": False}
    groups = group_gui_cases(eligible, selected)
    for (label, form), group_cases in groups.items():
        capabilities = [case["capability"] for case in group_cases]
        folder = out / "cases" / label / form
        folder.mkdir(parents=True)
        profile = folder / "data_dir"
        shutil.copytree(seed_root / "data_dir", profile)
        # Generate a clean U1 setup, rather than copying any user's account,
        # devices, recent files, plugins or token-bearing configuration.
        config = read(profile / "Snapmaker_Orca.conf")
        config["models"] = [{"model": "Snapmaker U1", "nozzle_diameter": "0.4", "vendor": "Snapmaker"}]
        config["presets"] = {"machine": "Snapmaker U1 (0.4 nozzle)"}
        config["app"].update(show_home_page=False, default_page="3D", background_processing="0")
        write(profile / "Snapmaker_Orca.conf", config)
        phases = ["first", "resume"] if form in ("installed-disabled", "config-restart") else ["first"]
        item = {"id": label + "/" + form, "case_ids": [case["id"] for case in group_cases],
                "capabilities": capabilities, "phases": [], "status": "prepared"}
        for phase in phases:
            app = folder / (phase + ".app")
            # APFS clone of the prepared app: no source mutation or extra signing.
            subprocess.run(["/bin/cp", "-cR", str(seed), str(app)], check=True)
            copied_binary = app / "Contents/MacOS/Snapmaker_Orca"
            if sha(copied_binary) != binary_sha:
                raise ValueError("Prepared app does not contain the fixed executable")
            report = folder / (phase + "-report.json")
            request = folder / (phase + "-request.json")
            write(request, {"schema": 1, "protocol": "local-dualhost-v1", "label": label,
                  "capabilities": capabilities, "form": form, "phase": phase, "packages": str(packages),
                  "output": str(folder), "profile": str(profile)})
            with (app / "Contents/Info.plist").open("rb") as stream:
                info = plistlib.load(stream)
            bundle = "com.snapmaker.codex.dualhost.r" + hashlib.sha256(str(app).encode()).hexdigest()[:20]
            info["CFBundleIdentifier"] = bundle
            info["CFBundleName"] = "Canonical " + label + " " + phase
            info["LSEnvironment"] = {"ORCA_LOCAL_PLUGIN_SMOKE_FILE": str(request),
                                    "ORCA_LOCAL_PLUGIN_SMOKE_REPORT": str(report),
                                    "ORCA_LOCAL_PLUGIN_SMOKE_MODE": "dualhost",
                                    "ORCA_LOCAL_PLUGIN_SMOKE_DATADIR": str(profile)}
            with (app / "Contents/Info.plist").open("wb") as stream:
                plistlib.dump(info, stream)
            subprocess.run(["/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister", "-f", str(app)], check=True)
            item["phases"].append({"phase": phase, "app": str(app), "binary": str(copied_binary), "bundle_id": bundle,
                  "report": str(report), "request": str(request), "request_sha256": sha(request), "profile": str(profile),
                  "additional_arguments": ["--datadir", str(profile)], "creates_new_application_instance": True})
        run["launch_groups"].append(item)
        for case in group_cases:
            run["cases"].append({"id": case["id"], "group": item["id"], "phases": item["phases"]})
    write(out / "run.json", run)
    print(json.dumps({"run": str(out / "run.json"), "cases": len(run["cases"]),
                      "processes": sum(len(group["phases"]) for group in run["launch_groups"]), "execution": "not-run"}, ensure_ascii=False))


def finish(args):
    run_path = args.run.resolve(strict=True)
    run = read(run_path)
    collector = tools(Path(run["repo"]))
    collector.validated_manifest(Path(run["manifest"]))
    if sha(run["manifest"]) != run["manifest_sha256"] or sha(run["binary"]) != run["binary_sha256"]:
        raise ValueError("Prepared canonical inputs/executable changed")
    case = safe_case(run, args.case)
    phase = next((phase for phase in case["phases"] if phase["phase"] == args.phase), None)
    if phase is None:
        raise ValueError("This case has no requested phase")
    folder = Path(phase["report"]).parent
    finalized = folder / (args.phase + "-final.json")
    if finalized.exists():
        raise ValueError("Phase already finalized; never overwrite completed evidence")
    if sha(phase["binary"]) != run["binary_sha256"]:
        raise ValueError("Executed bundle binary changed")
    if sha(phase["request"]) != phase["request_sha256"]:
        raise ValueError("Canonical request changed after preparation")
    report = read(phase["report"])
    if (report.get("ui_thread") is not True or report.get("normal_window_close") is not True or
            Path(report.get("data_dir", "")).resolve() != Path(phase["profile"]).resolve()):
        raise ValueError("GUI report did not use the prepared profile and real UI/close path")
    exit_record = read(phase["report"] + ".exit")
    pid = report.get("pid")
    if (type(pid) is not int or pid <= 0 or exit_record.get("pid") != pid or exit_record.get("result") != 0 or
            exit_record.get("on_exit") is not True or report.get("normal_close_completed") is not True or
            report.get("interpreter_stopped") is not True):
        raise ValueError("Actual matching-PID OnExit/runtime shutdown did not complete; retain failure artifacts")
    startup = Path(phase["report"] + ".startup")
    if startup.read_text(encoding="utf-8") != "post_init complete; no modal dialogs\n":
        raise ValueError("Actual GUI startup marker is missing or incomplete")
    exit_check = verify_process_exit(pid, startup.stat().st_mtime)
    group = report["dualhost"]
    if not report.get("ok") or group.get("passed") is not True:
        raise ValueError("GUI helper failed; retain report/checkpoint: " + str(group))
    observations = group["cases"]
    expected_cases = {item["id"] for item in run["cases"] if item["group"] == case["group"]}
    if {value.get("case") for value in observations.values()} != expected_cases:
        raise ValueError("Shared package run omitted a prepared capability case")
    for observed in observations.values():
        if observed.get("case").rsplit("/", 1)[-1] != args.case.rsplit("/", 1)[-1] or observed.get("phase") != args.phase:
            raise ValueError("GUI report belongs to another canonical form/phase")
        if observed.get("case") not in {item["id"] for item in run["cases"] if item["group"] == case["group"]}:
            raise ValueError("GUI report contains an unprepared canonical case")
        if any(op.get("pid") != pid for op in observed["operations"]):
            raise ValueError("GUI operation PID does not match the actual app exit")
    observed = observations[args.case.split("/")[1]]
    previous_lines = 0
    if args.phase == "resume":
        first = read(folder / "first-final.json")
        if first["pid"] == pid:
            raise ValueError("Restart reused the first host process")
        previous_lines = first["storage_trace_lines"]
    trace = Path(observed["storage"]) / "dualhost-events.jsonl"
    captured = folder / (args.phase + "-trace.jsonl")
    if captured.exists():
        raise ValueError("Captured trace already exists")
    if trace.exists():
        shutil.copy2(trace, captured)
    else:
        # Installing disabled never imports the package; preserve that real absence.
        if args.case.split("/")[-1] != "installed-disabled":
            raise ValueError("Enabled canonical package produced no callback trace")
        captured.write_text("", encoding="utf-8")
    lines = captured.read_text(encoding="utf-8").splitlines()
    if previous_lines > len(lines):
        raise ValueError("Restart truncated previously recorded callback evidence")
    # Ordered close is earned only now, from real MainFrame close + OnExit.
    if not (args.case.endswith("/installed-disabled") and args.phase == "first"):
        for value in observations.values():
            value["operations"].append({"action": "close", "outcome": "ok", "pid": pid})
    traces = [{"path": path} for path in observed.get("archived_traces", [])]
    traces.append({"path": str(captured), "first_line": previous_lines + 1, "last_line": len(lines)})
    final = {"pid": pid, "storage_trace_lines": len(lines), "observations": observations,
             "traces": traces, "binary_sha256": run["binary_sha256"],
             "report_sha256": sha(phase["report"]), "exit_sha256": sha(phase["report"] + ".exit"),
             "trace_sha256": sha(captured), "exit_check": exit_check, "startup_sha256": sha(startup)}
    write(finalized, final)
    print(json.dumps({"case": args.case, "phase": args.phase, "pid": pid, "final": str(finalized)}, ensure_ascii=False))


def collect(args):
    run_path = args.run.resolve(strict=True)
    run = read(run_path)
    collector = tools(Path(run["repo"]))
    plan_path = run_path.parent / "plan/plan.json"
    plan = read(plan_path)
    indexed = {case["id"]: case for case in plan["cases"]}
    for case in run["cases"]:
        folder = Path(case["phases"][0]["report"]).parent
        if not all((folder / (phase["phase"] + "-final.json")).is_file() for phase in case["phases"]):
            continue
        host = {"execution": "gui-host", "binary_sha256": run["binary_sha256"], "operations": [],
                "errors": [], "returns": [], "configuration": {}, "artifacts": {}, "resource_consumer": {},
                "expected_token": 91 if case["id"].endswith("/config-restart") else 37,
                "operator_note": "Actual Manager, WebView JS bridge, production texture editor buttons/GL/Plater/Undo/3MF. Close verified by matching-PID OnExit. No upstream execution asserted."}
        traces = []
        for phase in case["phases"]:
            final = read(folder / (phase["phase"] + "-final.json"))
            if final["binary_sha256"] != run["binary_sha256"]:
                raise ValueError("Finalized phase belongs to another binary")
            if (sha(phase["report"]) != final["report_sha256"] or sha(phase["report"] + ".exit") != final["exit_sha256"] or
                    sha(phase["binary"]) != final["binary_sha256"]):
                raise ValueError("Finalized evidence changed after capture")
            if sha(Path(phase["report"] + ".startup")) != final["startup_sha256"]:
                raise ValueError("Finalized startup marker changed")
            # A delayed .ips may arrive after finish. Recheck the same PID and
            # its original execution time window at every collection.
            verify_process_exit(final["pid"], final["exit_check"]["window_start_unix"], final["exit_check"]["window_end_unix"])
            observed = final["observations"][case["id"].split("/")[1]]
            for key in ("operations", "errors", "returns"):
                host[key].extend(observed[key])
            for key in ("configuration", "artifacts", "resource_consumer"):
                host[key].update(observed[key])
            host["capability_enabled"] = observed["capability_enabled"]
            traces.extend(final["traces"])
        path = plan_path.parent / case["id"] / "host.json"
        write(path, host)
        indexed[case["id"]].update(status="executed", traces=traces)
    write(plan_path, plan)
    collector.collect(SimpleNamespace(host="snapmaker", revision=args.revision, binary=Path(run["binary"]),
                      manifest=Path(run["manifest"]), plan=plan_path, output=args.output))
    print("Upstream remains pending. This report contains only the actually finalized GUI cases.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    p = commands.add_parser("prepare")
    p.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[1])
    p.add_argument("--binary", type=Path, required=True)
    p.add_argument("--python-root", type=Path, required=True)
    p.add_argument("--uv-executable", type=Path, required=True)
    p.add_argument("--manifest", type=Path, required=True)
    p.add_argument("--output", type=Path, required=True)
    p.add_argument("--smoke-preparer", type=Path, help=argparse.SUPPRESS)
    group = p.add_mutually_exclusive_group()
    group.add_argument("--case", action="append", help="Repeat for selected IDs; default pages/pages/fresh-load")
    group.add_argument("--all", action="store_true", help="Prepare all 28 GUI cases only after the first real GUI case passes")
    p.set_defaults(run=prepare)
    p = commands.add_parser("finish")
    p.add_argument("--run", type=Path, required=True)
    p.add_argument("--case", required=True)
    p.add_argument("--phase", choices=("first", "resume"), required=True)
    p.set_defaults(run_function=finish)
    p = commands.add_parser("collect")
    p.add_argument("--run", type=Path, required=True)
    p.add_argument("--revision", required=True)
    p.add_argument("--output", type=Path, required=True)
    p.set_defaults(run_function=collect)
    args = parser.parse_args()
    try:
        (args.run if args.command == "prepare" else args.run_function)(args)
    except (ValueError, KeyError, OSError, subprocess.CalledProcessError) as error:
        if args.command in ("finish", "collect"):
            # Keep failed verification separately; never rewrite the host report.
            try:
                root = args.run.resolve(strict=True).parent
                write(root / (args.command + "-rejected-" + str(time.time_ns()) + ".json"),
                      {"passed": False, "stage": args.command, "reason": str(error), "checked_at_unix": time.time()})
            except OSError:
                pass
        parser.exit(1, "Canonical GUI evidence error: " + str(error) + "\n")


if __name__ == "__main__":
    main()
