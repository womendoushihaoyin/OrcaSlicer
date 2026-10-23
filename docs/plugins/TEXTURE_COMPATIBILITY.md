# Editable texture compatibility and acceptance

Reference: OrcaSlicer commit `14a1aa333b6da3ecadf853a4a6285b0f193b7605`.

This matrix distinguishes stored editable data, the displacement kernel, and the GUI that edits or commits it. The existing **Apply texture** dialog produces a baked replacement mesh. Its successful preview/Apply/Undo/3MF workflow alone does not establish editable layer, paint, UV-editor or colour compatibility.

## Current acceptance (2026-10-23)

Production ON29/OFF12 passes 82 native scenarios plus specialist tests and generated SDK checks. Real GUI evidence includes editable Save/Undo/Redo/3MF before Bake (authoring09 and Cut15), image adjustment gray/RGB, UV groups/layout/cut, four physical filament outputs, visibility capture, and baked 3MF/slicing. See VERIFICATION.md for exact build/PID boundaries. The early isolated-link notes below are historical evidence, not the current build status.

Classic Result border/smoothing parameters have real geometry-consumer tests. They are disabled under V2 because the fixed upstream V2 early return does not consume them; saved values remain intact. Visibility16 verifies actual facing/mask replacement/Undo, with native same-facing double-wall occlusion checked separately. UV vertex/edge multiselection passed build24 native checks and multiselect17 actual editable 3MF/bake/slice/exit acceptance. Automatic mixed-recipe transaction and projection-frame authoring remain current integration batches. Actual projector18/19 did not establish native title-bar drag/close interaction. A standalone same-wx probe confirms a modal-capable NSPanel but leaves background input inconclusive; native geometry tests pass and manual interaction remains pending. Mixed bake/history passed actual23 through exact 3MF recentering; the archive mask-cache comparison is being corrected against the existing serialization contract.

## Integration state

- `ModelVolume` stores eight independent texture paint masks, layer parameters/options and per-corner model UVs. Copy/new IDs and the production Undo serializer carry that state.
- Production BBS 3MF reads/writes `paint_texture_0` through `paint_texture_7`, layer JSON and archive-owned PNG bytes under `Metadata/texture_displacement/`. Shared-mesh deduplication considers all eight paint masks.
- Optional JSON `model_uv` is a Snapmaker extension. Fixed upstream does **not** persist the imported model UV channel in 3MF. The extension retains three UV pairs per triangle, including seams; upstream readers may ignore it.
- The full kernel and standalone CGAL parameterizer are integrated into their production targets. The exact TriangleSelector source-face overload and OBJ import fixes are also applied. The files passed syntax checks using a temporary Clang VFS overlay before integration. Newly linked production runtime acceptance remains required.
- The editable GUI is a separate implementation batch. Colour palette selection, mixed-filament creation and assignment through the GUI are not established by the kernel's colour callback API.

## Acceptance matrix

| Area | Required behavior | Concrete acceptance |
|---|---|---|
| Layers and masks | At most eight distinct slots; insertion order does not change ascending-slot blend order; each slot has its own brush mask | `texture_layers`: JSON/defaults/rejection including duplicate slots. `texture_model`: copies, IDs, resets, layer/options/UV binary values. `texture_3mf`: real archive roundtrip, different masks on volumes sharing the same mesh |
| Asset ownership | Loaded projects use archive bytes only; missing embedded images retain settings without reading `layer.path`; decoded use survives archive removal | `texture_3mf`: missing-asset/source-path trap and delete-archive-then-decode |
| Model UV | Three UVs per face; preserve seams, interpolate brush-created corners from their exact parent face; normalized coordinates ignore tile size/aspect correction | `texture_projection`: different UV values on adjacent triangles and ownership after source data is released. `texture_lscm`: real Sphere brush subdivision/T-joint export plus exact parent-face UV checks |
| OBJ input | Preserve vt without an MTL; triangulate quads using the correct corner triplets; bound missing/invalid vt; support negative relative vt; flip V into image row convention | `texture_obj`: real OBJ import, quad correspondence, partial and negative indices, then UV-only production 3MF save/reopen |
| Triplanar | Axis-facing projections with fourth-power normal blending, rotation/offset/tile placement | `texture_projection`: nonuniform 2×2 pixels, known axis sample and weighted two-axis sample |
| Cylindrical | Arc-length coordinate about the selected patch axis and downward axial V | `texture_projection`: known nonuniform pixel at a measured axial position |
| Spherical | Longitude/latitude scaled by radius about the patch centre | `texture_projection`: known nonuniform longitude sample |
| View projection | Flat view axes and projective 3×4 placement; no sample behind the projector | `texture_projection`: known view-axis sample and behind-camera rejection |
| LSCM | Real CGAL conformal solve for curved disk charts; explicit rejection for non-disk input; planar chart shortcut; six cube charts and connected island layout | `texture_lscm`: curved-disk solve, finite nonzero UV area, closed-cube solver rejection, six-chart unwrap/net; `texture_projection`: unwrapped coordinate reaches the image sampler |
| Layer blending | Add, Subtract, Multiply, Divide and Override in slot order; no contribution outside that layer's painted patch | `texture_projection`: all five numerical results through the actual prepared sampler and non-overlapping paint checks |
| Kernel geometry | Actual ModelVolume data enters classic and complete TextureBake pipelines; physical depth remains in world mm under nonuniform scale; no mutation of live source | `texture_kernel`: changed geometry/volume, refinement, no open edges, scale check, cancellation produces no publishable mesh |
| Colour kernel | Height and RGB share placement; grayscale supplies no colour; palette callback returns per-face indices | `texture_projection`: RGB ModelUV sampling plus classic bake returning palette-index-plus-one for each triangle. This does not validate GUI filament/mix creation or final multicolour G-code |
| Undo and project workflow | A real editor commit must snapshot first; Undo must restore masks, layers, UV and source mesh; save/reopen must keep editable state | Native binary-value tests cover serialization values; production `UndoRedo.cpp` compiles with the new Model serializer. Actual authoring09 and Cut15 Plater Save/Undo/Redo/3MF before Bake passed; native and GUI scopes are recorded in VERIFICATION.md |
| UV editor and brushing | Real viewport mask changes and UV island/vertex edits affect the same preview kernel; close/cancel cannot publish a stale job | Separate editable-GUI acceptance. A data roundtrip or native kernel success is insufficient |

