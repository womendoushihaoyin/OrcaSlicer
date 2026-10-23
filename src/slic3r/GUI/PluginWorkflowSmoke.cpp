#include "PluginWorkflowSmoke.hpp"
#include "PluginSlicingGui.hpp"
#include "TextureLibrary.hpp"
#include "GUI_App.hpp"
#include "MainFrame.hpp"
#include "Plater.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/TextureDisplacement.hpp"
#include "libslic3r/Utils.hpp"
#include "slic3r/plugin/PluginManager.hpp"
#include <wx/evtloop.h>
#include <wx/utils.h>
#include <wx/weakref.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <set>
#include <stdexcept>

namespace Slic3r::GUI {
namespace {
void require(bool value, const std::string& message) { if (!value) throw std::runtime_error(message); }
struct TextureSmokeObservation {
    nlohmann::json* evidence = nullptr;
    std::filesystem::path path;
    void write() const { std::ofstream output(path); output << evidence->dump(2) << '\n'; }
};
thread_local TextureSmokeObservation* texture_observation = nullptr;
void pump_for(std::chrono::milliseconds duration)
{
    const auto until = std::chrono::steady_clock::now() + duration;
    do {
        if (auto* loop = wxEventLoopBase::GetActive()) loop->YieldFor(wxEVT_CATEGORY_ALL);
        wxMilliSleep(5);
    } while (std::chrono::steady_clock::now() < until);
}
template<class Predicate> void wait_until(Plater* plater, Predicate ready, const std::string& description)
{
    wxWeakRef<Plater> target(plater);
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(120);
    while (!ready()) {
        require(bool(target), "Plater closed during " + description);
        require(std::chrono::steady_clock::now() < until, "Timed out: " + description);
        pump_for(std::chrono::milliseconds(10));
        require(bool(target), "Plater closed during " + description);
    }
}
void check_slice(PartPlate* plate, double expected_width, nlohmann::json* evidence = nullptr)
{
    require(plate && plate->is_slice_result_valid(), "Plate has no valid result");
    const auto* print = plate->fff_print();
    require(print && print->is_step_done(psGCodeExport), "Real GUI G-code export did not finish");
    require(plate->get_slice_result() && !plate->get_slice_result()->moves.empty(), "No real G-code moves");
    size_t count = 0;
    nlohmann::json layer_regions = nlohmann::json::array();
    for (const auto* object : print->objects()) for (const auto* layer : object->layers()) {
        ++count;
        if (expected_width >= 0.) for (const auto& box : layer->lslices_bboxes)
            require(std::abs(unscale<double>(box.size().x()) - expected_width) < .02,
                    "Wrong sliced contour width at layer " + std::to_string(layer->id()) + ": " +
                    std::to_string(unscale<double>(box.size().x())) + " mm; expected " + std::to_string(expected_width));
        bool has_perimeters = false;
        nlohmann::json regions = nlohmann::json::array();
        for (const auto* region : layer->regions()) {
            has_perimeters |= !region->perimeters.empty();
            if (evidence) regions.push_back({{"surfaces", region->slices.surfaces.size()},
                                             {"perimeters", region->perimeters.entities.size()}});
        }
        // Painted volumes may allocate empty regions. Compatible nonempty
        // regions may also share their perimeter output (Layer::make_perimeters).
        // Every layer of these solid fixtures must still have actual walls.
        require(has_perimeters, "Native perimeters missing from layer " + std::to_string(layer->id()));
        if (evidence) layer_regions.push_back({{"layer", layer->id()}, {"regions", std::move(regions)}});
    }
    require(count > 0, "No sliced layers");
    if (evidence) (*evidence)["sliced_layer_regions"] = std::move(layer_regions);
}
void slice_current(Plater* plater, PartPlate* plate, double width, nlohmann::json* evidence = nullptr)
{
    plater->reslice();
    wait_until(plater, [&] {
        require(!plate->is_apply_result_invalid(), "Plater validation rejected the workflow presets/model");
        return plate->is_slice_result_valid() && plate->fff_print()->is_step_done(psGCodeExport) &&
            !plater->is_background_process_slicing();
    }, "real Plater background slice");
    check_slice(plate, width, evidence);
}
void select_smoke_presets(PresetBundle& bundle)
{
    const std::string printer = "Snapmaker U1 (0.4 nozzle)";
    const std::string process = "0.20mm Standard @Snapmaker U1 (0.4 nozzle)";
    const std::string filament = "Snapmaker PLA Basic @U1";
    wxGetApp().app_config->set_variant("Snapmaker", "Snapmaker U1", "0.4", true);
    if (!bundle.printers.find_preset(printer) || !bundle.prints.find_preset(process) || !bundle.filaments.find_preset(filament))
        bundle.load_presets(*wxGetApp().app_config, ForwardCompatibilitySubstitutionRule::EnableSilent);
    require(bundle.printers.find_preset(printer) && bundle.prints.find_preset(process) && bundle.filaments.find_preset(filament),
            "Workflow smoke needs bundled Snapmaker U1 and PLA presets");
    // A fresh isolated profile has no user-selected variants/filaments yet.
    bundle.printers.find_preset(printer, false, true)->is_visible = true;
    bundle.prints.find_preset(process, false, true)->is_visible = true;
    bundle.filaments.find_preset(filament, false, true)->is_visible = true;
    require(bundle.printers.select_preset_by_name(printer, true), "Could not select workflow printer");
    require(bundle.prints.select_preset_by_name(process, true), "Could not select workflow process");
    require(bundle.filaments.select_preset_by_name(filament, true), "Could not select workflow filament");
    bundle.filament_presets = {filament};
    bundle.update_multi_material_filament_presets();
    for (size_t index = 0; index < bundle.filament_presets.size(); ++index)
        bundle.set_filament_preset(index, filament);
}
}

nlohmann::json run_plugin_workflow_smoke(const std::filesystem::path& report)
{
    wxString mode, isolated;
    require(wxIsMainThread() && wxGetEnv("ORCA_LOCAL_PLUGIN_SMOKE_MODE", &mode) && mode == "workflow" &&
            wxGetEnv("ORCA_LOCAL_PLUGIN_SMOKE_DATADIR", &isolated) && !isolated.empty(), "Workflow smoke requires explicit isolated GUI mode");
    require(std::filesystem::weakly_canonical(std::filesystem::u8path(isolated.ToUTF8().data())) ==
            std::filesystem::weakly_canonical(std::filesystem::u8path(data_dir())), "Workflow smoke refused active user profile");
    auto* plater = wxGetApp().plater();
    auto* bundle = wxGetApp().preset_bundle;
    require(plater && bundle && wxGetApp().app_config, "GUI has no live Plater/presets");
    struct Cleanup {
        wxWeakRef<Plater> target;
        ~Cleanup() {
            // Even a failed assertion must release the real worker before runtime shutdown.
            if (target) target->suppress_background_process(true);
            shutdown_plugin_slicing_gui();
        }
    } cleanup{plater};
    nlohmann::json evidence{{"real_plater", true}, {"workflow_stages", nlohmann::json::array()}};
    const auto checkpoint = [&](const char* stage) {
        evidence["workflow_stages"].push_back(stage);
        std::ofstream output(report.string() + ".workflow");
        output << evidence.dump(2) << '\n';
    };
#ifdef SUPPORT_BACKGROUND_PROCESSING
    constexpr bool automatic_supported = true;
#else
    constexpr bool automatic_supported = false;
#endif
    evidence["compiled_auto_background"] = automatic_supported;
    auto& manager = PluginManager::instance();
    PluginCapabilityId id;
    {
        const auto caps = manager.get_plugin_capabilities("", PluginCapabilityType::SlicingPipeline);
        require(caps.size() == 1 && caps.front()->name() == "Workflow contour", "Use workflow_slicing.py as the workflow fixture");
        id = caps.front()->identity();
    }
    wxGetApp().app_config->set("background_processing", "0");
    checkpoint("workflow_started");
    select_smoke_presets(*bundle);
    wxGetApp().mainframe->select_tab(MainFrame::tp3DEditor);
    plater->new_project(true, false, "Plugin workflow smoke");
    auto& process = bundle->prints.get_edited_preset().config;
    // Use bundled printer/material settings, but remove geometric compensation
    // from this exact-width fixture. U1 Standard otherwise shrinks layer zero
    // by 0.15 mm per side before the plugin's posSlice callback.
    process.set_key_value("elefant_foot_compensation", new ConfigOptionFloat(0.));
    process.set_key_value("slicing_pipeline_plugin", new ConfigOptionStrings({id.name}));
    process.set_key_value("plugins", new ConfigOptionStrings({id.plugin_key + ";;" + id.name}));
    process.set_key_value("print_plugin_config_overrides", new ConfigOptionString("[]"));
    require(manager.get_config().store_capability_config(id, {{"factor", .5}}), "Could not seed workflow capability config");
    wxGetApp().load_current_presets(false, false);
    plater->on_config_change(bundle->full_config());
    pump_for(std::chrono::milliseconds(800));
    checkpoint("presets_ready");

    const auto model_path = std::filesystem::u8path(data_dir()) / "workflow-cube.stl";
    auto cube = make_cube(10., 20., 1.);
    require(cube.write_binary(model_path.u8string().c_str()), "Could not write GUI model fixture");
    require(!plater->load_files(std::vector<std::string>{model_path.u8string()}, LoadStrategy::LoadModel).empty(), "Plater import failed");
    pump_for(std::chrono::milliseconds(800));
    auto& plates = plater->get_partplate_list();
    auto* first = plates.get_curr_plate();
    require(first && first->can_slice(), "Imported fixture is not sliceable");
    const int first_index = first->get_index();
    checkpoint("cube_imported");
    slice_current(plater, first, 5.);
    evidence["initial_gcode_moves"] = first->get_slice_result()->moves.size();
    checkpoint("first_plate_sliced");

    // Use the real Save workflow so dirty-state events have a clean baseline.
    // Merely exporting a copy intentionally preserves the live project's state.
    const auto saved_project = std::filesystem::u8path(data_dir()) / "workflow-project.3mf";
    plater->set_project_filename(wxString::FromUTF8(saved_project.u8string()));
    require(plater->save_project(false) == wxID_YES && std::filesystem::is_regular_file(saved_project),
            "Real workflow project save failed");
    evidence["saved_project"] = saved_project.u8string();
    checkpoint("project_save_verified");

    const int second_index = plates.create_plate();
    require(second_index >= 0, "Second plate creation failed");
    auto* second = plates.get_plate(second_index);
    second->config()->set_key_value("slicing_pipeline_plugin", new ConfigOptionStrings());
    second->config()->set_key_value("plugins", new ConfigOptionStrings());
    second->config()->set_key_value("print_plugin_config_overrides", new ConfigOptionString("[]"));
    plater->select_plate(second_index);
    require(!plater->load_files(std::vector<std::string>{model_path.u8string()}, LoadStrategy::LoadModel).empty(), "Second plate import failed");
    pump_for(std::chrono::milliseconds(800));
    slice_current(plater, second, 10.);
    const auto second_timestamp = second->fff_print()->step_state_with_timestamp(psGCodeExport).timestamp;
    const auto second_moves = second->get_slice_result()->moves.size();
    checkpoint("unrelated_plate_sliced");
    plater->select_plate(first_index);
    pump_for(std::chrono::milliseconds(400));
    // Adding a plate may move the plate layout; establish both cached results after that change.
    if (!first->is_slice_result_valid()) slice_current(plater, first, 5.);
    require(second->is_slice_result_valid(), "Second cached result disappeared before isolation test");

    require(manager.get_config().store_capability_config(id, {{"factor", .75}}), "Config update failed");
    require(first->is_slice_result_valid(), "Config callback mutated GUI synchronously");
    wait_until(plater, [&] { return !first->is_slice_result_valid(); }, "queued config invalidation");
    require(first->get_slice_result()->moves.empty() && !first->fff_print()->is_step_done(psGCodeExport), "Old G-code remained available");
    pump_for(std::chrono::milliseconds(700));
    require(!plater->is_background_process_slicing() && !first->is_slice_result_valid(), "Disabled background setting auto-started slicing");
    require(second->is_slice_result_valid() && second->get_slice_result()->moves.size() == second_moves &&
            second->fff_print()->step_state_with_timestamp(psGCodeExport).timestamp == second_timestamp, "Unrelated plate was invalidated");
    evidence["disabled_auto_stays_dirty"] = true;
    evidence["second_plate_isolated"] = true;
    checkpoint("queued_invalidation_verified");
    slice_current(plater, first, 7.5);
    checkpoint("manual_reslice_verified");

    if (automatic_supported) {
        wxGetApp().app_config->set("background_processing", "1");
        require(manager.get_config().store_capability_config(id, {{"factor", .6}}), "Automatic config update failed");
        wait_until(plater, [&] { return !first->is_slice_result_valid(); }, "automatic invalidation");
        wait_until(plater, [&] { return first->is_slice_result_valid() && first->fff_print()->is_step_done(psGCodeExport); }, "automatic reslice");
        check_slice(first, 6.);
        evidence["automatic_reslice"] = true;
        checkpoint("automatic_reslice_verified");
    }
    wxGetApp().app_config->set("background_processing", "0");
    // Drain finished status events before the lifetime test and release any consumer pins.
    wait_until(plater, [&] { return !plater->is_background_process_slicing(); }, "slice completion events");
    const auto committed_stamp = first->fff_print()->step_state_with_timestamp(psGCodeExport).timestamp;
    require(manager.get_config().store_capability_config(id, {{"factor", .8}}), "Closing queue update failed");
    shutdown_plugin_slicing_gui();
    pump_for(std::chrono::milliseconds(400));
    require(first->is_slice_result_valid() && first->fff_print()->step_state_with_timestamp(psGCodeExport).timestamp == committed_stamp,
            "Queued callback ran after GUI bridge shutdown");
    require(manager.unload_plugin(id.plugin_key), "Finished GUI slice retained plugin pin");
    pump_for(std::chrono::milliseconds(200));
    evidence["queued_shutdown_safe"] = true;
    evidence["finished_slice_releases_plugin"] = true;
    evidence["ok"] = true;
    checkpoint("workflow_complete");
    return evidence;
}

bool plugin_texture_smoke_active() { return texture_observation != nullptr; }

void record_plugin_texture_preview(std::size_t triangles, bool rendered, unsigned gl_error)
{
    if (!texture_observation) return;
    auto& evidence = *texture_observation->evidence;
    evidence["gl_frames"] = evidence.value("gl_frames", size_t(0)) + 1;
    evidence["preview_triangles"] = triangles;
    evidence["preview_shader_drawn"] = rendered;
    evidence["preview_gl_error"] = gl_error;
    texture_observation->write();
}
void record_plugin_texture_source(const std::string& path, bool plugin_resource)
{
    if (!texture_observation) return;
    (*texture_observation->evidence)["preview_source"] = path;
    (*texture_observation->evidence)["preview_plugin_resource"] = plugin_resource;
    texture_observation->write();
}

nlohmann::json run_plugin_texture_smoke(const std::filesystem::path& report)
{
    wxString mode, isolated;
    require(wxIsMainThread() && wxGetEnv("ORCA_LOCAL_PLUGIN_SMOKE_MODE", &mode) && mode == "texture" &&
            wxGetEnv("ORCA_LOCAL_PLUGIN_SMOKE_DATADIR", &isolated) && !isolated.empty(), "Texture smoke requires explicit isolated GUI mode");
    require(std::filesystem::weakly_canonical(std::filesystem::u8path(isolated.ToUTF8().data())) ==
            std::filesystem::weakly_canonical(std::filesystem::u8path(data_dir())), "Texture smoke refused active user profile");
    auto* plater = wxGetApp().plater();
    auto* bundle = wxGetApp().preset_bundle;
    require(plater && bundle && wxGetApp().app_config, "GUI has no live Plater/presets");
    struct Cleanup {
        wxWeakRef<Plater> target;
        ~Cleanup() {
            texture_observation = nullptr;
            if (target) target->suppress_background_process(true);
            shutdown_plugin_slicing_gui();
        }
    } cleanup{plater};
    nlohmann::json evidence{{"real_plater", true}, {"texture_stages", nlohmann::json::array()}, {"editable_layers", true}};
    wxString color_setting;
    const bool require_color = wxGetEnv("ORCA_LOCAL_PLUGIN_SMOKE_TEXTURE_COLOR", &color_setting) && color_setting == "1";
    evidence["texture_color_required"] = require_color;
    wxString authoring_setting;
    const bool require_authoring = wxGetEnv("ORCA_LOCAL_PLUGIN_SMOKE_TEXTURE_AUTHORING", &authoring_setting) && authoring_setting == "1";
    evidence["texture_authoring_required"] = require_authoring;
    wxString image_setting;
    const bool require_image_adjustment = wxGetEnv("ORCA_LOCAL_PLUGIN_SMOKE_TEXTURE_IMAGE_ADJUST", &image_setting) && image_setting == "1";
    evidence["texture_image_adjustment_required"] = require_image_adjustment;
    TextureSmokeObservation observation{&evidence, std::filesystem::path(report.string() + ".texture")};
    texture_observation = &observation;
    const auto checkpoint = [&](const char* stage) {
        evidence["texture_stages"].push_back(stage);
        observation.write();
    };
    // The loader publishes its module on a worker and posts consumer observers
    // to wx. Native tests have no wx app and run those observers synchronously;
    // the real GUI must dispatch the queued load notification first.
    require(!PluginManager::instance().get_plugin_capabilities("", PluginCapabilityType::ResourceTexture).empty(),
            "Install a ResourceTexture fixture wheel before running texture smoke");
    wait_until(plater, [] {
        const auto entries = texture_library(true);
        return std::any_of(entries.begin(), entries.end(), [](const auto& entry) { return entry.source == TextureSource::Plugin; });
    }, "queued texture load observer");
    std::vector<std::string> plugin_choices;
    std::vector<TextureLibraryEntry> original_texture_entries;
    for (const auto& entry : texture_library(true))
        if (entry.source == TextureSource::Plugin) {
            plugin_choices.push_back(entry.pack_name + " / " + entry.name);
            original_texture_entries.push_back(entry);
        }
    require(!plugin_choices.empty(), "Install a ResourceTexture fixture wheel before running texture smoke");
    std::shared_ptr<std::vector<unsigned char>> original_image;
    if (require_image_adjustment) {
        require(original_texture_entries.size() == 1, "Image adjustment smoke requires one plugin source image");
        std::string error;
        original_image = load_texture_image_data(original_texture_entries.front(), error);
        require(bool(original_image), "Could not load original plugin image: " + error);
    }
    evidence["choose_plugin_texture"] = plugin_choices;
    wxGetApp().app_config->set("background_processing", "0");
    select_smoke_presets(*bundle);
    wxGetApp().mainframe->select_tab(MainFrame::tp3DEditor);
    plater->new_project(true, true, "Texture workflow smoke");
    auto& process = bundle->prints.get_edited_preset().config;
    process.set_key_value("slicing_pipeline_plugin", new ConfigOptionStrings());
    process.set_key_value("plugins", new ConfigOptionStrings());
    process.set_key_value("print_plugin_config_overrides", new ConfigOptionString("[]"));
    // A fresh profile deliberately starts with one selected PLA preset even on
    // the four-tool U1. Add the color fixture's slots through the real bundle
    // operation so auxiliary material/flush arrays are resized consistently.
    if (require_color) bundle->set_num_filaments(4);
    wxGetApp().load_current_presets(false, false);
    if (require_color) {
        require(bundle->filament_presets.size() >= 2, "Color fixture needs at least two physical filament slots");
        const std::vector<std::string> colors{"#FF0000", "#00FF00", "#0000FF", "#FFFFFF"};
        std::vector<std::string> palette;
        for (size_t i = 0; i < bundle->filament_presets.size(); ++i) palette.push_back(colors[i % colors.size()]);
        bundle->project_config.set_key_value("filament_colour", new ConfigOptionStrings(palette));
        evidence["texture_palette"] = palette;
    }
    plater->on_config_change(bundle->full_config());
    const auto model_path = std::filesystem::u8path(data_dir()) / "texture-workflow-cube.stl";
    auto cube = make_cube(8., 8., 8.);
    require(cube.write_binary(model_path.u8string().c_str()), "Could not write texture cube fixture");
    require(!plater->load_files(std::vector<std::string>{model_path.u8string()}, LoadStrategy::LoadModel).empty(), "Texture cube import failed");
    pump_for(std::chrono::milliseconds(800));
    require(plater->model().objects.size() == 1 && plater->model().objects.front()->volumes.size() == 1, "Unexpected texture fixture geometry");
    const auto volume = [&]() { return plater->model().objects.front()->volumes.front(); };
    const indexed_triangle_set original = volume()->mesh().its;
    const double original_volume = its_volume(original);
    // Seed real annotations and UVs. Topology-changing bake must transfer these,
    // while Undo restores the exact original channels.
    TriangleSelector selector(volume()->mesh());
    selector.set_facet(0, EnforcerBlockerType::ENFORCER);
    volume()->supported_facets.set(selector);
    volume()->seam_facets.set(selector);
    volume()->mmu_segmentation_facets.set(selector);
    volume()->fuzzy_skin_facets.set(selector);
    for (const auto& triangle : original.indices)
        for (int corner = 0; corner < 3; ++corner) {
            const auto& vertex = original.vertices[triangle[corner]];
            volume()->texture_uv.emplace_back(vertex.x() / 8.f, vertex.y() / 8.f);
        }
    const auto original_uv = volume()->texture_uv;
    plater->select_all();
    evidence["original_triangles"] = original.indices.size();
    checkpoint("texture_dialog_requested");
    // Select the plugin image, add a layer, Paint all, preview, then Bake into mesh.
    plater->apply_resource_texture();
    require(plater->model().objects.size() == 1, "Texture dialog lost its selected model");
    const auto authoring_state = [&] {
        auto state = nlohmann::json::parse(texture_displacement_layers_to_json(
            volume()->texture_displacement_layers, volume()->texture_displacement_options));
        // Archive location is assigned by the real 3MF writer. All editable values remain exact.
        for (auto& layer : state["layers"]) layer.erase("path_in_3mf");
        return state;
    };
    nlohmann::json saved_authoring;
    TriangleSelector::TriangleSplittingData saved_mask;
    std::vector<unsigned char> saved_image;
    const auto check_authoring = [&] {
        require(volume()->mesh().its.vertices == original.vertices && volume()->mesh().its.indices == original.indices,
                "Save editable layers changed the source mesh");
        require(authoring_state() == saved_authoring, "Editable seam/UV/group state changed during roundtrip");
        const auto& layer = volume()->texture_displacement_layers.front();
        require(layer.image_data && *layer.image_data == saved_image &&
                volume()->texture_displacement_facet(layer.slot).get_data() == saved_mask,
                "Editable image or painted mask changed during roundtrip");
        require(volume()->texture_uv == original_uv, "Editable roundtrip changed imported model UVs");
    };
    if (require_authoring) {
        require(volume()->texture_displacement_layers.size() == 1, "Save one editable layer before baking");
        const auto& layer = volume()->texture_displacement_layers.front();
        require(!layer.lscm_seam_edges.empty() && !layer.lscm_uv_overrides.empty(),
                "Authoring fixture requires actual manual seam and UV vertex/edge edits");
        require(layer.image_data && !volume()->texture_displacement_facet(layer.slot).empty(),
                "Authoring fixture requires an image and painted area");
        saved_authoring = authoring_state();
        saved_image = *layer.image_data;
        saved_mask = volume()->texture_displacement_facet(layer.slot).get_data();
        check_authoring();
        plater->undo();
        pump_for(std::chrono::milliseconds(200));
        require(volume()->texture_displacement_layers.empty() && volume()->texture_uv == original_uv,
                "Undo editable Save did not restore the original state");
        plater->redo();
        pump_for(std::chrono::milliseconds(200));
        check_authoring();
        const auto editable_project = std::filesystem::u8path(data_dir()) / "texture-authoring-editable.3mf";
        require(plater->export_3mf_copy(boost::filesystem::path(editable_project.u8string())), "Editable 3MF save failed");
        plater->new_project(true, true, "Editable texture roundtrip");
        require(!plater->load_files(std::vector<std::string>{editable_project.u8string()},
                LoadStrategy::LoadModel | LoadStrategy::LoadConfig | LoadStrategy::Silence).empty(), "Editable 3MF reload failed");
        pump_for(std::chrono::milliseconds(800));
        require(plater->model().objects.size() == 1 && plater->model().objects.front()->volumes.size() == 1,
                "Editable 3MF reload lost model structure");
        check_authoring();
        evidence["authoring_state"] = saved_authoring;
        evidence["authoring_saved_project"] = editable_project.u8string();
        evidence["authoring_roundtrip"] = true;
        checkpoint("editable_authoring_reopened");
        plater->select_all();
        // Inspect the actual reopened edits, then Preview and Bake without changing them.
        plater->apply_resource_texture();
    }
    const indexed_triangle_set baked = volume()->mesh().its;
    require(baked.indices.size() > original.indices.size() && its_volume(baked) > original_volume + .01,
            "Texture dialog was cancelled or did not replace the mesh with relief");
    if (!require_image_adjustment)
        require(evidence.value("preview_plugin_resource", false), "Select the installed plugin texture before pressing Apply");
    require(evidence.value("preview_shader_drawn", false) && evidence.value("preview_gl_error", 1u) == 0 &&
            evidence.value("preview_triangles", size_t(0)) == baked.indices.size(), "Baked mesh was not drawn by the actual GL preview");
    require(!volume()->supported_facets.empty() && !volume()->seam_facets.empty() && !volume()->mmu_segmentation_facets.empty() &&
            !volume()->fuzzy_skin_facets.empty(), "Bake did not transfer existing triangle annotations");
    require(volume()->texture_displacement_layers.size() == 1 && !volume()->texture_displacement_layers.front().empty(),
            "Bake did not preserve its editable layer and image");
    const auto baked_layer = volume()->texture_displacement_layers.front();
    const auto baked_state = authoring_state();
    evidence["baked_editable_state"] = baked_state;
    if (require_image_adjustment) {
        const auto saved_path = std::filesystem::u8path(baked_layer.path);
        require(std::filesystem::weakly_canonical(saved_path.parent_path()) ==
                std::filesystem::weakly_canonical(std::filesystem::u8path(user_texture_dir())) &&
                std::filesystem::is_regular_file(saved_path), "Save as copy must create an isolated user-library asset");
        require(baked_layer.image_data && *baked_layer.image_data != *original_image && baked_layer.preview_adjust.identity(),
                "Adjusted pixels must be embedded exactly once, without temporary adjustments");
        auto source_layer = baked_layer, adjusted_layer = baked_layer;
        source_layer.image_data = original_image;
        source_layer.smoothing = adjusted_layer.smoothing = 0.f;
        const auto source_pixels = decode_height_texture(source_layer);
        const auto adjusted_pixels = decode_height_texture(adjusted_layer);
        require(!source_pixels.empty() && !adjusted_pixels.empty() &&
                (source_pixels.pixels != adjusted_pixels.pixels || source_pixels.rgb != adjusted_pixels.rgb),
                "Saved image differs only in encoding; change its actual pixels");
        std::string error;
        const auto unchanged = load_texture_image_data(original_texture_entries.front(), error);
        require(unchanged && *unchanged == *original_image, "Image adjustment modified the plugin source image");
        evidence["image_copy_saved"] = baked_layer.path;
        evidence["plugin_source_image_unchanged"] = true;
        evidence["adjusted_pixels_embedded"] = true;
    }
    const auto baked_uv = volume()->texture_uv;
    const auto baked_mmu = volume()->mmu_segmentation_facets.get_data();
    if (require_color) {
        require(baked_layer.color_enabled, "Enable texture colors before previewing and baking the color fixture");
        TriangleSelector painted(volume()->mesh());
        painted.deserialize(baked_mmu);
        // Paint all must produce actual material assignments on every baked
        // triangle, not merely leave the single seeded face from the input.
        require(painted.get_facets_strict(EnforcerBlockerType::NONE).indices.empty(),
                "Color bake left unassigned faces despite Paint all");
        std::vector<int> used;
        size_t assigned_faces = 0;
        for (size_t index = 0; index < bundle->filament_presets.size(); ++index) {
            const size_t faces = painted.get_facets_strict(EnforcerBlockerType(index + 1)).indices.size();
            if (faces) used.push_back(int(index + 1));
            assigned_faces += faces;
        }
        require(assigned_faces == baked.indices.size(), "Color bake used a material outside the fixture palette");
        require(used.size() >= 2, "RGB fixture did not produce multiple real filament assignments");
        evidence["texture_color_filaments"] = used;
        evidence["texture_color_painted_faces"] = baked.indices.size();
    }
    require(baked_uv.size() == baked.indices.size() * 3, "Bake did not transfer model UVs");
    const auto check_editable_data = [&] {
        require(volume()->texture_displacement_layers.size() == 1, "Layer count changed during roundtrip");
        require(authoring_state() == baked_state, "Roundtrip changed texture layers or bake options");
        const auto& layer = volume()->texture_displacement_layers.front();
        require(layer.slot == baked_layer.slot && layer.name == baked_layer.name && layer.depth_mm == baked_layer.depth_mm &&
                layer.image_data && *layer.image_data == *baked_layer.image_data, "Roundtrip lost layer parameters or image bytes");
        require(layer.color_enabled == baked_layer.color_enabled && volume()->mmu_segmentation_facets.get_data() == baked_mmu,
                "Roundtrip changed texture color settings or material assignments");
        require(volume()->texture_displacement_facet(layer.slot).empty(), "Baked displacement mask was not consumed");
        require(volume()->texture_uv == baked_uv, "Roundtrip changed model UVs");
        require(!volume()->supported_facets.empty() && !volume()->seam_facets.empty() &&
                !volume()->mmu_segmentation_facets.empty() && !volume()->fuzzy_skin_facets.empty(),
                "Roundtrip lost transferred triangle annotations");
    };
    check_editable_data();
    const double baked_volume = its_volume(baked);
    evidence["baked_triangles"] = baked.indices.size();
    evidence["baked_volume_mm3"] = baked_volume;
    checkpoint("preview_and_apply_verified");
    plater->undo();
    pump_for(std::chrono::milliseconds(400));
    require(volume()->mesh().its.vertices == original.vertices && volume()->mesh().its.indices == original.indices,
            "Undo did not restore the original mesh");
    require(!volume()->supported_facets.empty() && !volume()->seam_facets.empty() && !volume()->mmu_segmentation_facets.empty() &&
            !volume()->fuzzy_skin_facets.empty(), "Undo did not restore original triangle annotations");
    if (require_authoring) check_authoring();
    else require(volume()->texture_displacement_layers.empty() && volume()->texture_uv == original_uv,
                 "Undo did not restore original editable data");
    plater->redo();
    pump_for(std::chrono::milliseconds(400));
    require(volume()->mesh().its.vertices == baked.vertices && volume()->mesh().its.indices == baked.indices, "Redo did not restore the baked mesh");
    check_editable_data();
    checkpoint("undo_redo_verified");
    const auto project = std::filesystem::u8path(data_dir()) / "texture-workflow.3mf";
    require(plater->export_3mf_copy(boost::filesystem::path(project.u8string())), "Plater 3MF save failed");
    require(std::filesystem::is_regular_file(project) && std::filesystem::file_size(project) > 0, "3MF project was not written");
    checkpoint("project_saved");
    plater->new_project(true, true, "Texture roundtrip");
    require(!plater->load_files(std::vector<std::string>{project.u8string()},
            LoadStrategy::LoadModel | LoadStrategy::LoadConfig | LoadStrategy::Silence).empty(), "Plater 3MF reload failed");
    pump_for(std::chrono::milliseconds(800));
    require(plater->model().objects.size() == 1 && plater->model().objects.front()->volumes.size() == 1, "3MF reload lost model structure");
    require(volume()->mesh().its.indices.size() == baked.indices.size() &&
            std::abs(its_volume(volume()->mesh().its) - baked_volume) < std::max(.001, baked_volume * 1e-5), "3MF did not preserve baked geometry");
    check_editable_data();
    evidence["editable_data_roundtrip"] = true;
    evidence["saved_project"] = project.u8string();
    checkpoint("project_reopened");
    auto* plate = plater->get_partplate_list().get_curr_plate();
    slice_current(plater, plate, -1., &evidence);
    evidence["reopened_gcode_moves"] = plate->get_slice_result()->moves.size();
    if (require_color) {
        std::set<unsigned> tools;
        for (const auto& move : plate->get_slice_result()->moves)
            if (move.type == EMoveType::Extrude) tools.insert(move.extruder_id);
        require(tools.size() >= 2, "Texture colors did not reach multi-tool G-code");
        evidence["texture_color_gcode_tools"] = tools;
    }
    evidence["ok"] = true;
    checkpoint("texture_workflow_complete");
    return evidence;
}
} // namespace Slic3r::GUI
