#pragma once
#include "TextureIslandGroups.hpp"
#include "libslic3r/TextureDisplacementPaint.hpp"
#include <array>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>

namespace Slic3r::GUI {

struct TextureIslandCutResult
{
    PatchUnwrap layout;
    std::vector<Vec2f> base_uvs;
    std::vector<TextureIsland> islands;
    std::vector<int> groups;
    std::vector<std::pair<int,int>> seams;
};

// A new seam renumbers charts and their duplicated vertices. Transfer placed
// coordinates by actual patch face/corner identity, never by spatial proximity,
// then store the new chart copies as overrides. Recenter each new island's
// offset so later rotations still turn about its visible center.
inline TextureIslandCutResult transfer_cut_texture_layout(
    PatchUnwrap next, const PatchUnwrap& previous, const std::vector<TextureIsland>& islands,
    const std::vector<int>& groups, int cut_chart, std::vector<std::pair<int,int>> seams)
{
    if (next.source_face.size() != next.indices.size() || previous.source_face.size() != previous.indices.size() ||
        next.indices.size() != previous.indices.size())
        throw std::runtime_error("The cut changed the painted face mapping; no UV edits were applied");
    using Face = std::array<Vec2f,3>;
    std::map<int,Face> old_uvs;
    std::map<int,int> old_charts;
    for (size_t face = 0; face < previous.indices.size(); ++face) {
        Face uv;
        for (int corner = 0; corner < 3; ++corner) {
            const int vertex = previous.indices[face][corner];
            uv[size_t(corner)] = apply_island_transform(previous.uvs[size_t(vertex)],previous.vertex_chart[size_t(vertex)],previous,islands);
        }
        old_uvs.emplace(previous.source_face[face],uv);
        old_charts.emplace(previous.source_face[face],previous.vertex_chart[size_t(previous.indices[face][0])]);
    }
    TextureIslandCutResult result;
    result.base_uvs = next.uvs;
    result.seams = std::move(seams);
    result.islands.resize(size_t(next.chart_count));
    result.groups.resize(size_t(next.chart_count));
    std::vector<bool> assigned(next.uvs.size(),false);
    std::map<std::pair<int,int>,int> representatives;
    for (size_t face = 0; face < next.indices.size(); ++face) {
        const int source = next.source_face[face];
        const auto found = old_uvs.find(source);
        if (found == old_uvs.end()) throw std::runtime_error("The cut changed the painted face mapping; no UV edits were applied");
        const int chart = next.vertex_chart[size_t(next.indices[face][0])];
        const int old_chart = old_charts.at(source);
        // Newly cut pieces move independently. Groups belonging to the other
        // charts keep their relationship even when all chart numbers change.
        const std::pair<int,int> key = old_chart == cut_chart ? std::make_pair(1,chart) :
            std::make_pair(0,texture_island_group(groups,old_chart));
        result.groups[size_t(chart)] = representatives.emplace(key,chart).first->second;
        for (int corner = 0; corner < 3; ++corner) {
            const size_t vertex = size_t(next.indices[face][corner]);
            const Vec2f& uv = found->second[size_t(corner)];
            if (assigned[vertex] && (next.uvs[vertex] - uv).norm() > 1e-4f)
                throw std::runtime_error("The cut would merge distinct UV edits; no changes were applied");
            next.uvs[vertex] = uv;
            assigned[vertex] = true;
        }
    }
    std::vector<Vec2f> centers(size_t(next.chart_count),Vec2f::Zero());
    std::vector<size_t> counts(size_t(next.chart_count),0);
    for (size_t vertex = 0; vertex < next.uvs.size(); ++vertex) {
        const size_t chart = size_t(next.vertex_chart[vertex]);
        centers[chart] += next.uvs[vertex]; ++counts[chart];
    }
    for (size_t chart = 0; chart < centers.size(); ++chart)
        if (counts[chart] > 0) result.islands[chart].offset = centers[chart] / float(counts[chart]) - next.chart_centroid[chart];
    for (size_t vertex = 0; vertex < next.uvs.size(); ++vertex)
        next.uvs[vertex] -= result.islands[size_t(next.vertex_chart[vertex])].offset;
    result.layout = std::move(next);
    return result;
}

inline TextureIslandCutResult cut_texture_island(
    const indexed_triangle_set& mesh, const TriangleSelector::TriangleSplittingData& mask,
    const Transform3d& volume_to_world, const TextureDisplacementLayer& layer,
    const PatchUnwrap& layout, const std::vector<TextureIsland>& islands,
    const std::vector<int>& groups, int chart)
{
    if (chart < 0 || chart >= layout.chart_count)
        throw std::runtime_error("Select an LSCM island first");
    auto patch = extract_painted_patch(mesh,mask);
    if (patch.indices.empty()) throw std::runtime_error("The painted surface is empty");
    std::map<std::pair<int,int>,size_t> chart_edges;
    Vec3f lo = Vec3f::Constant(std::numeric_limits<float>::max());
    Vec3f hi = Vec3f::Constant(std::numeric_limits<float>::lowest());
    for (const auto& face : layout.indices) {
        if (layout.vertex_chart[size_t(face[0])] != chart) continue;
        for (int corner = 0; corner < 3; ++corner) {
            const int a = layout.source_vertex[size_t(face[corner])];
            const int b = layout.source_vertex[size_t(face[(corner + 1) % 3])];
            if (a < 0 || b < 0 || size_t(a) >= patch.vertices.size() || size_t(b) >= patch.vertices.size())
                throw std::runtime_error("The painted surface changed; reopen the UV editor before cutting");
            lo = lo.cwiseMin(patch.vertices[size_t(a)]); hi = hi.cwiseMax(patch.vertices[size_t(a)]);
            ++chart_edges[{std::min(a,b),std::max(a,b)}];
        }
    }
    if (chart_edges.empty()) throw std::runtime_error("The selected island has no mesh edges");
    const Vec3f extent = hi - lo;
    const int axis = extent.x() >= extent.y() && extent.x() >= extent.z() ? 0 : (extent.y() >= extent.z() ? 1 : 2);
    const float middle = .5f * (lo[axis] + hi[axis]);
    std::map<std::pair<int,int>,std::pair<int,int>> origins;
    for (const auto& origin : texture_patch_edge_origins(mesh,mask))
        origins.emplace(origin.patch_edge,origin.original_edge);
    std::set<std::pair<int,int>> seams(layer.lscm_seam_edges.begin(),layer.lscm_seam_edges.end());
    // Explicit seams replace automatic angle cuts in compute_patch_unwrap().
    // Freeze the current chart boundaries before adding the first manual cut,
    // otherwise a local split can merge all other automatically created islands.
    // Use topological edge provenance so partial-paint child vertices are never
    // persisted as original mesh endpoints. Open patch borders need no seam.
    std::map<std::pair<int,int>,std::set<int>> edge_charts;
    for (const auto& face : layout.indices) {
        const int face_chart = layout.vertex_chart[size_t(face[0])];
        for (int corner = 0; corner < 3; ++corner) {
            const int a = layout.source_vertex[size_t(face[corner])];
            const int b = layout.source_vertex[size_t(face[(corner + 1) % 3])];
            edge_charts[{std::min(a,b),std::max(a,b)}].insert(face_chart);
        }
    }
    for (const auto& [edge,charts] : edge_charts) {
        if (charts.size() < 2) continue;
        const auto origin = origins.find(edge);
        if (origin == origins.end())
            throw std::runtime_error("An existing island border crosses brush-created interior edges that have no original mesh edge. No seams were changed");
        seams.insert(origin->second);
    }
    bool changed = false;
    for (const auto& [edge,face_count] : chart_edges) {
        if ((patch.vertices[size_t(edge.first)][axis] < middle) == (patch.vertices[size_t(edge.second)][axis] < middle)) continue;
        const auto origin = origins.find(edge);
        if (origin == origins.end()) {
            // A painted-patch boundary is already an island border, so it needs
            // no additional seam. Only an interior child edge blocks the cut.
            if (face_count == 1) continue;
            throw std::runtime_error("This cut crosses brush-created interior edges that have no original mesh edge. No seams were changed; use the original-edge seam tools instead");
        }
        changed |= seams.insert(origin->second).second;
    }
    if (!changed) throw std::runtime_error("No new original mesh edges cross the island midpoint");
    std::vector<std::pair<int,int>> stored(seams.begin(),seams.end()), expanded = stored;
    patch = extract_painted_patch(mesh,mask,expanded);
    const Transform3d frame = texture_displacement_bake_frame(volume_to_world);
    for (auto& vertex : patch.vertices) vertex = (frame * vertex.cast<double>()).cast<float>();
    auto next = compute_patch_unwrap(patch,layer.lscm_seam_angle_deg,0.f,expanded);
    if (next.empty()) throw std::runtime_error("The cut surface could not be unwrapped; no changes were applied");
    return transfer_cut_texture_layout(std::move(next),layout,islands,groups,chart,std::move(stored));
}

} // namespace Slic3r::GUI