All new native scenarios in this matrix must be run on the newly linked binary before marking them PASS. Syntax-only checks do not validate CGAL solver results, mesh topology, archive roundtrips, GL rendering or UI lifetime behavior.

Current evidence: production `core-tests-23` passes `texture_registry`, `texture_paths`, `texture_library`, `texture_bake`, `texture_layers`, `texture_model` and `texture_3mf`. An isolated native link of the new full kernel/CGAL/selector objects against the existing ON7 archives also passes `texture_projection`, `texture_lscm` and `texture_kernel`. The latter validates the real new algorithms; it does not substitute for rebuilding the production target or exercising the editable GUI.

## Kernel integration batch

The integrated batch consists of:

1. `TriangleSelector::get_facets_strict(state, out_source)`, keeping the existing one-argument overload. Each original face's recursive output range receives its exact source index; recursive splitting is unchanged.
2. `TextureDisplacement.cpp` in `libslic3r`, reusing the already integrated Image/Layers/UV and TextureBake files. `TextureDisplacementParameterize.cpp` belongs to **libslic3r_cgal** so it inherits the project's CGAL/Eigen build configuration. The release dependency version provides `longest_border` in `Polygon_mesh_processing/measure.h`.
3. OBJ import/source fixes in `Format/OBJ.cpp` and `Format/objparser.cpp`. No new public OBJ header is needed. The local parser stores two scalars per vt, so relative vt offsets use that stride.
4. `texture_kernel.cpp` and `texture_obj.cpp` in the native test target, with explicit dispatch for `texture_kernel`, `texture_projection`, `texture_lscm` and `texture_obj` before the existing `texture_` prefix fallback.
5. `TextureEditSession.cpp`, `EditableTextureDialog.cpp` and `EditableTextureUV.cpp` in the GUI target, plus `texture_editor.cpp` and its three scenarios in the native target and test runner. Preview implementation is included by the dialog and is not a separate translation unit.

Image, Layers and UV types remain shared definitions; the full kernel does not duplicate their codec/sampling symbols. Layers/UV remain unconditional kernel sources because Model/3MF persist editable state with Python plugins OFF as well. The full consumer sources currently follow the local-plugin feature gate alongside TextureBake.

## Commit contract for the editable GUI

A worker receives a detached mesh/layer/mask/UV snapshot and the selected placement transform. It must not retain a live `ModelVolume*`. Before publishing, verify that selection, volume identity, geometry and placement still match the snapshot. Cancellation or an exception publishes no geometry.

Baking changes topology. Preserve or transfer existing support/seam/colour/fuzzy paint and model UVs to the resulting mesh. Fixed upstream clears all texture masks after a topology-changing bake; the classic path keeps unconsumed masks because its triangle indices are unchanged. Keep layer parameters/assets available for subsequent painting. Refuse a bake that produced no geometric change so it cannot erase painted masks without producing relief. `ModelVolume::set_mesh()` does not perform these transfers or clear this state. The older one-shot **Apply texture** entry deliberately clears all editable texture state after taking its Undo snapshot.

## Saving, reopening and slicing

**Save editable layers stores editing data. Bake into mesh changes the printed surface.** Saving layers retains their image bytes, parameters, paint masks and UV layout on the selected volume. Saving the project as BBS 3MF embeds that data; reopening the project restores it for the editor. A preview is temporary and is not a replacement for baking.

Slicing consumes `ModelVolume::mesh().its`. It does not automatically evaluate texture layers. Consequently, saving editable layers and then slicing prints the existing surface. To print the relief, preview it and choose **Bake into mesh** before slicing. After baking, the changed mesh is stored in the project and is the surface used by the ordinary slicing path. Undo restores the prior mesh and editable state.

