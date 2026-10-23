#include "libslic3r/TextureDisplacement.hpp"
#include "libslic3r/TextureDisplacementPaint.hpp"
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>

using namespace Slic3r;
namespace {
using Edge = std::pair<int, int>;
void require(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}
indexed_triangle_set plane(int copies, int unused)
{
    indexed_triangle_set mesh;
    mesh.vertices.assign(size_t(unused), Vec3f(99, 99, 99));
    for (int copy = 0; copy < copies; ++copy) {
        const int begin = int(mesh.vertices.size());
        // Separate identities at exactly coincident coordinates: never join across these copies.
        mesh.vertices.insert(mesh.vertices.end(), {Vec3f(0, 0, 0), Vec3f(10, 0, 0), Vec3f(10, 10, 0), Vec3f(0, 10, 0)});
        mesh.indices.emplace_back(begin, begin + 1, begin + 2);
        mesh.indices.emplace_back(begin, begin + 2, begin + 3);
    }
    mesh.vertices.emplace_back(-99, -99, -99); // An unused suffix must not shift generated IDs either.
    return mesh;
}
std::set<Edge> edges(const indexed_triangle_set &mesh)
{
    std::set<Edge> result;
    for (const auto &face : mesh.indices)
        for (int corner = 0; corner < 3; ++corner)
            result.emplace(std::minmax(face[corner], face[(corner + 1) % 3]));
    return result;
}
void stroke(TriangleSelector &selector, int seed)
{
    auto cursor = std::make_unique<TriangleSelector::Sphere>(Vec3f(5, 5, 0), Vec3f(5, 5, 10), 3.f,
        Transform3d::Identity(), TriangleSelector::ClippingPlane{});
    selector.select_patch(seed, std::move(cursor), EnforcerBlockerType::ENFORCER, Transform3d::Identity(), true);
}
std::map<Edge, Edge> origin_map(const std::vector<TexturePatchEdgeOrigin> &origins,
                              const indexed_triangle_set &base, const indexed_triangle_set &patch)
{
    const auto original_edges = edges(base), actual_edges = edges(patch);
    std::map<Edge, Edge> result;
    Edge previous(-1, -1);
    for (const auto &origin : origins) {
        require(origin.patch_edge.first < origin.patch_edge.second && origin.original_edge.first < origin.original_edge.second,
                "Edge provenance endpoints must be canonical");
        require(previous < origin.patch_edge, "Edge provenance must be sorted and unique by patch edge");
        require(actual_edges.count(origin.patch_edge) && original_edges.count(origin.original_edge),
                "Provenance invented a patch edge or original edge");
        require(result.emplace(origin.patch_edge, origin.original_edge).second, "Ambiguous ancestry for one patch edge");
        previous = origin.patch_edge;
    }
    return result;
}
// Independent oracle based on strict face ancestry and edge incidence, not midpoint propagation.
// A strict edge shared by source faces 2*k and 2*k+1 is exactly that copy's original diagonal.
void check_face_incidence_oracle(const TriangleSelector &selector, const indexed_triangle_set &patch,
                                const std::map<Edge, Edge> &origins, int copies, int unused)
{
    std::vector<int> painted_sources, rest_sources;
    const auto painted = selector.get_facets_strict(EnforcerBlockerType::ENFORCER, &painted_sources);
    const auto rest = selector.get_facets_strict(EnforcerBlockerType::NONE, &rest_sources);
    std::map<Edge, std::vector<int>> incident;
    const auto append = [&](const indexed_triangle_set &mesh, const std::vector<int> &sources) {
        for (size_t face = 0; face < mesh.indices.size(); ++face)
            for (int corner = 0; corner < 3; ++corner)
                incident[std::minmax(mesh.indices[face][corner], mesh.indices[face][(corner + 1) % 3])].push_back(sources[face]);
    };
    append(painted, painted_sources); append(rest, rest_sources);
    require(painted.indices.size() == patch.indices.size(), "Strict extraction changed face order");
    std::map<int, int> to_patch;
    std::set<Edge> painted_edges;
    for (size_t face = 0; face < painted.indices.size(); ++face)
        for (int corner = 0; corner < 3; ++corner) {
            to_patch[painted.indices[face][corner]] = patch.indices[face][corner];
            painted_edges.emplace(std::minmax(painted.indices[face][corner], painted.indices[face][(corner + 1) % 3]));
        }
    size_t inherited = 0, internal = 0;
    for (const auto &edge : painted_edges) {
        const Edge mapped = std::minmax(to_patch.at(edge.first), to_patch.at(edge.second));
        const auto &faces = incident.at(edge);
        if (faces.size() == 2 && faces[0] == faces[1]) {
            require(origins.count(mapped) == 0, "Internal subdivision edge was assigned a fictitious original ancestor");
            ++internal;
        }
        for (int copy = 0; copy < copies; ++copy) {
            if (std::find(faces.begin(), faces.end(), 2 * copy) != faces.end() &&
                std::find(faces.begin(), faces.end(), 2 * copy + 1) != faces.end()) {
                const Edge original(unused + 4 * copy, unused + 4 * copy + 2);
                const auto actual = origins.find(mapped);
                require(actual != origins.end() && actual->second == original,
                        "Shared source-face boundary lost its exact original-edge ancestor");
                ++inherited;
            }
        }
    }
    require(inherited > size_t(copies) && internal > 0, "Brush fixture did not produce both inherited and internal edges");
    std::cout << "provenance inherited=" << inherited << " internal_without_original=" << internal << '\n';
}
} // namespace

