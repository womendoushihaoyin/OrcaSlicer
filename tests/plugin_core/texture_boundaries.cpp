#include "slic3r/GUI/TextureEditSession.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/Print.hpp"
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <stdexcept>

using namespace Slic3r;
using Slic3r::GUI::TextureEditSession;
namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
std::string slices(const Model& model, const DynamicPrintConfig& config)
{
    Print print;
    print.set_status_silent();
    print.set_cancel_callback([] {});
    print.apply(model, config);
    print.restart();
    require(print.objects().size() == 1, "Boundary fixture did not create a PrintObject");
    auto& object = *print.get_object(0);
    object.slice();
    require(!object.layers().empty(), "Boundary fixture produced no real sliced layers");
    std::ostringstream out;
    out.precision(17);
    for (const auto* layer : object.layers()) {
        out << layer->print_z << ':' << layer->slice_z << ';';
        for (const auto& island : layer->lslices) {
            for (const auto& point : island.contour.points) out << point.x() << ',' << point.y() << '/';
            for (const auto& hole : island.holes) {
                out << 'h';
                for (const auto& point : hole.points) out << point.x() << ',' << point.y() << '/';
            }
        }
    }
    return out.str();
}
struct ArchiveResources {
    PlateDataPtrs plates;
    std::vector<Preset*> presets;
    ~ArchiveResources() { release_PlateData_list(plates); for (auto* preset : presets) delete preset; }
};
}

void test_texture_boundaries(const std::filesystem::path& fixtures, const std::filesystem::path& output)
{
    std::filesystem::create_directories(output);
    const auto original_image = output / std::filesystem::u8path("原始纹理 & source.png");
    std::filesystem::copy_file(fixtures / "texture-white.png", original_image);
    std::ifstream input(original_image, std::ios::binary);
    auto image = std::make_shared<std::vector<unsigned char>>(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    require(!image->empty(), "Unicode image fixture is empty");
    Model model;
    model.set_backup_path((output / "source-backup").u8string());
    auto* object = model.add_object(); object->add_instance();
    auto* volume = object->add_volume(make_cube(8, 8, 1));
    object->name = "可编辑纹理工程";
    auto config = DynamicPrintConfig::full_print_config();
    config.set_key_value("filament_settings_id", new ConfigOptionStrings({"PLA"}));
    config.set_key_value("layer_height", new ConfigOptionFloat(.2));
    config.set_key_value("elefant_foot_compensation", new ConfigOptionFloat(0.));
    const auto baseline = slices(model, config);
    const auto source_mesh = volume->mesh_ptr();
    TextureEditSession edit(*volume);
    edit.add_layer("纹理 \"editable\" & <layer>", original_image.u8string(), image);
    edit.layer(0)->depth_mm = .6f;
    edit.options.pipeline_v2 = false;
    TriangleSelector painted(volume->mesh());
    for (size_t face = 0; face < volume->mesh().its.indices.size(); ++face)
        painted.set_facet(int(face), EnforcerBlockerType::ENFORCER);
    edit.masks[0] = painted.serialize();
    edit.add_layer("Missing embedded image", original_image.u8string(), image);
    edit.layer(1)->image_data.reset(); edit.masks[1] = edit.masks[0];
    edit.add_layer("Damaged embedded image", original_image.u8string(), image);
    edit.layer(2)->image_data = std::make_shared<std::vector<unsigned char>>(std::initializer_list<unsigned char>{1,2,3,4});
    edit.masks[2] = edit.masks[0];
    edit.apply(*volume);
    require(volume->mesh_ptr() == source_mesh && slices(model, config) == baseline,
            "Saving editable data changed the printed geometry without an explicit bake");

    const auto project = output / std::filesystem::u8path("原位置 纹理工程.3mf");
    const std::string project_path = project.u8string();
    StoreParams params;
    params.path = project_path.c_str(); params.model = &model; params.config = &config;
    params.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence | SaveStrategy::SplitModel;
    require(store_bbs_3mf(params), "Could not save the Unicode editable project");
    const auto relocated_dir = output / std::filesystem::u8path("已移动的工程目录");
    std::filesystem::create_directories(relocated_dir);
    const auto relocated = relocated_dir / std::filesystem::u8path("重命名后 & editable.3mf");
    std::filesystem::rename(project, relocated);
    std::filesystem::remove(original_image);
    Model restored;
    restored.set_backup_path((output / "restored-backup").u8string());
    ConfigSubstitutionContext substitutions{ForwardCompatibilitySubstitutionRule::Enable};
    ArchiveResources resources;
    bool is_bbs = false;
    Semver version;
    require(load_bbs_3mf(relocated.u8string().c_str(), &config, &substitutions, &restored, &resources.plates,
                        &resources.presets, &is_bbs, &version, nullptr, LoadStrategy::LoadModel | LoadStrategy::LoadConfig),
            "Moved Unicode texture project did not reopen");
    require(restored.objects.size() == 1 && restored.objects[0]->volumes.size() == 1, "Moved texture project lost its object");
    auto& reopened_volume = *restored.objects[0]->volumes[0];
    TextureEditSession reopened(reopened_volume);
    require(reopened.layers.size() == 3 && reopened.layer(0)->name == edit.layer(0)->name &&
            reopened.layer(0)->path == original_image.u8string() &&
            reopened.layer(0)->image_data && *reopened.layer(0)->image_data == *image &&
            reopened.layer(0)->image_data != image && reopened.masks == edit.masks,
            "Moving the project lost its editable image, name, source label or mask");
    require(reopened.layer(1)->empty() && decode_height_texture(*reopened.layer(2)).empty(),
            "Missing or damaged archive image was treated as a valid texture");
    require(slices(restored, config) == baseline, "Reopening editable data automatically baked it into slicing");
    std::filesystem::remove(relocated);
    require(!decode_height_texture(*reopened.layer(0)).empty(), "Loaded pixels depended on the archive remaining on disk");

    // Unknown enum values remain safely rejected by the editing boundary; a failed edit never
    // partially publishes to the model. Missing image data itself is repairable in the editor.
    auto unknown = reopened;
    unknown.layer(0)->blend_mode = TextureBlendMode(999);
    bool rejected = false;
    try { unknown.apply(reopened_volume); } catch (const std::exception&) { rejected = true; }
    require(rejected && slices(restored, config) == baseline, "An unknown blend mode reached or changed printed geometry");
    reopened.layer(1)->image_data = reopened.layer(0)->image_data;
    reopened.layer(2)->image_data = reopened.layer(0)->image_data;
    reopened.layer(1)->path_in_3mf.clear(); reopened.layer(2)->path_in_3mf.clear();
    reopened.layer(1)->depth_mm = .1f; reopened.layer(2)->depth_mm = .1f;
    reopened.apply(reopened_volume);
    TextureEditSession repaired(reopened_volume);
    require(!decode_height_texture(*repaired.layer(1)).empty() && !decode_height_texture(*repaired.layer(2)).empty(),
            "Replacing a missing or damaged image did not repair the reopened editable layer");
    auto baked = build_texture_displacement(repaired.source->its, repaired.layers, repaired.masks, repaired.options,
                                            {}, nullptr, repaired.volume_to_world, nullptr, nullptr,
                                            repaired.model_uv.empty() ? nullptr : &repaired.model_uv);
    repaired.apply(reopened_volume, std::move(baked));
    require(slices(restored, config) != baseline, "Explicit bake did not reach the ordinary PrintObject slicing path");
}
