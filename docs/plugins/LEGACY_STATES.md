# Local Plater three-state acceptance

This fixture tests one unchanged canonical multi-capability wheel on one isolated macOS host. The states are genuinely uninstalled, installed but disabled, and enabled with Script/Pages/SlicingPipeline/ResourceTexture. Each state imports the same 20×20×1 mm STL, performs one actual Plater/BackgroundSlicingProcess slice and one `Plater::save_project(false)` Save. Only the enabled project is then closed, reopened from its saved 3MF with configuration, and sliced again without resetting its plugin selection: four slices, three saves, one reopen.

**Scope of G-code evidence:** the real Plater produces the preview G-code. U1 is a non-BBL printer; `BackgroundSlicingProcess::process_fff()` calls final-file postprocessing automatically only for BBL, whereas the normal non-BBL export path uses `finalize_gcode()`. This fixture preserves the raw preview, copies it into a separate output artifact and invokes the same public production `run_post_process_scripts(..., false, "File", ..., actual_print.full_print_config())` entry on a worker. The real plugin lease/transaction and canonical `psGCodePostProcess` callback must append exactly one token-37 marker. This is actual Plater generation plus production final-file processing. It does **not** test the Export menu/native SaveFileDialog, upload, a printer connection or device/account login. Pages and Texture are loaded here; their interactive consumers are covered by the separate canonical GUI acceptance.

The helper uses no independent `Print`, no replacement Python module, no synthetic success record, and no source wheel edits. Installation precedes a persisted disabled state without loading Python capabilities. That state must have one descriptor, zero loaded capabilities, and no callback trace. The uninstalled state must have neither registered nor discoverable installed packages.

## Prepare and execute

Use a freshly built plugins-ON binary containing `gui_legacy_states.cpp` and a new output directory. The canonical fixture pins are a local acceptance protocol, not an upstream packaging requirement:

- `orca_dualhost_multi-1.0.0-py3-none-any.whl`: `a1349e4d4493ed38bad8a7198c1fbecd09d4f868e65e7daf4494ee7da4da575f`
- `reference-cube.stl`: `8f6176f7f70c5aa56a656437194b6869a58593e297954c381c85c9fcde7530bf`
- Multi package source module: `0c7edbad9b8444196637bb8ee24e5c41e64841a50aeceaf2d2794e2fc61a7f06`

Example on the current local development machine (all arguments are paths, no network or device credentials):

```sh
project_repo='/Users/leo/CodeProject/Orca_Dev/Snapmaker_Orca/plugin_local_phase1_release_2.4.1'
acceptance_root='/Users/leo/Documents/ChatGPT/技术/SnapmakerOrca插件一期实施/release_2.4.1'
run_directory='/Users/leo/CodeProject/Orca_Dev/plugin-smoke/legacy-states-01-中文'
python3 "$project_repo/scripts/prepare_local_plugin_gui_smoke.py" \
  --binary "$acceptance_root/build-app/src/Snapmaker_Orca" \
  --python-root '/Users/leo/Documents/ChatGPT/技术/SnapmakerOrca插件一期实施/toolchains/python/cpython-3.12.13-macos-aarch64-none' \
  --uv-executable '/Users/leo/.local/bin/uv' \
  --output-dir "$run_directory" --mode legacy_states \
  --fixture "$acceptance_root/dualhost-packages-02/orca_dualhost_multi-1.0.0-py3-none-any.whl"
```

The prepare tool verifies both pinned inputs and refuses an existing `data_dir`. It prints the bundle ID and isolated `--datadir`. The GUI operator launches that development bundle through the documented native app tool, dismisses only the isolated initial setup if necessary, and waits for normal close. The helper drives the four actual slices itself. Neither prepare nor verify launches a GUI. Do not reuse an old profile, copy user account settings, or infer completion from a report while its PID is still alive.

After the process exits:

