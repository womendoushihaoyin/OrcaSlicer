#!/usr/bin/env python3
"""Run each native upstream-runtime contract in a fresh process and profile."""
import argparse
import importlib.util
import json
from pathlib import Path
import subprocess
import shutil
import os
import time
from concurrent.futures import ThreadPoolExecutor, as_completed

parser = argparse.ArgumentParser()
parser.add_argument("--binary", type=Path, required=True)
parser.add_argument("--output-dir", type=Path, required=True)
parser.add_argument("--wheelhouse", type=Path)
parser.add_argument("--jobs", type=int, default=1, help="Independent native processes to run concurrently")
args = parser.parse_args()
if args.jobs < 1:
    parser.error("--jobs must be positive")
args.output_dir.mkdir(parents=True, exist_ok=False)
repo = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("fixtures", repo / "tests/plugin_core/fixtures.py")
fixtures = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fixtures)
fixture_dir = args.output_dir / "fixtures"
fixtures.prepare(fixture_dir)
texture_spec = importlib.util.spec_from_file_location("texture_fixtures", repo / "tests/plugin_core/texture_fixtures.py")
texture_fixtures = importlib.util.module_from_spec(texture_spec)
texture_spec.loader.exec_module(texture_fixtures)
texture_fixtures.prepare(fixture_dir, fixtures.wheel)
if args.wheelhouse:
    for wheel in args.wheelhouse.glob("*.whl"):
        shutil.copy2(wheel, fixture_dir / wheel.name)
results = []
scenarios = ["metadata", "config", "wheel", "multicap", "audit", "failure", "paths", "dependencies", "dependency_environment", "replacement", "replacement_edges", "presets", "slicing_graph", "geometry", "lease", "lease_out_of_order", "stderr"]
if args.wheelhouse:
    scenarios.append("numpy")
scenarios.extend(["slicing_stages", "slicing_shared", "slicing_failure", "slicing_cancel", "slicing_drain"])
scenarios.extend(["replacement_journal", "slicing_noop", "slicing_plugin", "slicing_plugin_errors", "slicing_plugin_pin"])
scenarios.extend(["gcode_plugin", "gcode_plugin_errors", "gcode_plugin_pin", "gcode_plugin_export", "gcode_plugin_events", "gcode_plugin_event_pin"])
scenarios.extend(["path3_geometry", "path3_extrusion", "path3_projection"])
scenarios.extend(["texture_registry", "texture_paths", "texture_library", "texture_bake"])
scenarios.extend(["texture_layers", "texture_model", "texture_3mf"])
scenarios.extend(["texture_kernel", "texture_projection", "texture_lscm", "texture_obj"])
scenarios.extend(["texture_editor", "texture_editor_invalid", "texture_editor_bake", "texture_boundaries"])
scenarios.append("texture_editor_result")
scenarios.extend(["texture_color_palette", "texture_color_commit", "texture_color_v2", "texture_history", "texture_vertex_indices", "texture_image_adjust", "texture_cut", "texture_layout_tools", "texture_island_groups", "texture_authoring", "texture_seam_subdivision"])
scenarios.append("texture_edge_origins")
scenarios.append("texture_visibility")
scenarios.append("texture_mix_recipes")
scenarios.extend(["contour_native", "contour_invalidation", "contour_export_modes", "contour_consumers", "contour_transform", "contour_lifecycle", "project_dirty", "config_notify", "slicing_invalidation"])
scenarios.extend(["contour_local_z", "contour_local_z_rejections", "contour_local_z_writer"])
def run_scenario(scenario):
    log = args.output_dir / (scenario + ".log")
    with log.open("w") as stream:
        try:
            proc = subprocess.run([str(args.binary.resolve()), scenario, str(fixture_dir.resolve()),
                                   str((args.output_dir / (scenario + "-中文" + ('-"quoted"' if scenario == "stderr" and os.name != "nt" else ""))).resolve())],
                                  stdout=stream, stderr=subprocess.STDOUT, timeout=180)
            code = proc.returncode
        except subprocess.TimeoutExpired:
            code = "timeout"
    return {"scenario": scenario, "exit_code": code, "log": str(log.resolve())}

with ThreadPoolExecutor(max_workers=args.jobs) as executor:
    futures = {executor.submit(run_scenario, scenario): scenario for scenario in scenarios}
    completed = {}
    for future in as_completed(futures):
        result = future.result()
        completed[result["scenario"]] = result
        print(result["scenario"], result["exit_code"], flush=True)
    results.extend(completed[scenario] for scenario in scenarios)
# Recovery cases deliberately stay ordered and outside the independent workers.
# A real process death inside Python on_load, followed by a fresh host/profile open.
recovery_data = args.output_dir / "replacement-process-中文"
for scenario, expected in [("replacement_crash", 82), ("replacement_recover", 0)]:
    log = args.output_dir / (scenario + ".log")
    with log.open("w") as stream:
        try:
            proc = subprocess.run([str(args.binary.resolve()), scenario, str(fixture_dir.resolve()), str(recovery_data.resolve())],
                                  stdout=stream, stderr=subprocess.STDOUT, timeout=90)
            code = proc.returncode
        except subprocess.TimeoutExpired:
            code = "timeout"
    results.append({"scenario": scenario, "exit_code": 0 if code == expected else code or "unexpected success",
                    "actual_exit_code": code, "expected_exit_code": expected, "log": str(log.resolve())})
    print(scenario, code, "expected", expected, flush=True)
# Two live hosts must not recover/replace each other's package files.
lock_data = args.output_dir / "shared-profile-中文"
log = args.output_dir / "profile_lock.log"
with log.open("w") as stream:
    holder = subprocess.Popen([str(args.binary.resolve()), "profile_hold", str(fixture_dir.resolve()), str(lock_data.resolve())],
                              stdout=stream, stderr=subprocess.STDOUT)
    try:
        deadline = time.monotonic() + 10
        while not (lock_data / "ready").exists() and holder.poll() is None and time.monotonic() < deadline:
            time.sleep(0.02)
        if not (lock_data / "ready").exists(): raise RuntimeError("profile holder did not start")
        probe = subprocess.run([str(args.binary.resolve()), "profile_probe", str(fixture_dir.resolve()), str(lock_data.resolve())],
                               stdout=stream, stderr=subprocess.STDOUT, timeout=10)
        (lock_data / "release").touch()
        code = holder.wait(timeout=10) or probe.returncode
    finally:
        if holder.poll() is None:
            holder.kill()
            holder.wait(timeout=10)
results.append({"scenario": "profile_lock", "exit_code": code, "log": str(log.resolve())})
print("profile_lock", code, flush=True)
(args.output_dir / "results.json").write_text(json.dumps(results, indent=2) + "\n")
raise SystemExit(any(result["exit_code"] != 0 for result in results))
