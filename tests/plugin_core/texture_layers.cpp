#include "libslic3r/TextureDisplacementLayers.hpp"
#include "libslic3r/TextureDisplacementUV.hpp"
#include <nlohmann/json.hpp>
#include <stdexcept>

using namespace Slic3r;
namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
}
// Metadata and UV/paint math contracts; the production ModelVolume/3MF paths have separate tests.
void test_texture_layers()
{
    TextureDisplacementLayer layer;
    layer.slot = 3;
    layer.name = "Retained layer";
    layer.path_in_3mf = "Metadata/texture_displacement/volume-3.png";
    layer.image_data = std::make_shared<std::vector<unsigned char>>(std::initializer_list<unsigned char>{1, 2, 3});
    layer.depth_mm = .8f;
    layer.projection_method = TextureProjectionMethod::ModelUV;
    layer.blend_mode = TextureBlendMode::Override;
    layer.tile_method = TextureTileMethod::MirroredRepeat;
    layer.offset = Vec2f(.2f, .4f);
    layer.lscm_seam_edges = {{1, 4}, {4, 8}};
    layer.lscm_uv_overrides = {{-1, Vec2f(3.f, 5.f)}};
    layer.islands = {{Vec2f(1.f, 2.f), 45.f, 2.f}};
    layer.island_groups = {2, 2};
    layer.view_project_projective = true;
    layer.view_project_matrix = {1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 1.f};
    layer.preview_adjust.brightness = .2f;
    TextureDisplacementOptions options;
    options.active_slot = 3;
    options.panel_mode = 1;
    options.v2_refine_mm = .25f;
    const auto json = texture_displacement_layers_to_json({layer}, options);
    std::vector<TextureDisplacementLayer> restored;
    TextureDisplacementOptions restored_options;
    require(texture_displacement_layers_from_json(json, restored, restored_options), "layer JSON roundtrip failed");
    require(restored.size() == 1 && restored.front().projection_method == TextureProjectionMethod::ModelUV,
            "model UV projection was silently converted on save/load");
    require(texture_displacement_layers_to_json(restored, restored_options) == json, "layer parameters were lost in JSON roundtrip");
    require(!restored.front().image_data && restored.front().preview_adjust.identity(), "metadata JSON persisted image/live edit state");
    const auto unchanged = texture_displacement_layers_to_json(restored, restored_options);
    require(!texture_displacement_layers_from_json(R"({"layers":[{"slot":8}]})", restored, restored_options), "out-of-range layer slot accepted");
    require(!texture_displacement_layers_from_json(R"({"layers":[{"slot":1},{"slot":1}]})", restored, restored_options), "duplicate layer slots accepted");
    require(!texture_displacement_layers_from_json(R"({"layers":[{"slot":1,"offset":["bad",0]}]})", restored, restored_options), "bad UV numbers accepted");
    require(texture_displacement_layers_to_json(restored, restored_options) == unchanged, "failed layer load partially changed outputs");
    require(texture_displacement_layers_from_json(R"({"version":1,"future":true,"layers":[{"slot":0}]})", restored, restored_options),
            "older/minimal layer metadata did not keep defaults");
    require(restored.front().depth_mm == .4f && restored_options.displace_border, "missing fields lost upstream defaults");
    require(blend_displacement(2.f, .5f, TextureBlendMode::Add) == 2.5f &&
            blend_displacement(2.f, .5f, TextureBlendMode::Multiply) == 1.f &&
            blend_displacement(2.f, 0.f, TextureBlendMode::Divide) == 40.f &&
            blend_displacement(2.f, .5f, TextureBlendMode::Override) == .5f, "layer blend contract changed");

    indexed_triangle_set mesh;
    mesh.vertices = {Vec3f(0, 0, 0), Vec3f(1, 0, 0), Vec3f(0, 1, 0), Vec3f(1, 1, 0)};
    mesh.indices = {Vec3i32(0, 1, 2), Vec3i32(1, 3, 2)};
    const std::vector<Vec2f> uv{Vec2f(0, 0), Vec2f(1, 0), Vec2f(0, 1), Vec2f(2, 0), Vec2f(3, 1), Vec2f(2, 1)};
    const auto mapped = compute_model_corner_uvs(mesh, uv, mesh, {0, 1});
    require(mapped == uv, "per-corner UV seam was collapsed");
    indexed_triangle_set patch;
    patch.vertices = {Vec3f(.25f, .25f, 0), Vec3f(.5f, .25f, 0), Vec3f(.25f, .5f, 0)};
    patch.indices = {Vec3i32(0, 1, 2)};
    const auto interpolated = compute_model_corner_uvs(mesh, uv, patch, {0});
    require(interpolated.size() == 3 && (interpolated[0] - Vec2f(.25f, .25f)).norm() < 1e-6f,
            "brush-split triangle UV interpolation failed");
    require(transfer_uv_channel(mesh, uv, patch) == interpolated, "replacement topology lost interpolated UV coordinates");
    for (auto& point : patch.vertices) point.z() = 1.f;
    const auto remote = transfer_uv_channel(mesh, uv, patch, .1f);
    require(remote.size() == 3 && remote[0].isZero(), "UV transfer ignored distance limit");
    require(compute_model_corner_uvs(mesh, {}, mesh, {0, 1}).empty(), "mismatched UV channel accepted");
    TriangleMesh triangle_mesh(mesh);
    TriangleSelector selector(triangle_mesh);
    selector.set_facet(0, EnforcerBlockerType::ENFORCER);
    selector.set_facet(1, EnforcerBlockerType::BLOCKER);
    const auto paint = transfer_facet_paint(mesh, selector.serialize(), mesh);
    require(paint.size() == 2 && paint[0] == uint8_t(EnforcerBlockerType::ENFORCER) && paint[1] == uint8_t(EnforcerBlockerType::BLOCKER),
            "facet paint transfer lost distinct source states");
}
