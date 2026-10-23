#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/Format/3mf.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/Zipper.hpp"
#include "libslic3r/miniz_extension.hpp"
#include <miniz.h>
#include "slic3r/plugin/PluginManager.hpp"
#include "slic3r/plugin/PythonInterpreter.hpp"
#include <boost/log/core.hpp>
#include <boost/nowide/args.hpp>
#include <boost/nowide/filesystem.hpp>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string_view>
#include <pybind11/embed.h>

using namespace Slic3r;
namespace test_fs = std::filesystem;
namespace {
void require(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}
constexpr std::array<ModelVolumeType, 6> modes = {ModelVolumeType::PRECISE_SEAM_CENTER, ModelVolumeType::PRECISE_SEAM_LEFT,
    ModelVolumeType::PRECISE_SEAM_RIGHT, ModelVolumeType::PRECISE_SEAM_ENFORCED, ModelVolumeType::PRECISE_SEAM_BLOCKED,
    ModelVolumeType::PRECISE_SEAM_NEUTRAL};
const std::array<const char*, 6> python_modes = {"PreciseSeamCenter", "PreciseSeamLeft", "PreciseSeamRight", "PreciseSeamEnforced",
    "PreciseSeamBlocked", "PreciseSeamNeutral"};
const std::string notes = "quoted \"value\" & <tag>\nnext line &amp; path\\file";

void dynamic_config_conversion()
{
    FullPrintConfig source;
    source.gcode_flavor.value = gcfKlipper;
    source.z_hop_types.values = {zhtNormal, zhtSpiral};
    source.retract_lift_enforce.values.clear();
    const auto& base = static_cast<const PrintRegionConfig&>(source);
    DynamicPrintConfig converted(base);
    require(converted.keys() == base.keys_ref(), "Static conversion lost configuration keys");
    size_t enum_count = 0, enum_vector_count = 0;
    for (const auto& key : converted.keys()) {
        const auto* option = converted.option(key);
        const auto* original = source.option(key);
        // Every copied option must be serializable, not only the enum implicated
        // in the original 3MF crash. Preserve empty arrays and non-default values.
        const auto serialized = converted.opt_serialize(key);
        if (const auto* vector = dynamic_cast<const ConfigOptionVectorBase*>(option))
            (void)vector->vserialize();
        if (option->type() == coEnums) {
            ++enum_vector_count;
            const auto* values = dynamic_cast<const ConfigOptionEnumsGeneric*>(option);
            const auto* before = dynamic_cast<const ConfigOptionEnumsGeneric*>(original);
            require(values && before && values->keys_map && values->values == before->values,
                    "Static enum vector lost mapping or values: " + key);
            DynamicPrintConfig parsed;
            parsed.set_deserialize_strict(key, serialized);
            require(parsed.option<ConfigOptionEnumsGeneric>(key)->values == before->values,
                    "Serialized enum vector changed values: " + key);
        } else {
            require(serialized == original->serialize(), "Static option value changed: " + key);
            if (option->type() == coEnum) ++enum_count;
        }
    }
    require(enum_count > 0 && enum_vector_count > 0, "Enum regression did not cover both representations");
    require(converted.opt_serialize("gcode_flavor") == "klipper", "Non-default scalar enum changed");
    require(converted.opt_serialize("retract_lift_enforce").empty(), "Empty enum array was replaced by defaults");
    auto defaults = DynamicPrintConfig::full_print_config();
    for (const auto& key : defaults.keys()) (void)defaults.opt_serialize(key);
}

void python_types()
{
    Model model;
    auto* volume = model.add_object()->add_volume(make_cube(1, 1, 1));
    PythonGILState gil;
    auto host = pybind11::module_::import("orca").attr("host");
    auto object = pybind11::cast(volume, pybind11::return_value_policy::reference);
    for (size_t i = 0; i < modes.size(); ++i) {
        volume->set_type(modes[i]);
        require(host.attr("ModelVolumeType").attr(python_modes[i]).cast<ModelVolumeType>() == modes[i], "Python seam enum mismatch");
        require(object.attr("is_precise_seam")().cast<bool>(), "Python seam predicate false");
        require(object.attr("is_precise_seam_strong")().cast<bool>() == (i < 3), "Python strong group mismatch");
        require(object.attr("is_precise_seam_weak")().cast<bool>() == (i >= 3), "Python weak group mismatch");
        require(ModelVolume::type_from_string(ModelVolume::type_to_string(modes[i])) == modes[i], "Native type string mismatch");
        require(volume->get_extruders().empty(), "Seam helper contributes printable material");
    }
    volume->set_type(ModelVolumeType::MODEL_PART);
    require(!object.attr("is_precise_seam")().cast<bool>(), "Ordinary part classified as seam");
}

void make_unknown_mode_archive(const std::string& input, const std::string& output, bool bbs)
{
    struct Reader {
        mz_zip_archive zip{};
        ~Reader() { if (zip.m_pState) close_zip_reader(&zip); }
    } reader;
    require(open_zip_reader(&reader.zip, input), "Cannot read test 3MF");
    Zipper writer(output);
    size_t replacements = 0;
    const std::string metadata = bbs ? "Metadata/model_settings.config" : "Metadata/Slic3r_PE_model.config";
    for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&reader.zip); ++i) {
        mz_zip_archive_file_stat stat;
        require(mz_zip_reader_file_stat(&reader.zip, i, &stat), "Cannot read test archive entry");
        if (stat.m_is_directory) continue;
        std::string name(stat.m_filename);
        std::replace(name.begin(), name.end(), '\\', '/');
        std::string data(size_t(stat.m_uncomp_size), '\0');
        if (!data.empty()) require(mz_zip_reader_extract_to_mem(&reader.zip, i, data.data(), data.size(), 0), "Cannot extract test archive");
        if (name == metadata) {
            size_t position = 0;
            const std::string key = "key=\"precise_seam_type\"";
            while ((position = data.find(key, position)) != std::string::npos) {
                const auto value = data.find("value=\"", position);
                const auto close = data.find('"', value + 7);
                require(value != std::string::npos && close != std::string::npos, "Invalid test metadata");
                data.replace(value + 7, close - value - 7, "precise_seam_future");
                position = value + 7 + std::string("precise_seam_future").size();
                ++replacements;
            }
        }
        writer.add_entry(name, data.data(), data.size());
    }
    require(replacements == modes.size(), "Unknown-mode test did not replace every mode");
    writer.finalize();
}

