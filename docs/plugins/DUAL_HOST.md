# Fixed-upstream dual-host behavior gate

This gate installs the **same wheel bytes** in Snapmaker Orca and upstream Orca at commit `14a1aa333b6da3ecadf853a4a6285b0f193b7605`. Source inspection, generated SDKs and utility tests do not count as host execution. The fixed upstream runtime is currently **pending**.

## Package and matrix

Run with Python 3.11 or newer from the repository root:

```sh
python3 scripts/prepare_plugin_dualhost.py --output /absolute/new-directory/packages
python3 -m unittest discover -s tests/plugin_contracts -p 'test_*.py' -v
```

The builder creates five deterministic, dependency-free wheels: Script, Pages, SlicingPipeline, ResourceTexture and all four together. The manifest records each wheel hash, installed Python source hash, the 20 × 20 × 1 mm reference STL hash, common settings recipe and 56 cases: four individual capabilities plus four capabilities in the multi-capability package, each exercised in seven forms.

The seven forms are an **explicit local acceptance protocol**, not an upstream documentation requirement. Wheel format is an independent dimension; this matrix does not claim `.py`, sdist, ZIP or dependency installation parity.

| Form | Required sequence |
| --- | --- |
| fresh-load | install → enable → load → exercise → normal close |
| installed-disabled | install → disable → restart → verify disabled → normal close |
| disable-reenable | install → enable → load → exercise → disable → verify disabled → enable → exercise → close |
| config-restart | install → enable → load → set config → exercise → close → restart → verify config → exercise → close |
| same-package-reload | install → enable → load → exercise → reload identical package → exercise → close |
| uninstall-reinstall | install → enable → load → exercise → uninstall → verify removed → reinstall identical wheel → enable → load → exercise → close |
| error-recovery | install → enable → load → set error → observe failed exercise → recover → successful exercise → close |

Use a disposable, separate profile for each host/form. Keep the package files immutable. Multi-capability cases can share one actual run of a form, provided each capability is exercised and gets its own observation; the collector filters the shared trace by capability. Record automatic operations as completed actions with an explanatory note, e.g. installation automatically loads an enabled package.

## Host exercises

| Capability | Actual action and evidence |
| --- | --- |
| Script | Execute “Dualhost Script”; retain the host result. The trace captures `ExecutionResult` type/fields/status, JSON with Chinese/Greek text, and the actual invalid-enum `TypeError`. |
| Pages | Open “Dualhost Page”, press **Round trip 中文-α**, and wait for the displayed reply. The JavaScript acknowledgement proves WebView → Python → WebView → Python, beyond `get_ui()` returning HTML. |
| SlicingPipeline | Load the generated STL, apply the manifest's common recipe and the same printer/nozzle/filament/plate, enable this slicing capability, slice and export a fresh textual G-code file. Retain the host's resolved configuration export. The trace must include actual `posSlice` and `psGCodePostProcess` callbacks; export must contain exactly one `; ORCA_DUALHOST contract-v1 token=37` footer (91 after config restart). |
| ResourceTexture | Use the valid `height-纹理.png` from the installed resource package on the reference mesh. Render a real preview, apply/bake displacement, Undo, then save/reopen 3MF. Retain a preview image and the resulting 3MF. Record actual decode, mesh-change, Undo and project-roundtrip results; a directory listing/provider callback is insufficient. |

Global configuration starts as `{"token":37,"mode":"success"}`. For `config-restart`, persist `{"token":91,"mode":"success"}` in the capability configuration, close normally, restart a **different process**, and observe the value being used. Record `configuration.after_restart` as the object returned/read from the host. ResourceTexture's config retention is recorded from the host configuration UI/API, separately from image use.

For Script/Pages/Slicing error recovery, set `mode` to `raise`, perform the action, retain the host diagnostic, restore `mode` to `success`, then repeat successfully. For ResourceTexture, attempt the packaged `bad-image.png` in the real resource consumer, retain its rejection diagnostic, and successfully consume the valid image. If a host silently filters the invalid file, report what happened; do not invent a visible error. Fatal/recoverable/skipped Script results are additionally selectable via `mode=fatal`, `recoverable`, `skip`; they are supported fixture probes, not extra completed matrix cases.

During `installed-disabled`, capture the trace window from the restart/disabled check through close. Discovery may query a resource directory; that does not prove active use. Functional Script/Page/slicing callbacks in this window fail the disabled contract. For the re-enable form, keep the full before/after trace and record the disabled UI/consumer check explicitly.

