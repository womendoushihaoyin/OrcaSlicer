#include "libslic3r/TextureDisplacement.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>

using namespace Slic3r;
namespace {
void require(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}
TriangleSelector::TriangleSplittingData paint(const indexed_triangle_set& mesh, bool brush = false)
{
    const TriangleMesh triangles(mesh);
    TriangleSelector selector(triangles);
    if (brush) {
        selector.set_edge_limit(.3f);
        auto cursor = std::make_unique<TriangleSelector::Sphere>(Vec3f(6, 3.5f, 0), Vec3f(6, 3.5f, 10),
            2.f, Transform3d::Identity(), TriangleSelector::ClippingPlane{});
        selector.select_patch(0, std::move(cursor), EnforcerBlockerType::ENFORCER, Transform3d::Identity(), true);
    } else {
        for (size_t i = 0; i < mesh.indices.size(); ++i)
            selector.set_facet(int(i), EnforcerBlockerType::ENFORCER);
    }
    return selector.serialize();
}
float mesh_difference(const indexed_triangle_set& a, const indexed_triangle_set& b)
{
    if (a.vertices.size() != b.vertices.size() || a.indices != b.indices)
        return 1e9f;
    float error = 0.f;
    for (size_t i = 0; i < a.vertices.size(); ++i)
        error = std::max(error, (a.vertices[i] - b.vertices[i]).norm());
    return error;
}
indexed_triangle_set plane()
{
    indexed_triangle_set mesh;
    mesh.vertices = {Vec3f(0, 0, 0), Vec3f(10, 0, 0), Vec3f(10, 10, 0), Vec3f(0, 10, 0)};
    mesh.indices = {Vec3i32(0, 1, 2), Vec3i32(0, 2, 3)};
    return mesh;
}
indexed_triangle_set with_unused_vertices(const indexed_triangle_set& mesh, const std::array<int, 4>& original_ids)
{
    indexed_triangle_set result = mesh;
    // Duplicate positions deliberately: recovering indices by matching coordinates would be ambiguous.
    result.vertices.assign(size_t(original_ids.back() + 2), Vec3f::Zero());
    for (size_t i = 0; i < original_ids.size(); ++i)
        result.vertices[size_t(original_ids[i])] = mesh.vertices[i];
    for (auto& face : result.indices)
        for (int corner = 0; corner < 3; ++corner)
            face[corner] = original_ids[size_t(face[corner])];
    return result;
}
TextureDisplacementLayer layer(const std::filesystem::path& fixtures)
{
    std::ifstream input(fixtures / "texture-gray.png", std::ios::binary);
    require(bool(input), "Missing texture vertex-index fixture");
    TextureDisplacementLayer result;
    result.image_data = std::make_shared<std::vector<unsigned char>>(
        std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    result.slot = 0;
    result.depth_mm = .4f;
    result.tiling_scale = 1.f;
    result.auto_connect_islands = false;
    result.projection_method = TextureProjectionMethod::LSCM;
    result.lscm_seam_angle_deg = 180.f;
    result.island_padding_mm = 0.f;
    result.lscm_seam_edges = {{0, 2}};
    result.lscm_uv_overrides = {{0, Vec2f(.5f, .5f)}, {1, Vec2f(.5f, .5f)},
                               {2, Vec2f(.5f, .5f)}, {3, Vec2f(.5f, .5f)}};
    // Negative keys refer to one unwrapped copy and must not be treated as original vertex indices.
    result.lscm_uv_overrides.emplace_back(lscm_uv_override_key(0), Vec2f(.25f, .5f));
    return result;
}
void compare_samplers(const indexed_triangle_set& original, const indexed_triangle_set& compact,
                      const TextureDisplacementLayer& original_layer, const TextureDisplacementLayer& compact_layer,
                      const TextureDisplacementFacetsData& masks, const TextureDisplacementFacetsData& compact_masks)
{
    const auto sampler = make_combined_displacement_sampler(original, {original_layer}, masks);
    const auto reference = make_combined_displacement_sampler(compact, {compact_layer}, compact_masks);
    require(bool(sampler) && bool(reference), "Texture vertex-index sampler missing");
    for (const Vec3f& point : std::array<Vec3f, 4>{Vec3f(1, 2, 0), Vec3f(2, 1, 0), Vec3f(5, 8, 0), Vec3f(8, 5, 0)})
        require(std::abs(sampler(point, Vec3f::UnitZ()) - reference(point, Vec3f::UnitZ())) < 1e-5f,
                "Sampler changed manual seam or UV edits when unused vertices were present");
}
} // namespace

void test_texture_vertex_indices(const std::filesystem::path& fixtures)
{
    const auto compact = plane();
    const auto compact_layer = layer(fixtures);
    TextureDisplacementFacetsData compact_masks;
    compact_masks[0] = paint(compact);

    // Prefix, internal and trailing holes, including a mesh with only a trailing unused vertex.
    for (const auto& ids : std::array<std::array<int, 4>, 4>{{{1, 2, 3, 4}, {0, 2, 4, 6}, {2, 4, 5, 8}, {0, 1, 2, 3}}}) {
        const auto original = with_unused_vertices(compact, ids);
        auto original_layer = compact_layer;
        original_layer.lscm_seam_edges = {{ids[0], ids[2]}};
        for (auto& entry : original_layer.lscm_uv_overrides)
            if (entry.first >= 0)
                entry.first = ids[size_t(entry.first)];
        TextureDisplacementFacetsData masks;
        masks[0] = paint(original);
        const auto source_layers = std::vector<TextureDisplacementLayer>{original_layer};
        const auto source_mesh = original;
        const auto patch = extract_painted_patch(original, masks[0]);
        const auto direct = compute_patch_unwrap(original, 180.f, 0.f, original_layer.lscm_seam_edges);
        const auto extracted = compute_patch_unwrap(patch, 180.f, 0.f, original_layer.lscm_seam_edges);
        require(direct.chart_count == 2 && extracted.chart_count == 2,
                "Manual seam changed islands during painted patch extraction");
        for (int id : ids)
            require(size_t(id) < patch.vertices.size() && patch.vertices[size_t(id)] == original.vertices[size_t(id)],
                    "Extracted patch lost original vertex identity");
        require(compute_lscm_corner_uvs(original, original_layer) == compute_lscm_corner_uvs(patch, original_layer),
                "Extracted patch changed per-corner UV edits");
        compare_samplers(original, compact, original_layer, compact_layer, masks, compact_masks);
        for (bool v2 : {false, true}) {
            TextureDisplacementOptions options;
            options.pipeline_v2 = v2;
            options.v2_refine_mm = 3.f;
            options.v2_max_triangles_k = 0;
            options.v2_flip_edges = false;
            const auto actual = build_texture_displacement(original, source_layers, masks, options);
            const auto expected = build_texture_displacement(compact, {compact_layer}, compact_masks, options);
            require(!actual.indices.empty() && mesh_difference(actual, expected) < 1e-5f,
                    "Bake compacted the mesh without preserving manual seam or UV keys");
        }
        require(original.vertices == source_mesh.vertices && original.indices == source_mesh.indices &&
                source_layers[0].lscm_seam_edges == original_layer.lscm_seam_edges &&
                source_layers[0].lscm_uv_overrides == original_layer.lscm_uv_overrides,
                "Texture bake changed the editable source mesh or layer keys");
    }

    // A real subdividing brush appends vertices after the original mesh prefix. Restoring unused
    // originals must move those generated indices too, without changing any sub-triangle geometry.
    const std::array<int, 4> ids{1, 3, 5, 7};
    const auto original = with_unused_vertices(compact, ids);
    const auto original_brush = extract_painted_patch(original, paint(original, true));
    const auto compact_brush = extract_painted_patch(compact, paint(compact, true));
    require(original_brush.indices.size() > 2 && original_brush.indices.size() == compact_brush.indices.size(),
            "Brush subdivision did not produce corresponding painted pieces");
    bool generated_vertex = false;
    for (size_t face = 0; face < original_brush.indices.size(); ++face)
        for (int corner = 0; corner < 3; ++corner) {
            const int a = original_brush.indices[face][corner], b = compact_brush.indices[face][corner];
            require(a >= 0 && size_t(a) < original_brush.vertices.size(), "Restored brush vertex is out of range");
            generated_vertex |= size_t(a) >= original.vertices.size();
            require((original_brush.vertices[size_t(a)] - compact_brush.vertices[size_t(b)]).norm() < 1e-6f,
                    "Restoring original vertex indices changed a brush sub-triangle");
        }
    require(generated_vertex, "Brush case did not exercise generated vertex remapping");
}