void test_texture_edge_origins(const std::filesystem::path &)
{
    for (int unused : {0, 3}) {
        const auto mesh = plane(2, unused);
        const TriangleMesh triangles(mesh);
        TriangleSelector selector(triangles);
        require(texture_patch_edge_origins(mesh, selector.serialize()).empty(), "Empty paint produced edge provenance");
        for (size_t face = 0; face < mesh.indices.size(); ++face)
            selector.set_facet(int(face), EnforcerBlockerType::ENFORCER);
        const auto data = selector.serialize();
        const auto patch = extract_painted_patch(mesh, data);
        const auto origins = origin_map(texture_patch_edge_origins(mesh, data), mesh, patch);
        require(origins.size() == edges(mesh).size(), "Unsplit painted mesh did not retain all original edges");
        for (const auto &[edge, original] : origins)
            require(edge == original, "Unused vertices or coincident geometry changed original vertex IDs");
    }
    for (int variant = 0; variant < 4; ++variant) {
        const bool coincident = variant == 1, tjoint = variant == 2;
        const int copies = coincident ? 2 : 1, unused = variant == 3 ? 3 : 0;
        const auto mesh = plane(copies, unused);
        const TriangleMesh triangles(mesh);
        TriangleSelector selector(triangles);
        selector.set_edge_limit(.4f);
        stroke(selector, 0);
        if (coincident) stroke(selector, 2);
        if (tjoint) selector.set_facet(1, EnforcerBlockerType::ENFORCER);
        const auto data = selector.serialize();
        const auto patch = extract_painted_patch(mesh, data);
        const auto origins = origin_map(texture_patch_edge_origins(mesh, data), mesh, patch);
        check_face_incidence_oracle(selector, patch, origins, copies, unused);
        size_t skipped_by_original_index_only = 0;
        for (const auto &[edge, original] : origins) {
            if (edge.first >= int(mesh.vertices.size()) || edge.second >= int(mesh.vertices.size()))
                ++skipped_by_original_index_only;
            // Existing seam expansion must agree with the new provenance without persisting child IDs.
            auto expanded = std::vector<Edge>{original};
            extract_painted_patch(mesh, data, expanded);
            require(std::binary_search(expanded.begin(), expanded.end(), edge),
                    "New provenance disagrees with actual sampler seam expansion");
        }
        require(skipped_by_original_index_only > 1, "Brush fixture did not expose the original Cut index-only omission");
        const auto repeated = origin_map(texture_patch_edge_origins(mesh, data), mesh, patch);
        require(origins == repeated, "Provenance extraction is not deterministic");
        std::cout << "variant=" << variant << " old_index_only_omissions=" << skipped_by_original_index_only << '\n';
    }
}