void roundtrip(const test_fs::path& output, bool bbs)
{
    Model source;
    source.set_backup_path((output / "source-backup").u8string());
    auto config = DynamicPrintConfig::full_print_config();
    config.set_key_value("filament_settings_id", new ConfigOptionStrings({"A"}));
    config.set_key_value("filament_colour", new ConfigOptionStrings({"#FFFFFF"}));
    auto* object = source.add_object();
    object->add_volume(make_cube(20, 20, 2))->config.set_key_value("extruder", new ConfigOptionInt(2));
    object->add_instance();
    for (size_t i = 0; i < modes.size(); ++i) {
        auto* helper = object->add_volume(make_cube(2, 6, 4));
        helper->set_type(ModelVolumeType::PARAMETER_MODIFIER);
        helper->name = python_modes[i];
        helper->config.set_key_value("notes", new ConfigOptionString(notes));
        helper->config.set_key_value("extruder", new ConfigOptionInt(2));
        helper->set_type(modes[i]);
    }
    auto* ordinary_modifier = object->add_volume(make_cube(1, 1, 1));
    ordinary_modifier->set_type(ModelVolumeType::PARAMETER_MODIFIER);
    ordinary_modifier->config.set_key_value("extruder", new ConfigOptionInt(2));
    const auto path = (output / (bbs ? "bbs.3mf" : "prusa.3mf")).u8string();
    if (bbs) {
        StoreParams params;
        params.path = path.c_str(); params.model = &source; params.config = &config;
        params.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence;
        require(store_bbs_3mf(params), "BBS 3MF save failed");
    } else require(store_3mf(path.c_str(), &source, &config, false), "Prusa 3MF save failed");
    auto load = [&](Model& loaded, const std::string& source_path, const std::string& backup) {
        loaded.set_backup_path((output / backup).u8string());
        ConfigSubstitutionContext substitutions{ForwardCompatibilitySubstitutionRule::Enable};
        if (bbs) {
            struct Resources {
                PlateDataPtrs plates;
                std::vector<Preset*> presets;
                ~Resources() { release_PlateData_list(plates); for (auto* preset : presets) delete preset; }
            } resources;
            bool is_bbs = false;
            Semver version;
            require(load_bbs_3mf(source_path.c_str(), &config, &substitutions, &loaded, &resources.plates,
                                &resources.presets, &is_bbs, &version, nullptr,
                                LoadStrategy::LoadModel | LoadStrategy::LoadConfig), "BBS 3MF import failed");
        } else require(load_3mf(source_path.c_str(), config, substitutions, &loaded, false), "Prusa 3MF import failed");
    };
    Model loaded;
    load(loaded, path, "loaded-backup");
    require(loaded.objects.size() == 1 && loaded.objects.front()->volumes.size() == 8, "Round trip lost volumes");
    for (size_t i = 0; i < modes.size(); ++i) {
        auto* helper = loaded.objects.front()->volumes[i + 1];
        require(helper->type() == modes[i], std::string(bbs ? "BBS" : "Prusa") + " round trip lost subtype for " +
                helper->name + ": got " + ModelVolume::type_to_string(helper->type()) + ", expected " + ModelVolume::type_to_string(modes[i]));
        require(helper->config.get().opt_string("notes") == notes, "Dormant config/XML escaping did not round trip");
        require(helper->config.opt_int("extruder") == 2, "Dormant extruder did not round trip");
        helper->set_type(ModelVolumeType::PARAMETER_MODIFIER);
        require(helper->config.opt_int("extruder") == 2 && helper->config.get().opt_string("notes") == notes,
                "Returning helper to modifier lost dormant settings");
        helper->set_type(modes[i]);
    }
    if (bbs) {
        require(loaded.objects.front()->volumes.front()->config.opt_int("extruder") == 0,
                "Ordinary part bypassed invalid filament normalization");
        require(loaded.objects.front()->volumes.back()->config.opt_int("extruder") == 0,
                "Ordinary modifier bypassed invalid filament normalization");
    }
    const auto unknown_path = (output / (bbs ? "bbs-unknown.3mf" : "prusa-unknown.3mf")).u8string();
    make_unknown_mode_archive(path, unknown_path, bbs);
    Model unknown;
    load(unknown, unknown_path, "unknown-backup");
    require(unknown.objects.size() == 1 && unknown.objects.front()->volumes.size() == 8, "Unknown mode lost geometry");
    for (size_t i = 0; i < modes.size(); ++i) {
        const auto* helper = unknown.objects.front()->volumes[i + 1];
        require(helper->is_modifier(), "Unknown seam mode did not retain safe modifier fallback");
        require(!helper->config.has("notes"), "Unknown mode activated dormant settings");
    }
    if (bbs) {
        // Cover the separate single-volume normalization branch as well. The
        // helper's stored material remains dormant even without a printable part.
        Model single;
        single.set_backup_path((output / "single-source-backup").u8string());
        auto* single_object = single.add_object();
        single_object->add_instance();
        auto* helper = single_object->add_volume(make_cube(2, 2, 2));
        helper->set_type(ModelVolumeType::PRECISE_SEAM_CENTER);
        helper->config.set_key_value("extruder", new ConfigOptionInt(2));
        const auto single_path = (output / "bbs-single-helper.3mf").u8string();
        StoreParams params;
        params.path = single_path.c_str(); params.model = &single; params.config = &config;
        params.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence;
        require(store_bbs_3mf(params), "Single-helper BBS save failed");
        Model reloaded;
        load(reloaded, single_path, "single-loaded-backup");
        require(reloaded.objects.size() == 1 && reloaded.objects.front()->volumes.size() == 1,
                "Single-helper round trip lost geometry");
        require(reloaded.objects.front()->volumes.front()->is_precise_seam() &&
                reloaded.objects.front()->volumes.front()->config.opt_int("extruder") == 2,
                "Single-helper normalization erased dormant extruder");
        helper->set_type(ModelVolumeType::MODEL_PART);
        const auto part_path = (output / "bbs-single-part.3mf").u8string();
        params.path = part_path.c_str();
        require(store_bbs_3mf(params), "Single-part BBS save failed");
        Model part;
        load(part, part_path, "single-part-loaded-backup");
        require(part.objects.size() == 1 && part.objects.front()->volumes.size() == 1 &&
                !part.objects.front()->volumes.front()->config.has("extruder"),
                "Ordinary single-part extruder normalization changed");
    }
}

