#include "slic3r/GUI/TextureColorBake.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>

using namespace Slic3r;
using namespace Slic3r::GUI;
namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class F> void rejects(F action, const char* message)
{
    bool rejected = false;
    try { action(); } catch (const std::exception&) { rejected = true; }
    require(rejected, message);
}
std::shared_ptr<std::vector<unsigned char>> image(const std::filesystem::path& fixtures)
{
    std::ifstream input(fixtures / "texture-rgb.png", std::ios::binary);
    require(bool(input), "Missing RGB editor fixture");
    return std::make_shared<std::vector<unsigned char>>(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}
indexed_triangle_set plane()
{
    indexed_triangle_set mesh;
    mesh.vertices = {Vec3f(0,0,5),Vec3f(8,0,5),Vec3f(8,8,5),Vec3f(0,8,5)};
    mesh.indices = {Vec3i32(0,1,2),Vec3i32(0,2,3)};
    return mesh;
}
TriangleSelector::TriangleSplittingData paint(const TriangleMesh& mesh, int face, int state = 1)
{
    TriangleSelector selector(mesh); selector.set_facet(face, EnforcerBlockerType(state)); return selector.serialize();
}
size_t colored(const ModelVolume& volume, int state)
{
    TriangleSelector selector(volume.mesh()); selector.deserialize(volume.mmu_segmentation_facets.get_data());
    return selector.get_facets_strict(EnforcerBlockerType(state)).indices.size();
}
}
void test_texture_color_editor(const std::string& scenario, const std::filesystem::path& fixtures)
{
    const TexturePalette palette{{42,Vec3f(0,0,1),false},{3,Vec3f(1,0,0),false},{9,Vec3f(0,1,0),false}};
    if (scenario == "texture_color_palette") {
        const auto quantize = make_texture_palette_quantizer(palette);
        require(quantize(Vec3f(1,0,0)) == 1 && quantize(Vec3f(0,1,0)) == 2 && quantize(Vec3f(0,0,1)) == 0,
                "Perceptual quantizer confused palette index and actual filament ID");
        const TexturePalette mixed{{1,Vec3f(0,0,0),false},{2,Vec3f(1,1,1),false},{17,Vec3f(.5f,.5f,.5f),true}};
        require(make_texture_palette_quantizer(mixed)(Vec3f(.5f,.5f,.5f)) == 2, "Existing mixed filament was never used");
        require(make_texture_palette_quantizer(mixed,true)(Vec3f(.5f,.5f,.5f)) != 2, "Pure palette matched a mixed filament");
        const TexturePalette close{{1,Vec3f(.51f,.51f,.51f),false},{17,Vec3f(.5f,.5f,.5f),true}};
        require(make_texture_palette_quantizer(close)(Vec3f(.5f,.5f,.5f)) == 0, "A negligible mix improvement overrode the pure preference");
        rejects([&] { make_texture_palette_quantizer({{1,Vec3f(1,0,0),false},{1,Vec3f(0,0,1),false}}); },
                "Duplicate filament identities were accepted");
        require(!make_texture_palette_quantizer(palette,false,[](int) { return false; }), "Palette cancellation returned a usable quantizer");
        return;
    }
    Model model; auto* object = model.add_object(); object->add_instance();
    auto* volume = object->add_volume(TriangleMesh(plane()));
    volume->texture_uv.assign(6,Vec2f::Zero()); // Both faces sample the red pixel; only the first is painted.
    TriangleSelector prior(volume->mesh()); prior.set_facet(1,EnforcerBlockerType(9)); volume->mmu_segmentation_facets.set(prior);
    const auto old_mmu = volume->mmu_segmentation_facets.get_data();
    TextureEditSession session(*volume);
    session.add_layer("RGB texture","plugin://rgb",image(fixtures));
    session.layer(0)->color_enabled = true;
    session.options.color_despeckle = 0;
    session.options.pipeline_v2 = false;
    session.layer(0)->depth_mm = 0.f;
    session.masks[0] = paint(volume->mesh(),0);
    if (scenario == "texture_color_v2") {
        session.options.pipeline_v2 = true;
        session.options.v2_refine_mm = 1.f;
        session.options.v2_max_triangles_k = 50;
        session.layer(0)->depth_mm = .4f;
    } else require(scenario == "texture_color_commit", "Unknown color editor scenario");
    auto result = bake_texture_session(session,palette,old_mmu);
    require(!result.mesh.indices.empty() && result.texture_filaments.size() == result.mesh.indices.size(),
            "RGB bake did not produce real per-output triangle colors");
    require(std::count(result.texture_filaments.begin(),result.texture_filaments.end(),uint8_t(3)) > 0,
            "Texture red did not map to actual existing filament 3");
    require(std::count(result.preview_filaments.begin(),result.preview_filaments.end(),uint8_t(9)) > 0,
            "Preview dropped prior MMU paint outside the texture mask");
    require(std::none_of(result.texture_filaments.begin(),result.texture_filaments.end(),[](uint8_t state) { return state != 0 && state != 3; }),
            "A palette index was written as an MMU filament ID");
    if (scenario == "texture_color_v2")
        require(result.mesh.indices.size() > 2, "Color v2 test did not exercise new topology");
    else
        require(result.mesh.indices == session.source->its.indices && result.mesh.vertices == session.source->its.vertices,
                "Zero-depth color bake unexpectedly moved geometry");
    const auto mesh_before = volume->mesh_ptr();
    const auto expected_red = size_t(std::count(result.preview_filaments.begin(),result.preview_filaments.end(),uint8_t(3)));
    const auto expected_green = size_t(std::count(result.preview_filaments.begin(),result.preview_filaments.end(),uint8_t(9)));
    session.apply(*volume,std::move(result.mesh),result.texture_filaments);
    require(colored(*volume,3) == expected_red && colored(*volume,9) == expected_green,
            "Color commit disagreed with the merged preview or replaced unaffected prior paint");
    require(volume->texture_displacement_layers.size() == 1 && volume->texture_displacement_layers[0].color_enabled &&
            volume->texture_displacement_facet(0).empty(), "Color bake lost editable parameters or retained consumed masks");
    if (scenario == "texture_color_commit") require(volume->mesh_ptr() == mesh_before, "Color-only commit replaced unchanged geometry");
    TextureEditSession reopened(*volume);
    const auto canceled = bake_texture_session(reopened,palette,volume->mmu_segmentation_facets.get_data(),[](int) { return false; });
    require(canceled.mesh.indices.empty() && canceled.texture_filaments.empty(), "Canceled color job exposed a commit result");
    auto wrong_colors = std::vector<uint8_t>(volume->mesh().its.indices.size()+1,3);
    rejects([&] { reopened.apply(*volume,volume->mesh().its,wrong_colors); }, "Wrong-length color result reached the model");
}