This division matches the fixed upstream, rather than omitting an automatic-bake hook. Evidence at `14a1aa333b6da3ecadf853a4a6285b0f193b7605`:

- `src/slic3r/GUI/Gizmos/GLGizmoTextureDisplacement.cpp`, the Standard-mode description near line 7066 and the Bake footer near line 8537, directs the user to paint and explicitly bake.
- `src/slic3r/GUI/Jobs/TextureDisplacementBakeJob.cpp`, `process()`/`finalize()`, computes a detached result and replaces the volume mesh only when the bake commits. Preview jobs do not replace it.
- `src/libslic3r/Print.cpp`, `PrintApply.cpp`, `PrintObject.cpp` and `PrintObjectSlice.cpp` contain no texture-layer evaluation or `build_texture_displacement()` call. Local `PrintObjectSlice.cpp` likewise copies the volume mesh in `slice_volume()`.
- Local `TextureEditSession::apply()` writes the layer state for both actions, but calls `volume.set_mesh()` only when a baked result is supplied. `Plater::apply_resource_texture()` snapshots before applying either action. Its normal model-change notification may schedule background processing, but does not turn stored texture data into geometry.

Archive image ownership also makes the project portable: a layer's `path` is only a source label after import. The importer reads embedded bytes, never the old local path. Moving or renaming the 3MF, or removing the original texture/plugin resource, does not invalidate an image already embedded in the project. Missing embedded images keep their layer settings and can be repaired with **Replace image**; they contribute no displacement until repaired.

The native `texture_boundaries` scenario exercises this distinction through real `TextureEditSession`, BBS 3MF and `PrintObject::slice()` calls: save editable data leaves the slice contours unchanged; a project with Chinese names survives moving/renaming and deletion of its original image; reopening restores editable masks/images; replacing missing or corrupt images repairs the layers; explicit baking changes the ordinary slice result. Its isolated native execution passed. This test does not establish Plater event-loop or GL behavior, and must also be run through the production test target when integrated.

## Missing, disabled and unfamiliar data

| Input or lifecycle change | Current behavior and limit |
|---|---|
| A plugin texture capability is disabled, unloaded or removed | Its library entries disappear and stale entries cannot acquire new bytes. Images already copied onto a model remain usable and can be embedded in 3MF. This is verified by `texture_registry` and `texture_paths`; disabling a resource provider does not erase project content. |
| A layer is unpainted | It contributes no displacement. Fixed upstream has no general per-layer `enabled` member; `tile_enabled` and `color_enabled` control repetition and colour, not the entire layer. A zero depth is not a substitute for a disabled layer in every blend mode. |
| An image is missing from an otherwise valid layer document | The layer parameters and mask load with no image. The old filesystem `path` is not opened. The editor can replace the image and keep editing. |
| An archive carries corrupt image bytes | The bytes and parameters survive loading, but the bounded decoder rejects the image and the displacement sampler skips it. The editor can replace it. The session rejects a bake whose mesh is unchanged. The current editor also transfers an unconsumed missing/damaged-image mask across a topology-changing bake; this preserves more editing data than fixed upstream's reset-all behavior. |
| The layer JSON is missing, malformed, or contains duplicate/out-of-range slots | Import reports an error for that metadata and retains the model geometry. The failed layer document is not installed. Triangle mask data may still exist, but layer parameters cannot be reconstructed from masks alone. Saving such an imported project does not preserve the original malformed document. |
| Optional keys are absent or unknown | Absent fields retain defaults and unknown keys are ignored. The JSON writer emits the known version-1 fields; arbitrary future metadata is not preserved losslessly. |
| Projection/tile/blend values are unfamiliar | The codec clamps projection into the known range, while unfamiliar tile/blend enums are rejected by `TextureEditSession::validate()`. The latter prevents preview or mutation, but currently also prevents opening that volume in the editor to repair it. This is a compatibility limit, not a claim of support for future enum values. |
| Imported parameters/UVs are non-finite or stale for the mesh | The editing boundary rejects non-finite parameters, malformed masks, invalid mesh-seam indices and mismatched model UV counts before applying them. The archive reader independently bounds model UV size and values. Extreme signed keys in manual UV overrides are ignored without integer overflow. |

Source evidence for these boundaries is in `TextureDisplacementLayers.cpp` (JSON defaults/ranges), `Format/bbs_3mf.cpp::_apply_texture_displacement` (archive-only images and UV validation), `TextureDisplacement.cpp::prepare_sampleable_layers` (skip undecodable/unpainted layers), `TextureEditSession.cpp::validate/apply` (editing/commit boundary), and `TextureLibrary.cpp` plus `PluginResourceTextures.cpp` (provider lifetime and owned byte snapshots). Non-ASCII archive relocation and source-file removal are exercised on macOS; this is not a Windows-specific path test.

The local `apply_lscm_uv_overrides()` widens `-key - 1` to `int64_t` before the out-of-range check. Fixed upstream computes this expression in `int`, which overflows for an imported `INT_MIN` key. This one-line repair changes no public header or accepted ordinary UV index.