struct OuterLoop { Vec2d seam; BoundingBoxf bounds; };
std::vector<OuterLoop> outer_loops(const std::string& gcode)
{
    std::vector<OuterLoop> loops;
    bool outer = false, in_loop = false;
    GCodeReader parser;
    parser.parse_buffer(gcode, [&](GCodeReader& self, const GCodeReader::GCodeLine& line) {
        const std::string_view comment = line.comment();
        // Snapmaker's exporter uses FEATURE; upstream Orca uses TYPE. Read the
        // actual native tag while retaining the same path/count assertions.
        if (comment.find("TYPE:") != std::string_view::npos || comment.find("FEATURE:") != std::string_view::npos) {
            outer = comment.find("Outer wall") != std::string_view::npos;
            in_loop = false;
        }
        if (!outer || !line.extruding(self) || line.dist_XY(self) == 0.f) return;
        if (!in_loop) {
            loops.push_back({Vec2d(self.x(), self.y()), {}});
            loops.back().bounds.merge(loops.back().seam);
            in_loop = true;
        }
        loops.back().bounds.merge(Vec2d(line.new_X(self), line.new_Y(self)));
    });
    for (auto& loop : loops) loop.seam -= loop.bounds.center();
    return loops;
}
struct Fixture {
    Model model;
    DynamicPrintConfig config;
    Print print;
    Vec3d cube_min;
    explicit Fixture(const std::string& seam, int raft = 0)
    {
        auto* object = model.add_object();
        object->name = "precise-seam-contract-cube";
        auto* cube = object->add_volume(make_cube(20, 20, 2));
        object->add_instance(); object->ensure_on_bed();
        cube_min = cube->mesh().transformed_bounding_box(cube->get_matrix()).min;
        const auto& defaults = FullPrintConfig::defaults();
        config.apply(static_cast<const PrintConfig&>(defaults), true);
        config.apply(static_cast<const PrintObjectConfig&>(defaults), true);
        config.apply(static_cast<const PrintRegionConfig&>(defaults), true);
        config.set_deserialize_strict("seam_position", seam);
        config.set_deserialize_strict("seam_slope_type", "none");
        config.set_deserialize_strict("layer_height", "0.2");
        config.set_deserialize_strict("initial_layer_print_height", "0.2");
        config.set_deserialize_strict("outer_wall_line_width", "0.4");
        config.set_deserialize_strict("initial_layer_line_width", "0.4");
        config.set_deserialize_strict("raft_layers", std::to_string(raft));
        print.set_status_silent(); print.set_cancel_callback([] {});
    }
    ModelVolume* add(ModelVolumeType type, const Vec3d& size, const Vec3d& minimum)
    {
        auto* helper = model.objects.front()->add_volume(make_cube(size.x(), size.y(), size.z()));
        helper->set_type(type); helper->name = "helper";
        const Vec3d old = helper->mesh().transformed_bounding_box(helper->get_matrix()).min;
        helper->set_offset(helper->get_offset() + cube_min + minimum - old);
        return helper;
    }
    std::vector<OuterLoop> export_loops(const test_fs::path& path)
    {
        print.apply(model, config); print.restart(); print.process();
        GCodeProcessorResult processor;
        print.export_gcode(path.u8string(), &processor, nullptr);
        std::ifstream input(path);
        require(bool(input), "No exported G-code");
        std::string data(std::istreambuf_iterator<char>(input), {});
        return outer_loops(data);
    }
};
void exports(const test_fs::path& output)
{
    int count = 0;
    for (size_t i = 0; i < 3; ++i) for (const auto* seam : {"back", "aligned", "nearest"}) for (int raft : {0, 2}) {
        Fixture fixture(seam, raft);
        auto* helper = fixture.add(modes[i], {2, 6, 4}, {4, -3, -1});
        const auto loops = fixture.export_loops(output / ("strong-" + std::to_string(count++) + ".gcode"));
        require(loops.size() == 10, "Strong fixture did not produce ten outer walls");
        const double x = i == 0 ? 5. : i == 1 ? 4. : 6.;
        for (const auto& loop : loops) {
            require(std::abs(loop.seam.x() - (x - 10.)) < .015, "Strong mode did not set exported seam X");
            require(std::abs(loop.seam.y() - (loop.bounds.min.y() - loop.bounds.center().y())) < .015,
                    "Strong mode did not place seam on crossed front edge");
        }
        helper->set_offset(helper->get_offset() + Vec3d(1, 0, 0));
        fixture.print.apply(fixture.model, fixture.config);
        require(fixture.print.get_object(0)->is_step_done(posSlice) && !fixture.print.is_step_done(psGCodeExport),
                "Moving a seam helper did not preserve slices and invalidate G-code");
    }
    for (const auto type : {ModelVolumeType::INVALID, ModelVolumeType::PRECISE_SEAM_ENFORCED,
                           ModelVolumeType::PRECISE_SEAM_BLOCKED, ModelVolumeType::PRECISE_SEAM_NEUTRAL}) {
        Fixture fixture("back");
        if (type == ModelVolumeType::PRECISE_SEAM_BLOCKED) fixture.add(type, {30, 6, 4}, {-5, 17, -1});
        else if (type != ModelVolumeType::INVALID) fixture.add(type, {2, 6, 4}, {4, -3, -1});
        const auto loops = fixture.export_loops(output / ("weak-" + std::to_string(count++) + ".gcode"));
        require(loops.size() == 10, "Weak fixture did not produce ten outer walls");
        for (const auto& loop : loops) {
            if (type == ModelVolumeType::PRECISE_SEAM_ENFORCED)
                require(loop.seam.x() >= -6.02 && loop.seam.x() <= -3.98 && loop.seam.y() < -7., "Enforced zone ignored");
            else if (type == ModelVolumeType::PRECISE_SEAM_BLOCKED) require(loop.seam.y() < 7., "Blocked zone ignored");
            else require(loop.seam.y() > 7., "Neutral or no-helper changed back seam");
        }
    }
}
void priorities(const test_fs::path& output)
{
    for (bool strong : {false, true}) {
        Fixture fixture("back");
        if (strong) {
            fixture.add(ModelVolumeType::PRECISE_SEAM_CENTER, {2, 6, 4}, {4, -3, -1});
            fixture.add(ModelVolumeType::PRECISE_SEAM_CENTER, {2, 6, 4}, {12, -3, -1});
        } else {
            fixture.add(ModelVolumeType::PRECISE_SEAM_NEUTRAL, {2, 6, 4}, {4, -3, -1});
            fixture.add(ModelVolumeType::PRECISE_SEAM_ENFORCED, {2, 6, 4}, {4, -3, -1});
        }
        for (bool reordered : {false, true}) {
            if (reordered) std::swap(fixture.model.objects.front()->volumes[1], fixture.model.objects.front()->volumes[2]);
            fixture.model.objects.front()->sort_volumes(true);
            const auto loops = fixture.export_loops(output / (std::string("priority-") + (strong ? "strong-" : "weak-") +
                                                              (reordered ? "reordered.gcode" : "original.gcode")));
            require(loops.size() == 10, "Priority fixture did not produce ten outer walls");
            for (const auto& loop : loops) {
                if (strong) require(std::abs(loop.seam.x() - (reordered ? 3. : -5.)) < .015, "Strong model order ignored");
                else if (reordered) require(loop.seam.y() < -7., "Weak reordered Enforced priority ignored");
                else require(loop.seam.y() > 7., "Weak higher Neutral priority ignored");
            }
        }
    }
}
void apply_transitions(const test_fs::path& output)
{
    Fixture fixture("back");
    auto* helper = fixture.add(ModelVolumeType::PRECISE_SEAM_CENTER, {2, 6, 4}, {4, -3, -1});
    // Keep a second auxiliary volume so delete_volume() does not collapse the final
    // part transform into the instance and legitimately force ordinary reslicing.
    fixture.add(ModelVolumeType::PRECISE_SEAM_NEUTRAL, {2, 2, 4}, {40, 40, -1});
    auto check = [&](const std::string& name, double expected_x, bool front) {
        const auto loops = fixture.export_loops(output / ("transition-" + name + ".gcode"));
        require(loops.size() == 10, "Transition " + name + " did not produce ten outer walls");
        for (const auto& loop : loops) {
            if (front) require(std::abs(loop.seam.x() - expected_x) < .015 && loop.seam.y() < -7.,
                               "Transition " + name + " did not reach exported seam");
            else require(loop.seam.y() > 7., "Transition " + name + " did not restore back seam");
        }
    };
    auto invalidate = [&](const std::string& name) {
        fixture.print.apply(fixture.model, fixture.config);
        require(fixture.print.get_object(0)->is_step_done(posSlice) && !fixture.print.is_step_done(psGCodeExport),
                "Transition " + name + " did not preserve slices and invalidate G-code");
    };
    check("initial", -5., true);
    helper->set_offset(helper->get_offset() + Vec3d(1, 0, 0));
    invalidate("move"); check("move", -4., true);
    helper->set_type(ModelVolumeType::PRECISE_SEAM_LEFT);
    invalidate("subtype"); check("subtype", -5., true);
    helper->set_type(ModelVolumeType::SUPPORT_BLOCKER);
    invalidate("support"); check("support", 0., false);
    helper->set_type(ModelVolumeType::PRECISE_SEAM_CENTER);
    invalidate("support-to-seam"); check("support-to-seam", -4., true);
    fixture.model.objects.front()->delete_volume(1);
    invalidate("delete"); check("delete", 0., false);
    fixture.add(ModelVolumeType::PRECISE_SEAM_CENTER, {2, 6, 4}, {12, -3, -1});
    invalidate("add"); check("add", 3., true);
}

