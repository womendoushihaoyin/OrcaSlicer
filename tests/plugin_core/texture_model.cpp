#include "libslic3r/Model.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/TextureDisplacementUV.hpp"
#include <cereal/archives/binary.hpp>
#include <cereal/types/vector.hpp>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <set>

using namespace Slic3r;
namespace {
void require(bool value, const std::string& message) { if (!value) throw std::runtime_error(message); }
std::vector<unsigned char> bytes(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    require(bool(input), "Missing texture model fixture");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
std::string metadata(const ModelVolume& volume)
{
    auto layers = volume.texture_displacement_layers;
    for (auto& layer : layers) layer.path_in_3mf.clear();
    return texture_displacement_layers_to_json(layers, volume.texture_displacement_options);
}
void configure(ModelVolume& volume, const std::filesystem::path& fixtures)
{
    TextureDisplacementLayer first;
    first.slot = 0;
    first.name = "纹理 \"first\" & <height>";
    first.path = (fixtures / "texture-white.png").u8string();
    first.image_data = std::make_shared<std::vector<unsigned char>>(bytes(fixtures / "texture-white.png"));
    first.projection_method = TextureProjectionMethod::ModelUV;
    first.rotation_deg = 23.f;
    first.offset = Vec2f(.25f, -.5f);
    TextureDisplacementLayer second = first;
    second.slot = 7;
    second.name = "Second layer";
    second.depth_mm = .75f;
    second.blend_mode = TextureBlendMode::Multiply;
    second.projection_method = TextureProjectionMethod::Triplanar;
    second.islands.push_back({Vec2f(.5f, 1.f), 31.f, 2.f});
    volume.texture_displacement_layers = {second, first}; // insertion order differs from slot/blend order
    volume.texture_displacement_options.active_slot = 7;
    volume.texture_displacement_options.panel_mode = 1;
    volume.texture_displacement_options.v2_refine_mm = 2.f;
    TriangleSelector selector(volume.mesh());
    selector.set_facet(0, EnforcerBlockerType::ENFORCER);
    volume.texture_displacement_facet(0).set(selector);
    selector.reset();
    selector.set_facet(1, EnforcerBlockerType::ENFORCER);
    volume.texture_displacement_facet(7).set(selector);
    volume.texture_uv.resize(volume.mesh().its.indices.size() * 3);
    for (size_t i = 0; i < volume.texture_uv.size(); ++i)
        volume.texture_uv[i] = Vec2f(float(i) * .125f, float(i % 3) * .5f);
}
void check_volume(const ModelVolume& before, const ModelVolume& after)
{
    require(metadata(before) == metadata(after), "3MF or model copy lost editable layer parameters");
    require(before.texture_uv == after.texture_uv && after.has_texture_uv(), "Per-corner model UV seam did not round trip");
    require(before.texture_displacement_layers.size() == after.texture_displacement_layers.size(), "Lost texture layer");
    for (size_t i = 0; i < before.texture_displacement_layers.size(); ++i) {
        const auto& a = before.texture_displacement_layers[i].image_data;
        const auto& b = after.texture_displacement_layers[i].image_data;
        require(bool(a) == bool(b) && (!a || *a == *b), "Embedded texture pixels did not round trip");
    }
    for (int slot = 0; slot < int(TEXTURE_DISPLACEMENT_MAX_LAYERS); ++slot)
        require(before.texture_displacement_facet(slot).equals(after.texture_displacement_facet(slot)), "Independent texture paint mask changed");
}
}

void test_texture_model(const std::string& scenario, const std::filesystem::path& fixtures, const std::filesystem::path& output)
{
    std::filesystem::create_directories(output);
    Model source;
    source.set_backup_path((output / "texture-source-backup").u8string());
    auto* object = source.add_object();
    auto* first = object->add_volume(make_cube(8., 8., 8.));
    first->name = "first";
    object->add_instance();
    configure(*first, fixtures);
    if (scenario == "texture_model") {
        Model copied(source);
        check_volume(*first, *copied.objects.front()->volumes.front());
        Model clone_model;
        auto* cloned = clone_model.add_object(*object);
        const auto* clone = cloned->volumes.front();
        check_volume(*first, *clone);
        std::set<size_t> ids;
        ids.insert(clone->id().id);
        for (int slot = 0; slot < int(TEXTURE_DISPLACEMENT_MAX_LAYERS); ++slot) {
            require(clone->texture_displacement_facet(slot).id() != first->texture_displacement_facet(slot).id(), "Clone reused texture paint identity");
            require(ids.insert(clone->texture_displacement_facet(slot).id().id).second, "Texture paint IDs overlap");
        }
        // The production ModelVolume Undo serializer stores these exact value types alongside its
        // eight independently timestamped paint objects. Exercise binary pixel ownership as well as JSON.
        std::stringstream stream;
        { cereal::BinaryOutputArchive archive(stream); archive(first->texture_displacement_layers, first->texture_displacement_options, first->texture_uv); }
        std::vector<TextureDisplacementLayer> restored;
        TextureDisplacementOptions options;
        std::vector<Vec2f> uv;
        { cereal::BinaryInputArchive archive(stream); archive(restored, options, uv); }
        require(texture_displacement_layers_to_json(restored, options) == metadata(*first) && uv == first->texture_uv,
                "Undo layer/UV values did not round trip");
        require(restored[0].image_data && *restored[0].image_data == *first->texture_displacement_layers[0].image_data &&
                restored[0].image_data != first->texture_displacement_layers[0].image_data, "Undo pixels alias live mutable image data");
        cloned->volumes.front()->reset_extra_facets();
        require(clone->texture_displacement_facets_all_empty() && clone->texture_uv.empty(), "Topology reset kept stale texture masks/UV");
        require(first->is_texture_displacement_painted() && first->has_texture_uv(), "Reset mutated original volume");
        return;
    }
    require(scenario == "texture_3mf", "Unknown texture model scenario");
    auto* second = object->add_volume_with_shared_mesh(*first);
    second->name = "second";
    configure(*second, fixtures);
    // Same geometry pointer, different texture paint: archive mesh dedup must preserve the distinction.
    TriangleSelector second_paint(second->mesh());
    second_paint.set_facet(2, EnforcerBlockerType::ENFORCER);
    second->texture_displacement_facet(0).set(second_paint);
    second->texture_displacement_layers[0].depth_mm = 1.25f;
    second->texture_uv[0] = Vec2f(9.f, 8.f);
    require(first->mesh_ptr() == second->mesh_ptr(), "Fixture does not share geometry");
    // A missing archive image must never be filled from the source path, even when that local file exists.
    TextureDisplacementLayer missing;
    missing.slot = 3;
    missing.path = (fixtures / "texture-white.png").u8string();
    second->texture_displacement_layers.push_back(missing);
    auto config = DynamicPrintConfig::full_print_config();
    config.set_key_value("filament_settings_id", new ConfigOptionStrings({"PLA"}));
    const auto archive_path = (output / "editable-texture.3mf").u8string();
    StoreParams params;
    params.path = archive_path.c_str(); params.model = &source; params.config = &config;
    params.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence | SaveStrategy::ShareMesh | SaveStrategy::SplitModel;
    require(store_bbs_3mf(params), "Editable texture 3MF save failed");
    require(first->texture_displacement_layers[0].path_in_3mf.empty(), "Export mutated source layer paths");
    Model loaded;
    loaded.set_backup_path((output / "texture-loaded-backup").u8string());
    ConfigSubstitutionContext substitutions{ForwardCompatibilitySubstitutionRule::Enable};
    struct Resources {
        PlateDataPtrs plates;
        std::vector<Preset*> presets;
        ~Resources() { release_PlateData_list(plates); for (auto* preset : presets) delete preset; }
    } resources;
    bool is_bbs = false;
    Semver version;
    require(load_bbs_3mf(archive_path.c_str(), &config, &substitutions, &loaded, &resources.plates,
                        &resources.presets, &is_bbs, &version, nullptr, LoadStrategy::LoadModel | LoadStrategy::LoadConfig),
            "Editable texture 3MF import failed");
    require(loaded.objects.size() == 1 && loaded.objects.front()->volumes.size() == 2, "Texture round trip lost model structure");
    for (size_t i = 0; i < 2; ++i) check_volume(*source.objects.front()->volumes[i], *loaded.objects.front()->volumes[i]);
    require(!loaded.objects.front()->volumes[1]->texture_displacement_layers.back().image_data, "3MF import read a source file outside the archive");
    // A loaded project's own pixels remain usable after the source archive is removed.
    std::filesystem::remove(archive_path);
    std::string error;
    require(!decode_texture_image(*loaded.objects.front()->volumes[0]->texture_displacement_layers[0].image_data, error).empty(),
            "Loaded texture depended on the source archive lifetime");
}
