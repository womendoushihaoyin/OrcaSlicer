// Isolated acceptance fixture: real Plater slicing and the production final-file
// postprocessing entry. No independent Print or export-dialog claim is made.
#include "gui_legacy_states.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/plugin/PluginManager.hpp"
#include "slic3r/plugin/LocalPluginStore.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/GCode/PostProcessor.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Utils.hpp"
#include <wx/evtloop.h>
#include <wx/utils.h>
#include <wx/weakref.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <future>
#include <iterator>
#include <set>
#include <stdexcept>

namespace Slic3r::GUI {
namespace {
namespace fs = std::filesystem;
using json = nlohmann::json;
void require(bool value, const std::string& message) { if (!value) throw std::runtime_error(message); }
void write_json(const fs::path& path, const json& value)
{
    std::ofstream out(path); out << value.dump(2) << '\n';
    require(bool(out), "Cannot write legacy acceptance evidence: " + path.u8string());
}
std::string bytes(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    require(bool(in), "Cannot read evidence/input: " + path.u8string());
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
void pump()
{
    require(wxEventLoopBase::GetActive(), "Legacy fixture requires the real GUI event loop");
    wxEventLoopBase::GetActive()->YieldFor(wxEVT_CATEGORY_ALL); wxMilliSleep(10);
}
template<class Predicate> void wait(Plater* plater, Predicate ready, const char* description)
{
    wxWeakRef<Plater> target(plater);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
    while (!ready()) {
        require(bool(target), "Plater closed during legacy acceptance");
        require(std::chrono::steady_clock::now() < deadline, std::string("Timed out: ") + description);
        pump();
        require(bool(target), "Plater closed during legacy acceptance");
    }
}
json config_snapshot(const ConfigBase& config)
{
    json out = json::object();
    for (const auto& key : config.keys()) out[key] = config.opt_serialize(key);
    return out;
}
json model_snapshot(const Model& model)
{
    json objects = json::array();
    for (const auto* object : model.objects) {
        json volumes = json::array();
        for (const auto* volume : object->volumes) {
            std::vector<std::array<double, 9>> triangles;
            for (const auto* instance : object->instances) {
                const auto matrix = instance->get_matrix() * volume->get_matrix();
                const auto& mesh = volume->mesh().its;
                for (const auto& face : mesh.indices) {
                    std::array<std::array<double, 3>, 3> vertices;
                    for (int corner = 0; corner < 3; ++corner) {
                        const Vec3d point = matrix * mesh.vertices[face[corner]].cast<double>();
                        vertices[corner] = {point.x(), point.y(), point.z()};
                    }
                    // Cyclic rotations preserve winding; reversed triangles do not compare equal.
                    const auto first = std::min_element(vertices.begin(), vertices.end()) - vertices.begin();
                    std::array<double, 9> values;
                    for (int c = 0; c < 3; ++c) for (int a = 0; a < 3; ++a)
                        values[3*c+a] = vertices[(first+c)%3][a];
                    triangles.push_back(values);
                }
            }
            std::sort(triangles.begin(), triangles.end());
            volumes.push_back({{"type", int(volume->type())}, {"name", volume->name},
                {"config", config_snapshot(volume->config.get())}, {"world_triangles", triangles}});
        }
        objects.push_back({{"name", object->name}, {"instances", object->instances.size()},
            {"config", config_snapshot(object->config.get())}, {"volumes", volumes}});
    }
    return objects;
}
void select_presets(PresetBundle& bundle, const PluginCapabilityId* selected)
{
    const std::string printer = "Snapmaker U1 (0.4 nozzle)", process = "0.20mm Standard @Snapmaker U1 (0.4 nozzle)",
        filament = "Snapmaker PLA Basic @U1";
    wxGetApp().app_config->set_variant("Snapmaker", "Snapmaker U1", "0.4", true);
    if (!bundle.printers.find_preset(printer) || !bundle.prints.find_preset(process) || !bundle.filaments.find_preset(filament))
        bundle.load_presets(*wxGetApp().app_config, ForwardCompatibilitySubstitutionRule::EnableSilent);
    require(bundle.printers.find_preset(printer) && bundle.prints.find_preset(process) && bundle.filaments.find_preset(filament),
        "Bundled U1/PLA fixture presets unavailable");
    bundle.printers.find_preset(printer, false, true)->is_visible = true;
    bundle.prints.find_preset(process, false, true)->is_visible = true;
    bundle.filaments.find_preset(filament, false, true)->is_visible = true;
    require(bundle.printers.select_preset_by_name(printer, true) && bundle.prints.select_preset_by_name(process, true) &&
        bundle.filaments.select_preset_by_name(filament, true), "Cannot select fixed U1 presets");
    // Use the real count-changing entry so color modes, multi-colors and volume
    // types match the selected single filament before any slice or 3MF save.
    bundle.set_num_filaments(1);
    for (size_t i = 0; i < bundle.filament_presets.size(); ++i) bundle.set_filament_preset(i, filament);
    auto& cfg = bundle.prints.get_edited_preset().config;
    cfg.set_key_value("layer_height", new ConfigOptionFloat(.2));
    cfg.set_key_value("initial_layer_print_height", new ConfigOptionFloat(.2));
    cfg.set_key_value("elefant_foot_compensation", new ConfigOptionFloat(0.));
    cfg.set_key_value("wall_loops", new ConfigOptionInt(2));
    cfg.set_key_value("sparse_infill_density", new ConfigOptionPercent(15.));
    cfg.set_key_value("enable_support", new ConfigOptionBool(false));
    cfg.set_key_value("slicing_pipeline_plugin", new ConfigOptionStrings(selected ? std::vector<std::string>{selected->name} : std::vector<std::string>{}));
    cfg.set_key_value("plugins", new ConfigOptionStrings(selected ? std::vector<std::string>{selected->plugin_key + ";;" + selected->name} : std::vector<std::string>{}));
    cfg.set_key_value("print_plugin_config_overrides", new ConfigOptionString("[]"));
    wxGetApp().load_current_presets(false, false);
    wxGetApp().plater()->on_config_change(bundle.full_config());
}
json slice_and_export(Plater& plater, const fs::path& output, bool enabled)
{
    auto* plate = plater.get_partplate_list().get_curr_plate();
    require(plate && plate->can_slice() && !plate->is_slice_result_valid(), "Expected a fresh sliceable plate");
    const auto before = plate->fff_print()->step_state_with_timestamp(psGCodeExport).timestamp;
    plater.reslice();
    wait(&plater, [&] {
        require(!plate->is_apply_result_invalid(), "Plater rejected legacy fixture model/config");
        return plate->is_slice_result_valid() && plate->fff_print()->is_step_done(psGCodeExport) && !plater.is_background_process_slicing();
    }, "real legacy Plater slice/export");
    const auto* print = plate->fff_print();
    require(print->step_state_with_timestamp(psGCodeExport).timestamp != before, "Cached G-code was reused instead of slicing");
    require(plate->get_slice_result() && !plate->get_slice_result()->moves.empty(), "Actual G-code contains no moves");
    size_t layers = 0;
    for (const auto* object : print->objects()) for (const auto* layer : object->layers()) {
        ++layers;
        bool walls = false;
        for (const auto* region : layer->regions()) walls |= !region->perimeters.empty();
        require(walls, "Actual sliced layer has no native perimeters");
    }
    require(layers > 0, "Actual background slicing produced no layers");
    const auto actual = fs::u8path(plate->get_tmp_gcode_path());
    require(fs::is_regular_file(actual), "Actual final G-code file missing");
    const auto preview = fs::path(output).replace_extension(".preview.gcode");
    fs::copy_file(actual, preview, fs::copy_options::none);
    fs::copy_file(actual, output, fs::copy_options::none);
    const std::string marker = "; ORCA_DUALHOST contract-v1 token=37";
    const auto marker_count = [&](const fs::path& path) {
        const auto data = bytes(path); size_t count = 0, offset = 0;
        while ((offset = data.find(marker, offset)) != std::string::npos) { ++count; offset += marker.size(); }
        return count;
    };
    const auto preview_markers = marker_count(preview);
    require(preview_markers <= size_t(enabled), "Unexpected plugin marker in preview output");
    // U1 is non-BBL: preview slicing does not call final-file postprocessing.
    // Use the same public production entry as finalize_gcode, on a preserved
    // copy. This covers real Plater generation + real final-file processing,
    // not the export menu or its native SaveFileDialog.
    std::string route = "background_worker";
    if (preview_markers == 0) {
        route = "production_final_file_entry";
        const DynamicPrintConfig config = print->full_print_config();
        auto worker = std::async(std::launch::async, [config, output] {
            std::string path = output.u8string(), name = path;
            const bool processed = run_post_process_scripts(path, false, "File", name, config);
            require(path == output.u8string() && name == output.u8string(), "Canonical fixture unexpectedly renamed the output");
            return processed;
        });
        // A plugin can call host UI; do not block the GUI while it holds a GIL.
        wait(&plater, [&] { return worker.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready; }, "production final-file postprocessing");
        require(worker.get() == enabled, "Unexpected final-file dispatch selection");
    }
    require(marker_count(output) == size_t(enabled), "Final G-code lacks the unique expected canonical marker");
    return {{"gcode", output.u8string()}, {"actual_background_gcode", actual.u8string()},
        {"raw_preview", preview.u8string()}, {"postprocess_route", route}, {"marker_count", marker_count(output)},
        {"layers", layers}, {"moves", plate->get_slice_result()->moves.size()},
        {"export_timestamp_before", before}, {"export_timestamp_after", print->step_state_with_timestamp(psGCodeExport).timestamp},
        {"print_config", config_snapshot(print->full_print_config())}, {"model", model_snapshot(plater.model())}};
}
}

nlohmann::json run_plugin_legacy_states_smoke(const fs::path& fixture, const fs::path& report)
{
    wxString mode, isolated;
    require(wxIsMainThread() && wxGetEnv("ORCA_LOCAL_PLUGIN_SMOKE_MODE", &mode) && mode == "legacy_states" &&
        wxGetEnv("ORCA_LOCAL_PLUGIN_SMOKE_DATADIR", &isolated) && !isolated.empty() &&
        fs::weakly_canonical(fs::u8path(isolated.ToUTF8().data())) == fs::weakly_canonical(fs::u8path(data_dir())),
        "Legacy states requires explicit isolated GUI mode");
    require(fixture.filename() == "orca_dualhost_multi-1.0.0-py3-none-any.whl", "Use the unchanged canonical multi wheel");
    const auto model_file = fixture.parent_path() / "reference-cube.stl";
    require(fs::is_regular_file(model_file), "Canonical reference STL missing");
    auto* plater = wxGetApp().plater(); auto* bundle = wxGetApp().preset_bundle;
    require(plater && bundle && wxGetApp().app_config, "Live Plater/presets unavailable");
    struct Cleanup { wxWeakRef<Plater> plater; ~Cleanup() { if (plater) plater->suppress_background_process(true); } } cleanup{plater};
    auto& manager = PluginManager::instance();
    LocalPluginStore store(fs::u8path(get_orca_plugins_dir()), {});
    std::string error;
    require(manager.get_plugin_descriptors(true).empty() && store.scan(error).empty() && error.empty() &&
        manager.get_plugin_capabilities("", PluginCapabilityType::Unknown, false).empty(), "Uninstalled baseline is not empty");
    const auto output = report.parent_path() / "legacy-states";
    require(!fs::exists(output), "Refusing to reuse legacy evidence/profile"); fs::create_directories(output);
    json evidence{{"real_plater", true}, {"legacy_states", json::array()}, {"legacy_checkpoints", json::array()},
        {"input_wheel", fixture.u8string()}, {"input_stl", model_file.u8string()},
        {"empty_initial_catalog", true}, {"normalization", "Verifier: header timestamp, exact plugin selectors/marker only; model world coordinates tolerance 1e-6 mm"}};
    const auto checkpoint = [&](const char* value) {
        evidence["legacy_checkpoints"].push_back(value); write_json(fs::path(report).concat(".legacy"), evidence);
    };
    wxGetApp().app_config->set("background_processing", "0");
    wxGetApp().mainframe->select_tab(MainFrame::tp3DEditor);
    PluginDescriptor installed; PluginCapabilityId slice_id; fs::path trace;
    const auto project = fs::u8path(data_dir()) / "legacy-project.3mf";
    for (const std::string state : {"uninstalled", "disabled", "enabled"}) {
        if (state == "disabled") {
            require(store.install(fixture, installed, error), "Install failed: " + error);
            require(bytes(fs::u8path(installed.entry_path)) == bytes(fixture), "Installed wheel bytes differ from canonical input");
            require(store.set_enabled(installed.plugin_key, false, error), "Disable persistence failed: " + error);
            require(manager.try_get_valid_plugin_descriptor(installed.plugin_key, installed) && !installed.enabled &&
                !manager.is_plugin_loaded(installed.plugin_key) && manager.get_plugin_capabilities("", PluginCapabilityType::Unknown, false).empty(),
                "Installed-disabled package is unexpectedly live");
            trace = fs::u8path(manager.get_storage_dir(installed.plugin_key)) / "dualhost-events.jsonl";
            require(!fs::exists(trace), "Installing/disabled package executed plugin callbacks");
            evidence["installed_entry"] = installed.entry_path; evidence["plugin_key"] = installed.plugin_key;
            evidence["trace"] = trace.u8string(); checkpoint("installed_disabled_without_python_callbacks");
        } else if (state == "enabled") {
            require(store.set_enabled(installed.plugin_key, true, error), "Enable persistence failed: " + error);
            manager.load_plugin(installed.plugin_key);
            wait(plater, [&] { return !manager.is_plugin_load_in_progress(installed.plugin_key); }, "canonical multi capability load");
            require(manager.is_plugin_loaded(installed.plugin_key), "Canonical load failed: " + manager.get_plugin_load_error(installed.plugin_key));
            std::set<PluginCapabilityType> types;
            { const auto caps = manager.get_plugin_capabilities(installed.plugin_key);
              require(caps.size() == 4, "Enabled multi package did not expose four capabilities");
              for (const auto& cap : caps) { const auto id = cap->identity(); types.insert(id.type); if (id.type == PluginCapabilityType::SlicingPipeline) slice_id = id; } }
            require(types == std::set<PluginCapabilityType>{PluginCapabilityType::Script, PluginCapabilityType::Pages,
                PluginCapabilityType::SlicingPipeline, PluginCapabilityType::ResourceTexture} && slice_id.name == "Dualhost Slice", "Wrong canonical capability set");
            evidence["enabled_capability_count"] = 4; checkpoint("four_capabilities_loaded");
        }
        plater->new_project(true, true, "Legacy plugin states");
        select_presets(*bundle, state == "enabled" ? &slice_id : nullptr);
        require(!plater->load_files(std::vector<std::string>{model_file.u8string()}, LoadStrategy::LoadModel).empty(), "Real STL import failed");
        auto entry = slice_and_export(*plater, output / (state + ".gcode"), state == "enabled");
        entry["state"] = state;
        entry["registered_packages"] = manager.get_plugin_descriptors(true).size();
        entry["loaded_capabilities"] = manager.get_plugin_capabilities("", PluginCapabilityType::Unknown, false).size();
        entry["trace_exists"] = !trace.empty() && fs::exists(trace);
        if (state != "enabled") require(!entry["trace_exists"].get<bool>(), "Inactive state ran plugin callbacks");
        plater->set_project_filename(wxString::FromUTF8(project.u8string()));
        require(plater->save_project(false) == wxID_YES && fs::is_regular_file(project), "Actual Save project failed");
        const auto saved = output / (state + ".3mf"); fs::copy_file(project, saved, fs::copy_options::none);
        entry["project"] = saved.u8string(); evidence["legacy_states"].push_back(std::move(entry)); checkpoint(state.c_str());
    }
    // Reopen only the enabled project, without reapplying any selector or preset.
    plater->new_project(true, true, "Legacy plugin states");
    require(!plater->load_files(std::vector<std::string>{project.u8string()},
        LoadStrategy::LoadModel | LoadStrategy::LoadConfig | LoadStrategy::Silence).empty(), "Actual enabled 3MF reopen failed");
    auto reopened = slice_and_export(*plater, output / "reopened.gcode", true);
    reopened["state"] = "reopened"; evidence["legacy_states"].push_back(std::move(reopened));
    evidence["slicing_name"] = slice_id.name; evidence["ok"] = true; checkpoint("enabled_project_reopened_and_sliced");
    return evidence;
}
} // namespace Slic3r::GUI
