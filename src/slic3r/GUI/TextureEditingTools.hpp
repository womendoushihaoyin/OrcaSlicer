#pragma once
#include "TextureEditSession.hpp"
#include <algorithm>
#include <functional>
#include <limits>
#include <queue>
#include <stdexcept>

namespace Slic3r::GUI {

inline bool toggle_texture_seam(TextureDisplacementLayer& layer, int a, int b)
{
    if (a < 0 || b < 0 || a == b) return false;
    const std::pair<int,int> edge{std::min(a,b),std::max(a,b)};
    auto& seams = layer.lscm_seam_edges;
    const auto found = std::find(seams.begin(),seams.end(),edge);
    if (found == seams.end()) seams.push_back(edge);
    else seams.erase(found);
    return true;
}

// Fixed-upstream seam paths follow the original mesh's edge graph, in mesh
// index space. Persisting a compacted patch's indices would corrupt the recipe.
inline std::vector<std::pair<int,int>> texture_seam_path(const indexed_triangle_set& mesh, int from, int to)
{
    const size_t count = mesh.vertices.size();
    if (from < 0 || to < 0 || from == to || size_t(from) >= count || size_t(to) >= count) return {};
    std::vector<std::vector<std::pair<int,float>>> neighbors(count);
    for (const auto& face : mesh.indices)
        for (int corner = 0; corner < 3; ++corner) {
            const int a = face[corner], b = face[(corner + 1) % 3];
            if (a < 0 || b < 0 || size_t(a) >= count || size_t(b) >= count) return {};
            const float length = (mesh.vertices[size_t(a)] - mesh.vertices[size_t(b)]).norm();
            neighbors[size_t(a)].emplace_back(b,length);
            neighbors[size_t(b)].emplace_back(a,length);
        }
    std::vector<float> distance(count,std::numeric_limits<float>::infinity());
    std::vector<int> previous(count,-1);
    using Node = std::pair<float,int>;
    std::priority_queue<Node,std::vector<Node>,std::greater<Node>> pending;
    distance[size_t(from)] = 0.f; pending.emplace(0.f,from);
    while (!pending.empty()) {
        const auto [length,vertex] = pending.top(); pending.pop();
        if (length > distance[size_t(vertex)]) continue;
        if (vertex == to) break;
        for (const auto& [other,edge_length] : neighbors[size_t(vertex)]) {
            const float next = length + edge_length;
            if (next < distance[size_t(other)]) {
                distance[size_t(other)] = next; previous[size_t(other)] = vertex; pending.emplace(next,other);
            }
        }
    }
    if (previous[size_t(to)] < 0) return {};
    std::vector<std::pair<int,int>> result;
    for (int vertex = to; vertex != from; vertex = previous[size_t(vertex)]) {
        const int other = previous[size_t(vertex)];
        result.emplace_back(std::min(vertex,other),std::max(vertex,other));
    }
    std::reverse(result.begin(),result.end());
    return result;
}

inline void set_texture_uv_override(TextureDisplacementLayer& layer, int unwrapped, const Vec2f& raw_uv)
{
    if (unwrapped < 0 || unwrapped == std::numeric_limits<int>::max() || !raw_uv.allFinite())
        throw std::invalid_argument("Invalid texture UV edit");
    const int key = lscm_uv_override_key(unwrapped);
    const auto found = std::find_if(layer.lscm_uv_overrides.begin(),layer.lscm_uv_overrides.end(),
        [key](const auto& item) { return item.first == key; });
    if (found == layer.lscm_uv_overrides.end()) layer.lscm_uv_overrides.emplace_back(key,raw_uv);
    else found->second = raw_uv;
}

// ModelUV is per corner. Move the copies that are still one UV vertex within
// this island together, while preserving copies across an imported seam.
inline std::vector<int> texture_uv_vertex_copies(const PatchUnwrap& layout, int selected)
{
    if (selected < 0 || size_t(selected) >= layout.uvs.size() || layout.source_vertex.size()!=layout.uvs.size() ||
        layout.vertex_chart.size()!=layout.uvs.size()) return {};
    std::vector<int> result;
    for (size_t i = 0; i < layout.uvs.size(); ++i)
        if (layout.vertex_chart[i] == layout.vertex_chart[size_t(selected)] &&
            layout.source_vertex[i] == layout.source_vertex[size_t(selected)] &&
            layout.uvs[i] == layout.uvs[size_t(selected)]) result.push_back(int(i));
    return result;
}

inline void move_texture_uv_vertices(PatchUnwrap& layout, const std::vector<TextureIsland>& islands,
                                    const std::vector<int>& selected, const Vec2f& placed_delta)
{
    if (layout.vertex_chart.size()!=layout.uvs.size() || !placed_delta.allFinite()) return;
    for (const int vertex : selected) {
        if (vertex < 0 || size_t(vertex) >= layout.uvs.size()) continue;
        const auto transform = island_transform_matrix(layout.vertex_chart[size_t(vertex)],layout,islands);
        const Eigen::Matrix2f linear = transform.block<2,2>(0,0);
        if (std::abs(linear.determinant()) < 1e-12f) continue;
        layout.uvs[size_t(vertex)] += linear.inverse() * placed_delta;
    }
}
} // namespace Slic3r::GUI