```sh
python3 "$project_repo/scripts/verify_plugin_legacy_states.py" "$run_directory"
# Wait at least 10 seconds after the first verifier before this second read.
python3 "$project_repo/scripts/verify_plugin_legacy_states.py" "$run_directory" --recheck-exit
```

Require both commands to succeed and `legacy-final-verification.json` to contain `acceptance_complete: true`. The first result deliberately leaves the delayed-crash gate pending. The second checks the same PID and original startup/OnExit time window against newly arrived macOS `.ips` files, and rechecks every recorded raw input/output/report/binary SHA. An `OnExit` callback alone is never sufficient. Keep the original checkpoint/report/exit/crash evidence on failure; fix the cause before using a new profile.

## Comparisons and retained evidence

`legacy-states/` contains four untouched `.preview.gcode` files, four final `.gcode` files and three saved `.3mf` files. `gui-legacy_states.json` records actual world-space triangle coordinates, object/volume settings, the resolved Print configuration, actual layer/move counts, export step timestamps, descriptor/capability counts and the package's storage trace. The report `.legacy` checkpoint survives an assertion failure. Normal application close must run all four real `on_unload` callbacks and finalize the interpreter.

The verifier retains normalized copies and differences in `legacy-states/comparison/`. It allows only:

1. The single generated-by header timestamp.
2. Exactly validated `plugins` and `slicing_pipeline_plugin` metadata, with the known package/name in enabled states and empty selection in inactive states. Also validate the complete expected `different_settings_to_system` list: selecting the plugin adds only these two keys to the known fixture settings; unknown, missing or duplicated keys fail.
3. Exactly one final `\n; ORCA_DUALHOST contract-v1 token=37\n` suffix in enabled states.

Every other G-code byte and resolved configuration key must match the uninstalled baseline. Unknown differences fail with retained diffs; they are not accepted merely because motion counts match. Model comparisons preserve oriented triangles and all object/part metadata, with at most `1e-6 mm` world-coordinate error for the existing 3MF numeric roundtrip. Saved 3MFs must themselves contain the expected selectors, and reopened actual slicing must run the canonical callback again.

The trace must come from the pinned module and same actual PID. It must contain four single loads/unloads, two actual enabled slice jobs, two final-file postprocess jobs and paired actual GUI slicing/export/save lifecycle events. Inactive stages must have no trace. No Windows/Linux or fixed upstream host execution is inferred from this macOS result.

Tool-only contracts can run independently:

```sh
python3 scripts/test_plugin_legacy_states_contracts.py
```

They check strict normalization, bad marker/selector rejection, geometry/config changes, real trace shape/lifetime checks, profile/input guards, PID/crash rejection and delayed-check integrity. Synthetic contract inputs are validator tests, not GUI observations.

## Retained first-run failure and fixture correction

Actual `legacy-states-01-中文` (ON35, PID42918, normal exit0) generated all four real slices, five layers and777 moves each. Strict verification failed and its original raw files/diffs remain. Enabled output correctly added the two plugin selectors to the existing `different_settings_to_system` preset metadata; the gate now requires the complete exact inactive/enabled list, with negative tests for any extra/missing entries. This does not discard arbitrary metadata.

The first fixture assigned `filament_presets` directly to one item, leaving four `filament_colour_mode`, `filament_multi_colors` and `filament_volume_type` items in project_config. Existing 3MF loading aligned those arrays to one, producing a real pre/post-reopen difference. The fixture now calls production `PresetBundle::set_num_filaments(1)` before any slice, which aligns all arrays through the same entry as the application. No production preset/3MF behavior was changed and these three fields are not normalized by the verifier. ON36 rebuild and21 validator contracts pass. Corrected actual `legacy-states-02-中文` (PID45704) passed all comparisons, four slices/three saves/one reopen, actual trace and normal exit; the independent delayed check also passed and `legacy-final-verification.json` has `acceptance_complete: true`. All four states have5 layers/777 moves; inactive final G-code bytes are identical in this run. Enabled/reopened outputs differ only in the explicitly validated metadata, timestamp and single callback suffix. The three material arrays remain strictly compared.
