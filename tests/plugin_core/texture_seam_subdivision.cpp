#include "libslic3r/TextureDisplacement.hpp"
#include "libslic3r/TextureDisplacementPaint.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>

using namespace Slic3r;
namespace {
using Edge = std::pair<int, int>;
void require(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}
TextureDisplacementLayer layer(const std::filesystem::path& fixtures)
{
    TextureDisplacementLayer result;
    result.slot = 0;
    result.projection_method = TextureProjectionMethod::LSCM;
    result.auto_connect_islands = false;
    result.lscm_seam_angle_deg = 180.f;
    result.tiling_scale = 7.f;
    std::ifstream input(fixtures / "texture-gray.png", std::ios::binary);
    require(bool(input), "Missing seam subdivision texture fixture");
    result.image_data = std::make_shared<std::vector<unsigned char>>(
        std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    return result;
}
indexed_triangle_set plane(int copies, int prefix)
{
    indexed_triangle_set mesh;
    mesh.vertices.assign(size_t(prefix), Vec3f::Zero());
    for (int i = 0; i < copies; ++i) {
        const int base = int(mesh.vertices.size());
        // Identical positions but entirely separate vertex/edge identities for the second surface.
        mesh.vertices.insert(mesh.vertices.end(), {Vec3f(0, 0, 0), Vec3f(10, 0, 0), Vec3f(10, 10, 0), Vec3f(0, 10, 0)});
        mesh.indices.emplace_back(base, base + 1, base + 2);
        mesh.indices.emplace_back(base, base + 2, base + 3);
    }
    return mesh;
}
void stroke(TriangleSelector& selector, int seed)
{
    auto cursor = std::make_unique<TriangleSelector::Sphere>(Vec3f(5, 5, 0), Vec3f(5, 5, 10), 3.f,
        Transform3d::Identity(), TriangleSelector::ClippingPlane{});
    selector.select_patch(seed, std::move(cursor), EnforcerBlockerType::ENFORCER, Transform3d::Identity(), true);
}

// Independent oracle: strict face provenance identifies the shared boundary of original faces 0/1.
// It does not inspect midpoint ancestry or use distances. Include unpainted strict faces so a seam
// segment with paint on just one side is still recognized as part of that original shared edge.
std::vector<Edge> reference_seams(const TriangleSelector& selector, const indexed_triangle_set& extracted)
{
    std::vector<int> painted_source, rest_source;
    const auto painted = selector.get_facets_strict(EnforcerBlockerType::ENFORCER, &painted_source);
    const auto rest = selector.get_facets_strict(EnforcerBlockerType::NONE, &rest_source);
    require(painted.indices.size() == extracted.indices.size(), "Painted patch face provenance changed");
    std::map<Edge, std::set<int>> edge_faces;
    std::set<Edge> painted_edges;
    std::map<int, int> to_extracted;
    const auto append = [&](const indexed_triangle_set& part, const std::vector<int>& source) {
        for (size_t face = 0; face < part.indices.size(); ++face)
            for (int corner = 0; corner < 3; ++corner)
                edge_faces[std::minmax(part.indices[face][corner], part.indices[face][(corner + 1) % 3])].insert(source[face]);
    };
    append(painted, painted_source);
    append(rest, rest_source);
    for (size_t face = 0; face < painted.indices.size(); ++face)
        for (int corner = 0; corner < 3; ++corner) {
            const int a = painted.indices[face][corner], b = painted.indices[face][(corner + 1) % 3];
            painted_edges.insert(std::minmax(a, b));
            const int mapped = extracted.indices[face][corner];
            require((painted.vertices[size_t(a)] - extracted.vertices[size_t(mapped)]).norm() < 1e-6f,
                    "Extracted brush geometry changed");
            to_extracted[a] = mapped;
        }
    std::vector<Edge> result;
    for (const auto& [edge, faces] : edge_faces)
        if (faces.count(0) && faces.count(1) && painted_edges.count(edge))
            result.emplace_back(std::minmax(to_extracted.at(edge.first), to_extracted.at(edge.second)));
    std::sort(result.begin(), result.end());
    return result;
}
TextureDisplacementFacetsData paint_all(const indexed_triangle_set& mesh)
{
    const TriangleMesh triangles(mesh);
    TriangleSelector selector(triangles);
    for (size_t face = 0; face < mesh.indices.size(); ++face)
        selector.set_facet(int(face), EnforcerBlockerType::ENFORCER);
    TextureDisplacementFacetsData data;
    data[0] = selector.serialize();
    return data;
}
float geometry_change(const indexed_triangle_set& a, const indexed_triangle_set& b)
{
    if (a.vertices.size() != b.vertices.size() || a.indices != b.indices)
        return 1.f;
    float result = 0.f;
    for (size_t i = 0; i < a.vertices.size(); ++i)
        result = std::max(result, (a.vertices[i] - b.vertices[i]).norm());
    return result;
}
} // namespace

void test_texture_seam_subdivision(const std::filesystem::path& fixtures)
{
    for (int variant = 0; variant < 4; ++variant) {
        const bool coincident = variant == 1;
        const bool tjoint = variant == 2;
        const int prefix = variant == 3 ? 2 : 0;
        const auto mesh = plane(coincident ? 2 : 1, prefix);
        const TriangleMesh triangles(mesh);
        TriangleSelector selector(triangles);
        selector.set_edge_limit(.4f);
        stroke(selector, 0);
        if (coincident)
            stroke(selector, 2);
        if (tjoint)
            selector.set_facet(1, EnforcerBlockerType::ENFORCER); // Only the neighbour is split: strict export resolves its T-joints.
        const std::vector<Edge> original_seams{{prefix, prefix + 2}};
        auto expanded = original_seams;
        const auto patch = extract_painted_patch(mesh, selector.serialize(), expanded);
        const auto expected_edges = reference_seams(selector, patch);
        require(expected_edges.size() > 1 && expanded == expected_edges,
                "Original seam did not resolve to exactly its actual painted sub-edges");
        const auto actual = compute_patch_unwrap(patch, 180.f, 0.f, expanded);
        const auto expected = compute_patch_unwrap(patch, 180.f, 0.f, expected_edges);
        require(actual.chart_count == (coincident ? 3 : 2) && actual.chart_count == expected.chart_count,
                "Split seam cut the wrong charts or crossed disconnected coincident topology");

        auto original_layer = layer(fixtures);
        original_layer.lscm_seam_edges = original_seams;
        // A separate island can retain its own user placement. With this explicit placement the
        // classic path's original corner vertices also distinguish the split from an uncut chart.
        original_layer.islands.resize(2);
        original_layer.islands[1].offset = Vec2f(1.7f, .9f);
        TextureDisplacementFacetsData masks;
        masks[0] = selector.serialize();
        if (!coincident) {
            const auto sampler = make_combined_displacement_sampler(mesh, {original_layer}, masks);
            auto reference_layer = original_layer;
            reference_layer.lscm_seam_edges = expected_edges;
            const auto reference = make_combined_displacement_sampler(patch, {reference_layer}, paint_all(patch));
            require(bool(sampler) && bool(reference), "Split seam sampler is missing");
            for (const auto& face : patch.indices) {
                const Vec3f point = (patch.vertices[face[0]] + patch.vertices[face[1]] + patch.vertices[face[2]]) / 3.f;
                require(std::abs(sampler(point, Vec3f::UnitZ()) - reference(point, Vec3f::UnitZ())) < 1e-5f,
                        "Prepared sampler ignored a seam along brush-generated sub-edges");
            }
        }
        if (variant == 0 || tjoint) {
            TextureDisplacementOptions options;
            options.pipeline_v2 = !tjoint;
            options.v2_refine_mm = 1.5f;
            options.v2_max_triangles_k = 0;
            options.v2_flip_edges = false;
            const auto cut = build_texture_displacement(mesh, {original_layer}, masks, options);
            auto uncut_layer = original_layer;
            uncut_layer.lscm_seam_edges.clear();
            const auto uncut = build_texture_displacement(mesh, {uncut_layer}, masks, options);
            std::cout << "variant=" << variant << " v2=" << options.pipeline_v2 << " triangles=" << cut.indices.size() << " vertices=" << cut.vertices.size() << " change=" << geometry_change(cut, uncut) << std::endl;
            require(!cut.indices.empty() && !uncut.indices.empty() && geometry_change(cut, uncut) > 1e-4f,
                    "Real bake ignored the manual seam after brush subdivision");
        }
        require(original_layer.lscm_seam_edges == original_seams, "Sampling or baking overwrote persistent seam endpoints");
    }
}
