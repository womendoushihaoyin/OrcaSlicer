#!/usr/bin/env python3
"""Check GUI results, isolated paths, interpreter cleanup and application exit.

Run after the GUI automation has closed the test apps. A manager report alone
does not prove button interactions; retain screenshots and state observations.
"""
import argparse
import json
import os
from pathlib import Path
import sys


def verify(directory, modes):
    reports = []
    for mode in modes:
        report = directory / f"gui-{mode}.json"
        evidence = json.loads(report.read_text())
        exit_evidence = json.loads(Path(str(report) + ".exit").read_text())
        assert Path(str(report) + ".startup").read_text() == "post_init complete; no modal dialogs\n"
        assert evidence["ok"] and evidence["ui_thread"] and evidence["interpreter_stopped"], evidence
        assert Path(evidence["data_dir"]).resolve() == (directory / "data_dir").resolve(), "Wrong data directory"
        assert Path(evidence["python_home"]).resolve() == (
            directory / f"LocalPluginSmoke-{mode}.app/Contents/Frameworks/python"
        ).resolve(), "Python did not load from the test app"
        assert evidence["host"]["is_signed_in"] is False, "Test unexpectedly used a signed-in account"
        assert exit_evidence == {"on_exit": True, "pid": evidence["pid"], "result": 0}, exit_evidence
        if mode in ("install", "restart"):
            assert evidence["script_count"] == 1
        if mode == "restart":
            assert evidence["disabled_for_next_start"]
        if mode == "disabled":
            assert evidence["script_count"] == 0
        if mode == "host":
            contract = evidence["host_contract"]
            assert contract["presets"] and contract["project_state_preserved"] and contract["failed_export_state_preserved"]
            assert contract["object_count"] > 0 and Path(contract["export_path"]).is_file()
        if mode == "texture" and evidence.get("texture_mix_required"):
            assert evidence["real_plater"] and evidence["texture_stages"][-1] == "texture_mix_workflow_complete"
            assert evidence["plugin_resource"]["pack"] == "Texture mixed gradient"
            assert evidence["plugin_resource"]["plugin_key"] and evidence["plugin_resource"]["bytes"] > 0
            assert len(evidence["physical_colors"]) == 4
            assert evidence["preview_cancel_unchanged"] and evidence["cancel_monitor_samples"] > 0 and evidence["bake_monitor_samples"] > 0
            assert evidence["created_stable_ids"] and all(i > 0 for i in evidence["created_stable_ids"])
            assert evidence["mixed_paint_ids"] and all(i > 4 for i in evidence["mixed_paint_ids"])
            assert len(evidence["sidebar_after"]) > len(evidence["sidebar_before"])
            assert evidence["selected_object_removal_undo"] and evidence["formal_multistep_history"]
            assert evidence["ordinary_undo_preserves_physical_color"] and evidence["branch_discards_mixed_redo"]
            assert evidence["different_project_history_isolated"] and evidence["stable_ids_and_both_configs_roundtrip"]
            assert evidence["baked_triangles"] > evidence["original_triangles"] and Path(evidence["saved_project"]).is_file()
            assert evidence["reopened_gcode_moves"] > 0 and evidence["gcode_extrusions"] > 0
            assert Path(evidence["gcode_file"]).is_file()
            assert len(evidence["gcode_physical_tools"]) >= 2 and all(0 <= i < 4 for i in evidence["gcode_physical_tools"])
        if mode == "texture" and not evidence.get("texture_mix_required"):
            assert evidence["real_plater"] and evidence["texture_stages"][-1] == "texture_workflow_complete"
            assert evidence["preview_shader_drawn"]
            if evidence.get("texture_image_adjustment_required"):
                assert evidence["plugin_source_image_unchanged"] and evidence["adjusted_pixels_embedded"]
                assert Path(evidence["image_copy_saved"]).is_file()
            else:
                assert evidence["preview_plugin_resource"]
            assert evidence["preview_gl_error"] == 0 and evidence["baked_triangles"] > evidence["original_triangles"]
            assert evidence["preview_triangles"] == evidence["baked_triangles"]
            assert evidence["reopened_gcode_moves"] > 0 and Path(evidence["saved_project"]).is_file()
            if evidence.get("editable_layers"):
                assert evidence["editable_data_roundtrip"]
            if evidence.get("texture_authoring_required"):
                assert evidence["authoring_roundtrip"] and Path(evidence["authoring_saved_project"]).is_file()
                layer = evidence["authoring_state"]["layers"][0]
                assert layer["lscm_seam_edges"] and layer["lscm_uv_overrides"]
            if evidence.get("texture_color_required"):
                assert evidence["texture_color_painted_faces"] == evidence["baked_triangles"]
        if mode == "workflow":
            assert evidence["real_plater"] and evidence["initial_gcode_moves"] > 0
            assert evidence["dock_reentrant_teardown"]
            if "dock_layout_restore" in evidence:
                assert all(evidence["dock_layout_restore"].values()), evidence["dock_layout_restore"]
            assert evidence["disabled_auto_stays_dirty"] and evidence["second_plate_isolated"]
            assert evidence["queued_shutdown_safe"] and evidence["finished_slice_releases_plugin"]
            assert evidence["workflow_stages"][-1] == "workflow_complete"
            if evidence["compiled_auto_background"]:
                assert evidence["automatic_reslice"]
            event_logs = list((directory / "data_dir").rglob("workflow-events.jsonl"))
            assert len(event_logs) == 1, "Missing or ambiguous actual GUI lifecycle log"
            events = [json.loads(line) for line in event_logs[0].read_text().splitlines()]
            names = [event["event"] for event in events]
            for name in ("NewProject", "ObjectAdded", "PlateCreated", "PlateSelected", "ProjectDirtyChanged", "ProjectBeforeSave", "ProjectAfterSave"):
                assert name in names, f"Missing GUI lifecycle event: {name}"
            for start, finish in (("SliceStarted", "SliceGeometryFinished"), ("GCodeExportStarted", "GCodeExportFinished")):
                assert names.count(start) >= 3 and names.count(start) == names.count(finish), (start, finish, names)
                active = 0
                for event in events:
                    if event["event"] == start:
                        active += 1
                    elif event["event"] == finish:
                        assert active == 1 and event["code"] == "Ok", event
                        active -= 1
                assert active == 0
            assert names.count("SlicingJobComplete") >= 3, names
            assert all(event["id"] for event in events if event["event"] == "ObjectAdded"), "Object event lost stable ID"
        if mode == "preset":
            assert evidence["preset_override_saved"] and evidence["preset_global_config_unchanged"]
            assert evidence["slicing_selector_worker"] and evidence["slicing_layers"] > 0 and evidence["slicing_width_mm"] == 7.5
        if mode == "slicing":
            assert evidence["slicing_selector_worker"] and evidence["slicing_layers"] > 0 and evidence["slicing_width_mm"] == 5.
        if mode == "replacement":
            assert evidence["replacement_rollback"] and evidence["replacement_commit"]
        if mode == "config":
            assert evidence["custom_config_saved"]
        if mode == "pages":
            assert evidence["page_roundtrip"] and evidence["page_removed_on_unload"]
        if mode in ("pages_multi", "pages_multi_restart"):
            contract = evidence["multi_pages"]
            assert contract["independent_bridges"] and contract["rapid_reload_generation"]
            assert contract["late_old_page_message_rejected"]
            assert all(page["icon_callback"] for page in contract["pages"].values())
            assert contract["persisted_across_restart"] == (mode == "pages_multi_restart")
            assert len(contract["pages"]) == 2
            if mode == "pages_multi_restart":
                previous = json.loads((directory / "gui-pages_multi.json").read_text())
                assert previous["ok"] and previous["pid"] != evidence["pid"]
                assert json.loads((directory / "gui-pages_multi.json.exit").read_text())["result"] == 0
        if mode in ("dock_write", "dock_restart"):
            assert evidence["normal_window_close"] and evidence["normal_close_completed"]
            contract = evidence["dock_restart"]
            assert contract["normal_close_requested"]
            assert contract["restored_after_restart"] == (mode == "dock_restart")
            assert len(contract["panes"]) == 2 and all(pane["shown"] for pane in contract["panes"])
            if mode == "dock_restart":
                previous = json.loads((directory / "gui-dock_write.json").read_text())
                assert previous["ok"] and previous["normal_close_completed"] and previous["pid"] != evidence["pid"]
                assert previous["dock_restart"]["panes"] == contract["panes"]
                assert json.loads((directory / "gui-dock_write.json.exit").read_text())["result"] == 0
        if mode == "ui":
            assert evidence["ui_roundtrip"] and evidence["ui_removed_on_unload"]
            assert evidence["queued_ui_cancelled_on_shutdown"]
        try:
            os.kill(evidence["pid"], 0)
        except ProcessLookupError:
            pass
        else:
            raise AssertionError(f"Test PID {evidence['pid']} is still present; inspect before claiming exit")
        reports.append(evidence)
    assert len({e["pid"] for e in reports}) == len(reports), "Tests did not run in separate processes"
    test_pids = {e["pid"] for e in reports}
    if sys.platform == "darwin":
        oldest = min((directory / f"gui-{mode}.json").stat().st_mtime for mode in modes)
        for crash in (Path.home() / "Library/Logs/DiagnosticReports").glob("Snapmaker_Orca-*.ips"):
            if crash.stat().st_mtime < oldest - 60:
                continue
            data = json.loads(crash.read_text().split("\n", 1)[1])
            assert data.get("pid") not in test_pids, f"Test process crashed: {crash.name}"
    result = {"ok": True, "modes": modes, "pids": sorted(test_pids),
              "isolated_data_dir": str(directory / "data_dir"),
              "manager_button_interactions_require_separate_evidence": True}
    (directory / "verification.json").write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n")
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--modes", nargs="+", choices=("install", "restart", "disabled", "manager", "pages", "pages_multi", "pages_multi_restart", "dock_write", "dock_restart", "ui", "host", "config", "replacement", "slicing", "preset", "workflow", "texture"),
                        default=["install", "restart", "disabled"])
    args = parser.parse_args()
    print(json.dumps(verify(args.directory.resolve(), args.modes), ensure_ascii=False, indent=2))
