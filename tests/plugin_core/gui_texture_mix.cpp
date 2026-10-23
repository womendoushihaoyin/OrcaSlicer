// Runs only in the explicitly selected, isolated GUI smoke mode. The two modal
// dialogs remain real user interactions; all assertions use normal public APIs.
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/Selection.hpp"
#include "slic3r/GUI/PluginSlicingGui.hpp"
#include "slic3r/GUI/TextureEditSession.hpp"
#include "slic3r/GUI/TextureLibrary.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/Utils/UndoRedo.hpp"
#include "slic3r/plugin/PluginManager.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Utils.hpp"
#include <wx/button.h>
#include <wx/dialog.h>
#include <wx/evtloop.h>
#include <wx/timer.h>
#include <wx/weakref.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <functional>
#include <fstream>
#include <limits>
#include <set>
#include <stdexcept>

namespace Slic3r::GUI {
namespace {
void require(bool ok, const std::string& message)
{
    if (!ok) throw std::runtime_error("Mixed texture GUI: " + message);
}
void pump_for(std::chrono::milliseconds duration)
{
    const auto until = std::chrono::steady_clock::now() + duration;
    do {
        if (auto* loop = wxEventLoopBase::GetActive()) loop->YieldFor(wxEVT_CATEGORY_ALL);
        wxMilliSleep(5);
    } while (std::chrono::steady_clock::now() < until);
}
template<class Predicate> void wait_until(Plater& plater, Predicate ready, const char* description)
{
    wxWeakRef<Plater> alive(&plater);
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(120);
    while (!ready()) {
        require(alive && std::chrono::steady_clock::now() < end, description);
        pump_for(std::chrono::milliseconds(10));
        require(bool(alive), "Plater was closed during the test");
    }
}
std::string field(const DynamicPrintConfig& config, const char* key)
{
    return config.has(key) ? "present:" + config.opt_serialize(key) : "absent";
}
struct PaletteState
{
    std::vector<MixedFilament> rows;
    std::vector<std::string> display, presets, physical, plater_colors;
    std::vector<unsigned> sidebar_ids;
    std::string project_definitions, print_definitions;
};
PaletteState palette_state(Plater& plater)
{
    const auto& bundle = *wxGetApp().preset_bundle;
    PaletteState state;
    state.rows = bundle.mixed_filaments.mixed_filaments();
    state.display = bundle.mixed_filaments.display_colors();
    state.presets = bundle.filament_presets;
    state.physical = bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values;
    state.plater_colors = plater.get_extruder_colors_from_plater_config();
    state.sidebar_ids = wxGetApp().sidebar().get_ui_ordered_filament_ids();
    state.project_definitions = field(bundle.project_config, "mixed_filament_definitions");
    state.print_definitions = field(bundle.prints.get_edited_preset().config, "mixed_filament_definitions");
    return state;
}
void check_palette(Plater& plater, const PaletteState& expected, const char* context)
{
    const auto actual = palette_state(plater);
    require(actual.rows == expected.rows && actual.display == expected.display && actual.presets == expected.presets &&
            actual.physical == expected.physical && actual.plater_colors == expected.plater_colors &&
            actual.sidebar_ids == expected.sidebar_ids && actual.project_definitions == expected.project_definitions &&
            actual.print_definitions == expected.print_definitions, std::string(context) + " changed the palette, sidebar or definition fields");
}
ModelVolume& first_volume(Plater& plater)
{
    require(!plater.model().objects.empty() && plater.model().objects.front()->volumes.size() == 1, "Unexpected fixture structure");
    return *plater.model().objects.front()->volumes.front();
}
nlohmann::json editable_state(const ModelVolume& volume)
{
    auto result = nlohmann::json::parse(texture_displacement_layers_to_json(volume.texture_displacement_layers,
                                                                          volume.texture_displacement_options));
    for (auto& layer : result["layers"]) layer.erase("path_in_3mf");
    return result;
}
struct VolumeState
{
    indexed_triangle_set mesh;
    std::vector<Vec2f> uv;
    nlohmann::json editable;
    std::vector<std::vector<unsigned char>> images;
    std::vector<TriangleSelector::TriangleSplittingData> masks;
    TriangleSelector::TriangleSplittingData mmu;
    ObjectID id;
    Transform3d volume_matrix;
    std::vector<Transform3d> instance_matrices;
    explicit VolumeState(const ModelVolume& volume) : mesh(volume.mesh().its), uv(volume.texture_uv),
        editable(editable_state(volume)), mmu(volume.mmu_segmentation_facets.get_data()), id(volume.id()),
        volume_matrix(volume.get_matrix())
    {
        for (const auto* instance : volume.get_object()->instances) instance_matrices.push_back(instance->get_matrix());
        for (const auto& layer : volume.texture_displacement_layers)
            images.push_back(layer.image_data ? *layer.image_data : std::vector<unsigned char>{});
        for (int slot = 0; slot < 8; ++slot) masks.push_back(volume.texture_displacement_facet(slot).get_data());
    }
    struct ArchiveGeometry {
        indexed_triangle_set mesh;
        std::vector<Transform3d> world_matrices;
        Vec3d recenter_shift = Vec3d::Zero();
        double bed_shift = 0.;
    };
    ArchiveGeometry archive_geometry() const
    {
        TriangleMesh centered(mesh);
        ArchiveGeometry expected;
        expected.recenter_shift = centered.bounding_box().center();
        if (!expected.recenter_shift.isApprox(Vec3d::Zero()))
            centered.translate(-float(expected.recenter_shift.x()), -float(expected.recenter_shift.y()), -float(expected.recenter_shift.z()));
        expected.mesh = std::move(centered.its);
        for (const auto& instance : instance_matrices) {
            Transform3d world = instance * volume_matrix;
            if (!expected.recenter_shift.isApprox(Vec3d::Zero())) world.translate(expected.recenter_shift);
            expected.world_matrices.push_back(world);
        }
        require(!expected.world_matrices.empty(), "Archive fixture has no instance");
        // Plater calls ensure_on_bed(true) on this single-part fixture. Its
        // first-instance Z range is computed before applying the translation.
        const Transform3d& world = expected.world_matrices.front();
        const Vec3d z_row = world.linear().row(2).transpose();
        double min_z = std::numeric_limits<double>::max(), max_z = -min_z;
        for (const auto& point : expected.mesh.vertices) {
            const double z = z_row.dot(point.cast<double>());
            min_z = std::min(min_z, z); max_z = std::max(max_z, z);
        }
        min_z += world.translation().z(); max_z += world.translation().z();
        if (min_z >= SINKING_Z_THRESHOLD || max_z < 0.) {
            expected.bed_shift = -min_z;
            for (auto& placement : expected.world_matrices) placement.translation().z() += expected.bed_shift;
        }
        return expected;
    }
    nlohmann::json archive_geometry_report(const ModelVolume& volume) const
    {
        const auto expected = archive_geometry();
        const auto& actual = volume.mesh().its;
        const auto matrix_json = [](const Transform3d& matrix) {
            auto result = nlohmann::json::array();
            for (int row = 0; row < 4; ++row) {
                auto values = nlohmann::json::array();
                for (int col = 0; col < 4; ++col) values.push_back(matrix.matrix()(row, col));
                result.push_back(values);
            }
            return result;
        };
        nlohmann::json report{{"expected_vertices", mesh.vertices.size()}, {"actual_vertices", actual.vertices.size()},
            {"expected_triangles", mesh.indices.size()}, {"actual_triangles", actual.indices.size()},
            {"triangle_order_exact", actual.indices == mesh.indices}, {"recentered_vertices_exact", actual.vertices == expected.mesh.vertices},
            {"recenter_shift", {expected.recenter_shift.x(), expected.recenter_shift.y(), expected.recenter_shift.z()}},
            {"expected_bed_shift", expected.bed_shift}, {"before_volume_matrix", matrix_json(volume_matrix)},
            {"loaded_volume_matrix", matrix_json(volume.get_matrix())}};
        report["instances"] = nlohmann::json::array();
        double max_local_delta = 0., max_world_delta = 0.;
        for (size_t i = 0; i < std::min(mesh.vertices.size(), actual.vertices.size()); ++i)
            max_local_delta = std::max(max_local_delta, (actual.vertices[i].cast<double>() - mesh.vertices[i].cast<double>()).cwiseAbs().maxCoeff());
        const auto& instances = volume.get_object()->instances;
        for (size_t i = 0; i < std::min(instance_matrices.size(), instances.size()); ++i) {
            const Transform3d before = instance_matrices[i] * volume_matrix;
            const Transform3d after = instances[i]->get_matrix() * volume.get_matrix();
            for (size_t j = 0; j < std::min(mesh.vertices.size(), actual.vertices.size()); ++j)
                max_world_delta = std::max(max_world_delta, (after * actual.vertices[j].cast<double>() - before * mesh.vertices[j].cast<double>()).cwiseAbs().maxCoeff());
            report["instances"].push_back({{"before_matrix", matrix_json(instance_matrices[i])},
                {"loaded_matrix", matrix_json(instances[i]->get_matrix())}, {"expected_world_matrix", matrix_json(expected.world_matrices[i])}});
        }
        report["max_local_delta_mm"] = max_local_delta; report["max_world_delta_mm"] = max_world_delta;
        return report;
    }
    nlohmann::json archive_mask_report(const ModelVolume& volume) const
    {
        const auto describe = [](const TriangleSelector::TriangleSplittingData& data) {
            auto states = nlohmann::json::array();
            for (size_t i = 0; i < data.used_states.size(); ++i) if (data.used_states[i]) states.push_back(i);
            return nlohmann::json{{"triangle_mappings", data.triangles_to_split.size()},
                {"bit_count", data.bitstream.size()}, {"cached_states", states},
                {"has_enforcer", TriangleSelector::has_facets(data, EnforcerBlockerType::ENFORCER)}};
        };
        auto report = nlohmann::json::array();
        for (int slot = 0; slot < 8; ++slot) {
            const auto& before = masks[size_t(slot)];
            const auto& after = volume.texture_displacement_facet(slot).get_data();
            auto fresh = after;
            fresh.reset_used_states(); fresh.update_used_states(0);
            report.push_back({{"slot", slot}, {"before", describe(before)}, {"loaded", describe(after)},
                {"encoded_mask_exact", before.triangles_to_split == after.triangles_to_split && before.bitstream == after.bitstream},
                {"loaded_cache_is_fresh", after.used_states == fresh.used_states}});
        }
        return report;
    }
    void check(const ModelVolume& volume, bool archive = false) const
    {
        require(archive || volume.id() == id, "History changed the selected volume ID");
        if (archive) {
            const auto expected = archive_geometry();
            // BBS3MF add_volume() recenters the local float mesh, and its
            // single-volume normalization can move the offset into instances.
            // Require the exact recentered mesh and compensated world matrix.
            require(volume.mesh().its.indices == expected.mesh.indices && volume.mesh().its.vertices == expected.mesh.vertices,
                    "Archive mesh differs from the exact BBS3MF recentering result");
            const auto& instances = volume.get_object()->instances;
            require(instances.size() == expected.world_matrices.size(), "Archive instance count changed");
            for (size_t i = 0; i < instances.size(); ++i)
                require((instances[i]->get_matrix() * volume.get_matrix()).matrix().isApprox(expected.world_matrices[i].matrix(), 1e-12),
                        "Archive placement did not compensate mesh recentering and bed alignment");
        } else {
            require(volume.mesh().its.indices == mesh.indices && volume.mesh().its.vertices == mesh.vertices,
                    "History changed the expected mesh");
        }
        require(volume.texture_uv == uv && editable_state(volume) == editable &&
                volume.mmu_segmentation_facets.get_data() == mmu, "Editable/UV/MMU state did not roundtrip exactly");
        require(volume.texture_displacement_layers.size() == images.size(), "Layer count changed");
        for (size_t i = 0; i < images.size(); ++i) {
            const auto& image = volume.texture_displacement_layers[i].image_data;
            require(image && *image == images[i], "Embedded image changed");
        }
        for (int slot = 0; slot < 8; ++slot) {
            auto expected = masks[size_t(slot)];
            if (archive) {
                // 3MF stores the triangle mapping and bitstream, not this
                // derived cache. Legacy FacetsAnnotation::reset() can leave
                // cached states set after clearing all encoded paint.
                expected.reset_used_states(); expected.update_used_states(0);
            }
            require(volume.texture_displacement_facet(slot).get_data() == expected,
                    "Layer mask changed at slot " + std::to_string(slot));
        }
    }
};
wxButton* find_bake(wxWindow* window)
{
    if (auto* button = dynamic_cast<wxButton*>(window); button && button->GetLabel() == _L("Bake into mesh")) return button;
    for (auto* child : window->GetChildren()) if (auto* found = find_bake(child)) return found;
    return nullptr;
}
// No callbacks are injected into the editor. The timer observes real public wx
// state while its nested modal loop runs, including asynchronous preview work.
class ModalMonitor final : public wxTimer
{
public:
    ModalMonitor(Plater& plater, std::function<void()> checkpoint) : m_plater(plater), m_checkpoint(std::move(checkpoint)),
        m_palette(palette_state(plater)), m_volume(first_volume(plater)),
        m_project(wxGetApp().preset_bundle->project_config), m_print(wxGetApp().preset_bundle->prints.get_edited_preset().config),
        m_timestamp(plater.undo_redo_stack_main().active_snapshot_time()) { Start(50); }
    void Notify() override
    {
        wxDialog* dialog = nullptr;
        for (auto* window : wxTopLevelWindows)
            if (auto* candidate = dynamic_cast<wxDialog*>(window); candidate && candidate->IsModal() &&
                candidate->GetTitle() == _L("Edit texture layers")) { dialog = candidate; break; }
        if (!dialog) return; // Publication happens only after the modal closes.
        try {
            check_unchanged();
            ++samples;
            if (auto* button = find_bake(dialog); button && button->IsEnabled() && !preview_ready) {
                preview_ready = true;
                m_checkpoint();
            }
        } catch (const std::exception& exception) {
            failure = exception.what();
            Stop();
            dialog->EndModal(wxID_CANCEL);
        }
    }
    void check_unchanged() const
    {
        check_palette(m_plater, m_palette, "Preview/Cancel");
        m_volume.check(first_volume(m_plater));
        require(wxGetApp().preset_bundle->project_config == m_project &&
                wxGetApp().preset_bundle->prints.get_edited_preset().config == m_print,
                "Preview changed a source project or print setting");
        require(m_plater.undo_redo_stack_main().active_snapshot_time() == m_timestamp,
                "Preview changed the formal history stack");
    }
    void finish(bool canceled)
    {
        Stop();
        require(failure.empty(), failure);
        require(samples > 0 && preview_ready, "Press Preview displacement and wait for Bake to become available");
        if (canceled) check_unchanged();
    }
    size_t samples = 0;
    bool preview_ready = false;
    std::string failure;
private:
    Plater& m_plater;
    std::function<void()> m_checkpoint;
    PaletteState m_palette;
    VolumeState m_volume;
    DynamicPrintConfig m_project, m_print;
    size_t m_timestamp;
};
void select_presets(PresetBundle& bundle)
{
    const std::string printer = "Snapmaker U1 (0.4 nozzle)";
    const std::string process = "0.20mm Standard @Snapmaker U1 (0.4 nozzle)";
    const std::string filament = "Snapmaker PLA Basic @U1";
    wxGetApp().app_config->set_variant("Snapmaker", "Snapmaker U1", "0.4", true);
    if (!bundle.printers.find_preset(printer) || !bundle.prints.find_preset(process) || !bundle.filaments.find_preset(filament))
        bundle.load_presets(*wxGetApp().app_config, ForwardCompatibilitySubstitutionRule::EnableSilent);
    require(bundle.printers.find_preset(printer) && bundle.prints.find_preset(process) && bundle.filaments.find_preset(filament),
            "Bundled U1/PLA presets are unavailable");
    bundle.printers.find_preset(printer, false, true)->is_visible = true;
    bundle.prints.find_preset(process, false, true)->is_visible = true;
    bundle.filaments.find_preset(filament, false, true)->is_visible = true;
    require(bundle.printers.select_preset_by_name(printer, true) && bundle.prints.select_preset_by_name(process, true) &&
            bundle.filaments.select_preset_by_name(filament, true), "Could not select real presets");
    bundle.filament_presets = {filament};
    bundle.update_multi_material_filament_presets();
    for (size_t i = 0; i < bundle.filament_presets.size(); ++i) bundle.set_filament_preset(i, filament);
}
void jump_to(Plater& plater, size_t timestamp)
{
    const auto& stack = plater.undo_redo_stack_main();
    const auto& snapshots = stack.snapshots();
    auto index_of = [&](size_t target) {
        const auto it = std::find_if(snapshots.begin(), snapshots.end(), [=](const auto& s) { return s.timestamp == target; });
        require(it != snapshots.end(), "Expected formal snapshot was discarded unexpectedly");
        return int(it - snapshots.begin());
    };
    const int current = index_of(stack.active_snapshot_time()), target = index_of(timestamp);
    if (target < current) plater.undo_to(current - target - 1);
    else if (target > current) plater.redo_to(target - current - 1);
    pump_for(std::chrono::milliseconds(250));
    require(stack.active_snapshot_time() == timestamp, "Formal history jump did not reach its target");
}
void rename_fixture(Plater& plater, const char* snapshot, const char* name)
{
    plater.take_snapshot(snapshot);
    plater.model().objects.front()->name = name;
    plater.changed_object(0);
    pump_for(std::chrono::milliseconds(200));
}
void assert_fresh_history(Plater& plater)
{
    for (const auto& snapshot : plater.undo_redo_stack_main().snapshots())
        require(snapshot.name.find("Mix smoke ") == std::string::npos, "Different project retained old mixed history");
}
} // namespace

nlohmann::json run_plugin_texture_mix_smoke(const std::filesystem::path& report)
{
    wxString mode, isolated, mixed;
    require(wxIsMainThread() && wxGetEnv("ORCA_LOCAL_PLUGIN_SMOKE_MODE", &mode) && mode == "texture" &&
            wxGetEnv("ORCA_LOCAL_PLUGIN_SMOKE_TEXTURE_MIX", &mixed) && mixed == "1" &&
            wxGetEnv("ORCA_LOCAL_PLUGIN_SMOKE_DATADIR", &isolated) && !isolated.empty(), "Explicit isolated mixed mode is required");
    require(std::filesystem::weakly_canonical(std::filesystem::u8path(isolated.ToUTF8().data())) ==
            std::filesystem::weakly_canonical(std::filesystem::u8path(data_dir())), "Refusing the active user profile");
    auto* plater = wxGetApp().plater();
    auto* bundle = wxGetApp().preset_bundle;
    require(plater && bundle && wxGetApp().app_config, "Live Plater/presets are required");
    struct Cleanup {
        wxWeakRef<Plater> plater;
        bool auto_generate;
        ~Cleanup() {
            if (plater) plater->suppress_background_process(true);
            MixedFilamentManager::set_auto_generate_enabled(auto_generate);
            shutdown_plugin_slicing_gui();
        }
    } cleanup{plater, MixedFilamentManager::auto_generate_enabled()};
    MixedFilamentManager::set_auto_generate_enabled(false);
    nlohmann::json evidence{{"real_plater", true}, {"texture_mix_required", true}, {"texture_stages", nlohmann::json::array()}};
    const auto checkpoint = [&](const char* stage) {
        evidence["texture_stages"].push_back(stage);
        std::ofstream output(report.string() + ".texture-mix"); output << evidence.dump(2) << '\n';
    };
    require(!PluginManager::instance().get_plugin_capabilities("", PluginCapabilityType::ResourceTexture).empty(),
            "Install the dedicated ResourceTexture gradient fixture wheel");
    wait_until(*plater, [] {
        const auto entries = texture_library(true);
        return std::any_of(entries.begin(), entries.end(), [](const auto& e) {
            return e.source == TextureSource::Plugin && e.pack_name == "Texture mixed gradient";
        });
    }, "Waiting for the actual ResourceTexture library");
    std::vector<TextureLibraryEntry> entries;
    for (const auto& entry : texture_library(true))
        if (entry.source == TextureSource::Plugin && entry.pack_name == "Texture mixed gradient") entries.push_back(entry);
    require(entries.size() == 1, "Expected one dedicated plugin gradient");
    std::string error;
    const auto image = load_texture_image_data(entries.front(), error); // Capability pin and path validation are mandatory.
    require(bool(image), "Plugin library image could not be read: " + error);
    evidence["plugin_resource"] = {{"plugin_key", entries.front().plugin_key}, {"capability", entries.front().capability_name},
                                    {"pack", entries.front().pack_name}, {"path", entries.front().path}, {"bytes", image->size()}};
    wxGetApp().app_config->set("background_processing", "0");
    wxGetApp().save_mode(comAdvanced);
    select_presets(*bundle);
    wxGetApp().mainframe->select_tab(MainFrame::tp3DEditor);
    plater->new_project(true, true, "Mixed texture GUI acceptance");
    auto& process = bundle->prints.get_edited_preset().config;
    process.set_key_value("slicing_pipeline_plugin", new ConfigOptionStrings());
    process.set_key_value("plugins", new ConfigOptionStrings());
    process.set_key_value("print_plugin_config_overrides", new ConfigOptionString("[]"));
    process.set_key_value("mixed_filament_gradient_mode", new ConfigOptionBool(false));
    process.set_key_value("mixed_filament_advanced_dithering", new ConfigOptionBool(false));
    bundle->set_num_filaments(4);
    for (size_t i = 0; i < bundle->filament_presets.size(); ++i)
        bundle->set_filament_preset(i, "Snapmaker PLA Basic @U1");
    wxGetApp().load_current_presets(false, false);
    require(bundle->filament_presets.size() == 4, "Real U1 fixture must retain all four physical slots");
    const std::vector<std::string> physical{"#FF0000", "#00FF00", "#0000FF", "#FFFFFF"};
    bundle->project_config.set_key_value("filament_colour", new ConfigOptionStrings(physical));
    plater->on_config_change(bundle->full_config());
    wxGetApp().sidebar().update_mixed_filament_panel();
    wxGetApp().sidebar().update_dynamic_filament_list();
    plater->update_filament_colors_in_full_config();
    require(bundle->mixed_filaments.enabled_count() == 0, "Isolated fixture unexpectedly started with mixed recipes");
    const auto full = bundle->full_config();
    require(full.option<ConfigOptionFloats>("filament_diameter")->values.size() == 4 &&
            full.option<ConfigOptionFloatOrPercent>("outer_wall_line_width")->get_abs_value(.4) > 0.,
            "Fixture must use complete physical vectors and a real nonzero U1 wall width");
    const auto root = std::filesystem::u8path(data_dir());
    const auto cube_path = root / "mixed-gradient-cube.stl";
    require(make_cube(8., 8., 3.).write_binary(cube_path.u8string().c_str()), "Could not write the model fixture");
    require(!plater->load_files(std::vector<std::string>{cube_path.u8string()}, LoadStrategy::LoadModel).empty(), "Could not import the cube");
    pump_for(std::chrono::milliseconds(800));
    auto& volume = first_volume(*plater);
    // Constant UV samples an actual mixture throughout the closed solid. Thus
    // both physical tools in final G-code must come from a generated recipe.
    volume.texture_uv.assign(volume.mesh().its.indices.size() * 3, Vec2f(.5f, .65f));
    TextureEditSession session(volume);
    const int slot = session.add_layer(entries.front().name, entries.front().path, image);
    auto* layer = session.layer(slot);
    layer->color_enabled = true; layer->projection_method = TextureProjectionMethod::ModelUV;
    layer->tiling_scale = 1.f; layer->depth_mm = .1f;
    session.options.color_mix_enabled = true; session.options.color_mix_count = 4;
    session.options.v2_refine_mm = 1.f; session.options.v2_max_triangles_k = 20; session.options.color_despeckle = 0;
    require(!analyze_texture_detail(*layer).flat_colors, "Dedicated plugin image must exercise nonflat recipe planning");
    TriangleSelector mask(volume.mesh());
    for (size_t face = 0; face < volume.mesh().its.indices.size(); ++face) mask.set_facet(int(face), EnforcerBlockerType::ENFORCER);
    session.masks[slot] = mask.serialize();
    session.apply(volume);
    plater->changed_object(0); plater->select_all();
    pump_for(std::chrono::milliseconds(800));
    const auto original_name = plater->model().objects.front()->name;
    const auto original_timestamp = plater->undo_redo_stack_main().active_snapshot_time();
    rename_fixture(*plater, "Mix smoke before edit", "before-mix");
    const auto before_timestamp = plater->undo_redo_stack_main().active_snapshot_time();
    const PaletteState before_palette = palette_state(*plater);
    const VolumeState before_volume(first_volume(*plater));
    evidence["physical_colors"] = physical;
    evidence["original_triangles"] = before_volume.mesh.indices.size();
    evidence["sidebar_before"] = before_palette.sidebar_ids;
    checkpoint("dialog_one_preview_then_cancel");
    {
        ModalMonitor monitor(*plater, [&] { checkpoint("first_preview_ready_source_unchanged"); });
        plater->apply_resource_texture();
        monitor.finish(true);
        evidence["cancel_monitor_samples"] = monitor.samples;
    }
    evidence["preview_cancel_unchanged"] = true;
    // Revalidate the live plugin capability before entering the second dialog.
    const auto image_again = load_texture_image_data(entries.front(), error);
    require(image_again && *image_again == *image, "Plugin resource changed between the actual editor invocations");
    checkpoint("dialog_two_preview_then_bake");
    {
        ModalMonitor monitor(*plater, [&] { checkpoint("second_preview_ready_source_unchanged"); });
        plater->apply_resource_texture();
        monitor.finish(false);
        evidence["bake_monitor_samples"] = monitor.samples;
    }
    require(bundle->mixed_filaments.enabled_count() > 0, "Real Plater Bake did not publish any new mixed recipe");
    const VolumeState baked(first_volume(*plater));
    const PaletteState after_palette = palette_state(*plater);
    require(baked.mesh.indices.size() > before_volume.mesh.indices.size(), "Bake did not consume real relief geometry");
    require(baked.editable == before_volume.editable, "Bake changed editable texture settings");
    const auto& baked_volume = first_volume(*plater);
    // A geometry-changing bake assigns a fresh volume ID so scene and slicing
    // caches invalidate; only untouched sibling identities must be preserved.
    require(baked_volume.get_object() == plater->model().objects.front() && baked_volume.id().valid() &&
            baked_volume.id() != before_volume.id,
            "Published volume parent or identity is invalid");
    TriangleSelector painted(baked_volume.mesh()); painted.deserialize(baked.mmu);
    size_t assigned = 0;
    std::vector<unsigned> used;
    for (size_t i = 0; i < bundle->mixed_filaments.total_filaments(4); ++i) {
        const auto faces = painted.get_facets_strict(EnforcerBlockerType(i + 1)).indices.size();
        if (faces) {
            require(i >= 4 && bundle->mixed_filaments.is_mixed(unsigned(i + 1), 4), "Solid fixture must consume real generated mixed IDs");
            used.push_back(unsigned(i + 1)); assigned += faces;
        }
    }
    require(assigned == baked.mesh.indices.size(), "Some baked faces have no valid mixed assignment");
    std::vector<uint64_t> stable_ids;
    for (const auto& row : bundle->mixed_filaments.mixed_filaments()) if (row.enabled && !row.deleted) stable_ids.push_back(row.stable_id);
    require(!stable_ids.empty() && std::all_of(stable_ids.begin(), stable_ids.end(), [](auto id) { return id != 0; }), "No persistent recipe identities");
    evidence["created_stable_ids"] = stable_ids; evidence["mixed_paint_ids"] = used;
    evidence["baked_triangles"] = baked.mesh.indices.size(); evidence["sidebar_after"] = after_palette.sidebar_ids;
    evidence["project_definitions"] = after_palette.project_definitions; evidence["print_definitions"] = after_palette.print_definitions;
    checkpoint("actual_plater_mixed_bake_verified");

    // Real import selects the newly added object. Undo removes it while the
    // old selection still refers to it, exercising scene/palette refresh order.
    const auto second_path = root / "mixed-history-second.stl";
    require(make_cube(8., 8., 3.).write_binary(second_path.u8string().c_str()), "Could not write second object");
    // The wxArrayString overload is the normal GUI import operation and owns
    // its TakeSnapshot. The lower-level LoadStrategy overload intentionally
    // leaves snapshotting to its caller (also used during project restoration).
    wxArrayString second_files;
    second_files.Add(wxString::FromUTF8(second_path.u8string()));
    require(plater->load_files(second_files), "Second import failed");
    pump_for(std::chrono::milliseconds(400));
    require(plater->model().objects.size() == 2 && plater->get_selection().get_object_idx() == 1, "Second object was not selected by real import");
    checkpoint("selected_second_object_imported");
    plater->undo(); pump_for(std::chrono::milliseconds(300));
    require(plater->model().objects.size() == 1, "Undo did not remove the selected second object");
    baked.check(first_volume(*plater)); check_palette(*plater, after_palette, "Undo selected object import");
    evidence["selected_object_removal_undo"] = true;
    rename_fixture(*plater, "Mix smoke after edit", "after-mix");
    const auto top_timestamp = plater->undo_redo_stack_main().active_snapshot_time();
    jump_to(*plater, before_timestamp);
    before_volume.check(first_volume(*plater)); check_palette(*plater, before_palette, "Direct Undo across Bake");
    require(plater->model().objects.front()->name == "before-mix", "Pre-Bake ordinary edit was lost");
    jump_to(*plater, top_timestamp);
    baked.check(first_volume(*plater)); check_palette(*plater, after_palette, "Direct Redo across Bake");
    require(plater->model().objects.front()->name == "after-mix", "Post-Bake ordinary edit was lost");
    jump_to(*plater, original_timestamp);
    before_volume.check(first_volume(*plater)); check_palette(*plater, before_palette, "Jump before ordinary edit and Bake");
    require(plater->model().objects.front()->name == original_name, "Earlier model state was not restored");
    jump_to(*plater, top_timestamp);
    baked.check(first_volume(*plater)); check_palette(*plater, after_palette, "Redo all mixed history");
    evidence["formal_multistep_history"] = true;
    checkpoint("formal_history_jumps_verified");
    // A physical-color edit is not a Model snapshot. Ordinary model Undo must
    // leave that user's new color intact if no recipe definitions changed.
    rename_fixture(*plater, "Mix smoke color preservation", "color-test-ordinary-edit");
    auto changed_colors = physical; changed_colors[0] = "#E02010";
    bundle->project_config.set_key_value("filament_colour", new ConfigOptionStrings(changed_colors));
    plater->on_config_change(bundle->full_config());
    wxGetApp().sidebar().update_mixed_filament_panel();
    wxGetApp().sidebar().update_dynamic_filament_list();
    plater->update_filament_colors_in_full_config();
    pump_for(std::chrono::milliseconds(200));
    const auto recolored_palette = palette_state(*plater);
    plater->undo(); pump_for(std::chrono::milliseconds(200));
    require(plater->model().objects.front()->name == "after-mix", "Physical color guard blocked unrelated model Undo");
    check_palette(*plater, recolored_palette, "Ordinary Undo after physical color edit");
    baked.check(first_volume(*plater));
    evidence["ordinary_undo_preserves_physical_color"] = true;
    bundle->project_config.set_key_value("filament_colour", new ConfigOptionStrings(physical));
    plater->on_config_change(bundle->full_config());
    wxGetApp().sidebar().update_mixed_filament_panel();
    wxGetApp().sidebar().update_dynamic_filament_list();
    plater->update_filament_colors_in_full_config();
    pump_for(std::chrono::milliseconds(200));
    check_palette(*plater, after_palette, "Fixture color restoration");
    checkpoint("ordinary_undo_color_isolation_verified");
    const auto project = root / "真实自动混色.3mf";
    require(plater->export_3mf_copy(boost::filesystem::path(project.u8string())) && std::filesystem::file_size(project) > 0,
            "Real mixed project export failed");
    evidence["saved_project"] = project.u8string();
    const auto discarded_redo_timestamp = plater->undo_redo_stack_main().snapshots().back().timestamp;
    jump_to(*plater, before_timestamp);
    rename_fixture(*plater, "Mix smoke branch", "branch-without-mixed-recipes");
    // has_redo_snapshot() includes the active topmost Action snapshot. Verify
    // actual branch removal and the public Redo operation instead.
    const auto& branched_stack = plater->undo_redo_stack_main();
    const auto branch_timestamp = branched_stack.active_snapshot_time();
    require(branch_timestamp == branched_stack.snapshots().back().timestamp &&
            std::none_of(branched_stack.snapshots().begin(), branched_stack.snapshots().end(),
                         [=](const auto& snapshot) { return snapshot.timestamp == discarded_redo_timestamp; }),
            "Ordinary edit did not discard the old mixed redo branch");
    before_volume.check(first_volume(*plater)); check_palette(*plater, before_palette, "Branch before Bake");
    plater->redo(); pump_for(std::chrono::milliseconds(200));
    require(branched_stack.active_snapshot_time() == branch_timestamp &&
            plater->model().objects.front()->name == "branch-without-mixed-recipes",
            "Redo followed the discarded mixed branch");
    before_volume.check(first_volume(*plater)); check_palette(*plater, before_palette, "Redo after branch replacement");
    evidence["branch_discards_mixed_redo"] = true;
    checkpoint("formal_history_branch_verified");

    plater->new_project(true, true, "Different plain project");
    require(!plater->load_files(std::vector<std::string>{cube_path.u8string()}, LoadStrategy::LoadModel).empty(), "Different project import failed");
    pump_for(std::chrono::milliseconds(500)); assert_fresh_history(*plater);
    const auto unrelated_palette = palette_state(*plater);
    const auto unrelated_name = plater->model().objects.front()->name;
    rename_fixture(*plater, "Unrelated ordinary edit", "unrelated-edited");
    plater->undo(); pump_for(std::chrono::milliseconds(200));
    require(plater->model().objects.front()->name == unrelated_name, "New project normal Undo failed");
    check_palette(*plater, unrelated_palette, "New project ordinary Undo");
    plater->redo(); pump_for(std::chrono::milliseconds(200));
    check_palette(*plater, unrelated_palette, "New project ordinary Redo");
    evidence["different_project_history_isolated"] = true;
    plater->new_project(true, true, "Mixed project reopen");
    require(!plater->load_files(std::vector<std::string>{project.u8string()},
            LoadStrategy::LoadModel | LoadStrategy::LoadConfig | LoadStrategy::Silence).empty(), "Real 3MF reopen failed");
    pump_for(std::chrono::milliseconds(800)); assert_fresh_history(*plater);
    evidence["archive_geometry"] = baked.archive_geometry_report(first_volume(*plater));
    evidence["archive_masks"] = baked.archive_mask_report(first_volume(*plater));
    checkpoint("mixed_project_archive_loaded");
    baked.check(first_volume(*plater), true);
    check_palette(*plater, after_palette, "3MF reopen");
    evidence["stable_ids_and_both_configs_roundtrip"] = true;
    checkpoint("mixed_project_reopened");
    auto* plate = plater->get_partplate_list().get_curr_plate();
    require(plate && plate->can_slice(), "Reopened mixed model cannot slice");
    plater->reslice();
    wait_until(*plater, [&] {
        require(!plate->is_apply_result_invalid(), "Real U1 presets/model rejected by Plater validation");
        return plate->is_slice_result_valid() && plate->fff_print()->is_step_done(psGCodeExport) &&
               !plater->is_background_process_slicing();
    }, "Real mixed G-code export timed out");
    std::set<unsigned> tools;
    size_t extrusions = 0;
    for (const auto& move : plate->get_slice_result()->moves) if (move.type == EMoveType::Extrude) {
        require(move.extruder_id < 4 && move.position.allFinite() && move.position.z() >= -.001f &&
                std::isfinite(move.delta_extruder) && move.delta_extruder >= 0.f, "Virtual tool or invalid extrusion/position reached G-code");
        tools.insert(move.extruder_id); ++extrusions;
    }
    require(tools.size() >= 2 && extrusions > 0, "Generated mixed paint did not resolve to multiple physical tools");
    size_t layer_count = 0;
    for (const auto* object : plate->fff_print()->objects()) for (const auto* layer : object->layers()) {
        bool walls = false;
        for (const auto* region : layer->regions()) walls |= !region->perimeters.empty();
        require(walls, "Reopened solid has a layer without perimeters"); ++layer_count;
    }
    require(layer_count > 0, "No actual sliced layers");
    const auto produced_gcode = std::filesystem::u8path(plate->get_gcode_filename());
    const auto gcode_copy = root / "mixed-reopened.gcode";
    require(std::filesystem::is_regular_file(produced_gcode) && std::filesystem::file_size(produced_gcode) > 0,
            "Actual G-code file is missing");
    std::filesystem::copy_file(produced_gcode, gcode_copy, std::filesystem::copy_options::overwrite_existing);
    evidence["gcode_file"] = gcode_copy.u8string();
    evidence["gcode_physical_tools"] = tools; evidence["gcode_extrusions"] = extrusions;
    evidence["reopened_gcode_moves"] = plate->get_slice_result()->moves.size();
    evidence["ok"] = true;
    checkpoint("texture_mix_workflow_complete");
    return evidence;
}
} // namespace Slic3r::GUI