Keep lifecycle activity reproducible: same project opens, same pane actions and normal closes on both hosts. Unexpected callback/order differences remain in the report; they are not silently normalized away.

## Create and fill an observation plan

The scaffold hashes the **actual executable selected for the run** but does not launch it:

```sh
python3 scripts/compare_plugin_dualhost.py scaffold \
  --manifest /absolute/packages/manifest.json \
  --binary /absolute/path/to/actual/host/executable \
  --output /absolute/new-directory/snapmaker-run
```

It creates `plan.json` and one `host.json` per case. Every action starts pending. After actual operation, fill each action's `outcome` (`ok` or `error`) and actual PID, in order. Restart must use a new PID. For an error-recovery case, `exercise_error` must have outcome `error`; later `exercise` must succeed. Set `execution` to `gui-host` or `native-host` according to what actually ran. Pages and texture require `gui-host`.

The fixture writes `dualhost-events.jsonl` under its own `orca.host.plugin.storage()` directory. Each record carries actual PID, source hash, sequence, capability, callback and thread relation. Copy the trace **before uninstall or profile removal**, and before a reload/restart if necessary. A case can consume multiple chronological trace files or inclusive line ranges:

```json
{
  "id": "script/script/config-restart",
  "status": "executed",
  "host_observation": "script/script/config-restart/host.json",
  "traces": [
    {"path": "saved-before-restart.jsonl", "first_line": 1, "last_line": 12},
    {"path": "saved-after-restart.jsonl"}
  ],
  "gcode": null
}
```

Do not overlap or duplicate trace ranges. Use the ranges for this case only; do not mix callbacks from unrelated projects, modes or earlier cases. Source hashes must match the manifest's installed module. The collector preserves selected raw lines, complete source-file hashes and range provenance.

Fill host result/error observations from actual behavior. `errors` is an array of objects with `type`, optional `status`/`operation`, and the full diagnostic in `message`. Configuration observations and resource measurements belong in the host JSON, not invented trace records. Resource success fields are:

```json
{
  "resource_consumer": {
    "image_decoded": true, "preview_rendered": true,
    "mesh_changed": true, "undo_restored": true, "project_roundtrip": true
  },
  "artifacts": {
    "mesh": "/absolute/packages/reference-cube.stl",
    "preview": "/absolute/actual-preview.png",
    "project": "/absolute/actual-saved.3mf"
  }
}
```

For invalid-image recovery also record `invalid_image_rejected`. Add useful measured facts such as triangle counts, GL errors and reopen status; the comparison preserves differences. Slicing requires artifacts `mesh` and `resolved_config`, plus the actual exported file in the plan's `gcode` field. Artifact paths in `host.json` are relative to that JSON; trace/G-code paths in the plan are relative to the plan. The collector copies artifacts with hashes. Do not replace a failed result with a successful placeholder.

## Collect and compare

```sh
python3 scripts/compare_plugin_dualhost.py collect \
  --host snapmaker --revision ACTUAL_SNAPMAKER_REVISION_OR_DIRTY_BUILD_LABEL \
  --binary /absolute/snapmaker/executable \
  --manifest /absolute/packages/manifest.json \
  --plan /absolute/snapmaker-run/plan.json --output /absolute/new-directory/snapmaker-observations

python3 scripts/compare_plugin_dualhost.py collect \
  --host upstream --revision 14a1aa333b6da3ecadf853a4a6285b0f193b7605 \
  --binary /absolute/fixed-upstream/executable \
  --manifest /absolute/packages/manifest.json \
  --plan /absolute/upstream-run/plan.json --output /absolute/new-directory/upstream-observations

python3 scripts/compare_plugin_dualhost.py compare \
  --manifest /absolute/packages/manifest.json \
  --snapmaker /absolute/snapmaker-observations/observations.json \
  --upstream /absolute/upstream-observations/observations.json \
  --output /absolute/new-directory/comparison
```

Omit either host argument when no real record exists. Both omitted produces an honest 56-pending report and exit **2**, without inventing an upstream binary. Exit **0** means every matrix case matched with no contract failures; **2** means pending, different or contract-failed; **1** means invalid evidence/input. Existing output directories are never overwritten.