void structured_slices()
{
    Fixture fixture("back");
    fixture.print.apply(fixture.model, fixture.config); fixture.print.restart(); fixture.print.process();
    TriangleMesh shell = make_cube(10, 10, 10);
    TriangleMesh cavity = make_cube(6, 6, 12);
    cavity.translate(2., 2., -1.); cavity.flip_triangles(); shell.merge(cavity);
    TriangleMesh island = make_cube(2, 2, 10);
    island.translate(30., 0., 0.); shell.merge(island);
    Model helpers;
    auto* helper = helpers.add_object()->add_volume(shell);
    helper->set_type(ModelVolumeType::PRECISE_SEAM_CENTER);
    for (bool mirrored : {false, true}) {
        helper->set_mirror(Vec3d(mirrored ? -1. : 1., 1., 1.));
        const auto layers = fixture.print.get_object(0)->slice_single_volume_regions(helper);
        require(layers.size() == 10, "Auxiliary slices do not use object layer count");
        for (const auto& regions : layers) {
            require(regions.size() == 2, "Auxiliary slice lost its separate island");
            size_t holes = 0;
            double area = 0.;
            for (const auto& region : regions) {
                require(region.contour.is_counter_clockwise(), "Auxiliary contour winding wrong");
                holes += region.holes.size(); area += region.area();
                for (const auto& hole : region.holes) require(hole.is_clockwise(), "Auxiliary hole winding wrong");
            }
            require(holes == 1 && std::abs(area / double(scale_(1.)) / double(scale_(1.)) - 68.) < 1e-4,
                    "Auxiliary slice lost hole topology or mirrored area");
        }
    }
}

} // namespace
int main(int argc, char** argv)
{
    boost::nowide::args utf8_args(argc, argv);
    boost::nowide::nowide_filesystem();
    if (argc != 2) return 2;
    const auto output = test_fs::u8path(argv[1]);
    if (test_fs::exists(output)) return 3;
    test_fs::create_directories(output);
    set_data_dir((output / "profile").u8string());
    test_fs::create_directories(output / "profile");
    boost::log::core::get()->set_logging_enabled(false);
    auto& manager = PluginManager::instance();
    int status = 0;
    try {
        require(manager.initialize(), "Cannot initialize production bindings");
        dynamic_config_conversion();
        std::cout << "PASS static-to-dynamic config enum mappings, values and serialization" << std::endl;
        python_types();
        std::cout << "PASS precise seam Python binding contract" << std::endl;
        roundtrip(output, false);
        std::cout << "PASS precise seam Prusa 3MF and unknown-mode fallback" << std::endl;
        roundtrip(output, true);
        std::cout << "PASS precise seam BBS/Prusa 3MF and unknown-mode fallback" << std::endl;
        exports(output); priorities(output); apply_transitions(output);
        std::cout << "PASS precise seam exported positions, priorities and update transitions" << std::endl;
        structured_slices();
        std::cout << "PASS precise seam real Python bindings, BBS/Prusa 3MF roundtrip, dormant config, 18 strong G-code cases, weak zones, neutral/no-helper, model priority, unknown-mode fallback, mirrored holes, exported apply transitions\n";
    } catch (const std::exception& error) { std::cerr << "FAIL precise seam: " << error.what() << '\n'; status = 1; }
    manager.shutdown(); PythonInterpreter::instance().shutdown();
    return status;
}