Dimensions include return shapes/data, error classes, configuration, lifecycle order and main-thread relation, Page messages, resource-provider bytes, actual resource-consumer outcomes, slicing context and G-code. Raw diagnostics retain process-specific details; comparison omits lifecycle-generated IDs/paths and compares error classes/statuses rather than pybind address formatting. Exact G-code hashes remain recorded; command hashes ignore comments and redundant whitespace, but retain numeric/toolpath differences. Different engine versions may produce different G-code even with matching plugin contracts: investigate the reported dimension and retained resolved configs instead of labeling every command difference a plugin regression.

Binary revision is an **operator attestation**, coupled to a recorded executable hash and provenance. The collector cannot establish that an arbitrary executable was built from a claimed SHA. Retain its source HEAD, tracked patch/clean status, configure/build logs and binary hash alongside the run; a renamed current release or an `orca` stub module cannot substitute for the fixed upstream host.

## Shortest feasible fixed-upstream execution path

1. Prefer an already built, runnable artifact with verifiable provenance for the exact SHA and matching architecture. No such local artifact is established by this work.
2. Otherwise obtain a complete isolated checkout at the fixed SHA and its required submodules, including `deps_src/pybind11`. Do not modify/rebase the migration checkout or reuse another commit's CMake cache.
3. Prepare matching dependencies in an isolated prefix. Fixed root CMake requires Boost ≥ 1.83, Eigen3 5.0.1 and bundled Python **3.12.13 exactly** at `<prefix>/libpython`; GUI additionally links LibDataChannel, noise and platform FFmpeg. Reuse existing dependency artifacts only after version/ABI/configuration verification; do not mix already compiled Snapmaker GUI/core objects into this host.
4. A reduced **real GUI host** can use `SLIC3R_CAD=OFF` consistently in both deps and main configuration, `SLIC3R_GUI=ON`, `ORCA_BUILD_PYTHON_STUBGEN_MODULE=OFF`, and an explicit existing `ORCA_BUNDLED_UV_EXECUTABLE` to avoid that automatic download. Build the `OrcaSlicer` target with bounded parallelism in its own build directory. These flags are verified in fixed source; this is a proposed build path, not a successful configure/build.
5. Run the same wheel first through fresh-load Script and multi-capability cases, then the complete matrix. CLI/native execution can cover part of Script/Slicing if supported, but cannot satisfy real WebView/texture-render acceptance. `orca_stubgen` links `libslic3r_gui` in this revision and does not provide a lightweight replacement for the GUI host.

Read-only inventory (no downloads, configure or build):

```sh
python3 scripts/inspect_plugin_upstream_build.py \
  --source /absolute/candidate/upstream-checkout \
  --prefix /absolute/candidate/dependency-prefix \
  --output /absolute/new-file/upstream-inventory.json
```

The current local `OrcaSlicer` checkout is clean at `e023ce5777693ff5db119d5e40c39392cc1438d3`, and its Git object database does not contain the fixed commit. The extracted fixed-source evidence is partial, not a buildable checkout. The existing arm64 prefix has Boost 1.84 headers; checked standard locations lack required Eigen, bundled Python, FFmpeg, LibDataChannel and noise artifacts. The inventory records the exact checked paths; it does not claim those libraries are absent everywhere on the machine. A full new GUI/core build plus missing-dependency work is therefore required unless a provenance-verified fixed binary is supplied. No credible duration estimate or runtime success is inferred from source inspection.

Current evidence: `dualhost-packages-02` (five wheels + STL), `dualhost-pending-02` (56 pending), `dualhost-upstream-inventory-01.json`, under the release_2.4.1 implementation evidence directory. Utility tests passed 10/10 in a Unicode directory. All five wheels were also installed with offline/no-index/no-deps `uv pip --target` into a disposable directory using local Python 3.14.6; installed module hashes and Unicode resources matched. This verifies packaging only, not embedded Python 3.12 or a host import. Snapmaker实际native28（Script/Slicing及multi对应能力）已在 `dualhost-snapmaker-native-01/observations-final` 收集；ON35真实GUI全量28项已在27个正常退出进程中完成并收集（`dualhost-full-01-中文/observations-01`）；与native28汇总为 `dualhost-snapmaker-aggregate-01`，Snapmaker侧56/56 observed、coverage_complete=true。首次两例pilot证据独立保留，不重复合并。保留每个case自己的原始report/二进制SHA，不将native runner与GUI可执行文件合成一个共同版本。固定上游仍未真实执行。 Windows/Linux execution and fixed-upstream runtime comparison remain pending.
