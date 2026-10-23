#pragma once
#include "TextureEditSession.hpp"
#include <algorithm>
#include <limits>
#include <map>
#include <numeric>

namespace Slic3r::GUI {

inline int texture_island_group(const std::vector<int>& groups, int chart)
{
    return chart >= 0 && size_t(chart) < groups.size() && groups[size_t(chart)] >= 0 ? groups[size_t(chart)] : chart;
}

inline std::vector<int> texture_island_move_set(const std::vector<int>& groups, int chart_count,
                                              const std::vector<int>& selected, int primary)
{
    std::vector<int> result;
    const auto add = [&](int chart) {
        if (chart >= 0 && chart < chart_count && std::find(result.begin(), result.end(), chart) == result.end())
            result.push_back(chart);
    };
    auto seeds = selected;
    if (seeds.empty()) seeds.push_back(primary);
    for (const int seed : seeds) {
        if (seed < 0 || seed >= chart_count) continue;
        const int group = texture_island_group(groups, seed);
        for (int chart = 0; chart < chart_count; ++chart)
            if (texture_island_group(groups, chart) == group) add(chart);
    }
    add(primary);
    return result;
}

inline void normalize_texture_island_groups(std::vector<int>& groups, int chart_count)
{
    std::vector<int> normalized(size_t(std::max(0, chart_count)));
    std::map<int, int> representatives;
    for (int chart = 0; chart < chart_count; ++chart) {
        const auto entry = representatives.emplace(texture_island_group(groups, chart), chart);
        normalized[size_t(chart)] = entry.first->second;
    }
    groups = std::move(normalized);
}

inline void join_texture_island_groups(std::vector<int>& groups, int first, int second, int chart_count)
{
    if (first < 0 || second < 0 || first >= chart_count || second >= chart_count) return;
    normalize_texture_island_groups(groups, chart_count);
    const int a = groups[size_t(first)], b = groups[size_t(second)];
    for (int& group : groups)
        if (group == a || group == b) group = std::min(a, b);
}

inline void unjoin_texture_island_group(std::vector<int>& groups, int chart, int chart_count)
{
    if (chart < 0 || chart >= chart_count) return;
    normalize_texture_island_groups(groups, chart_count);
    const int old_group = groups[size_t(chart)];
    int replacement = -1;
    for (int other = 0; other < chart_count; ++other)
        if (other != chart && groups[size_t(other)] == old_group) { replacement = other; break; }
    // Detaching the representative must relabel the other members too. Merely
    // setting groups[chart] = chart leaves that representative joined forever.
    for (int other = 0; other < chart_count; ++other)
        if (other != chart && groups[size_t(other)] == old_group) groups[size_t(other)] = replacement;
    groups[size_t(chart)] = chart;
}

// Shift adds, Ctrl/Cmd toggles, and an unmodified click on an existing member
// preserves the selection for dragging. Return whether the clicked island is selected.
inline bool select_texture_island(std::vector<int>& selected, int& primary, int hit, bool add, bool toggle)
{
    const auto found = std::find(selected.begin(), selected.end(), hit);
    if (hit < 0) {
        if (!add && !toggle) { selected.clear(); primary = -1; }
        return false;
    }
    if (add) {
        if (found == selected.end()) selected.push_back(hit);
    } else if (toggle) {
        if (found != selected.end()) {
            selected.erase(found);
            primary = selected.empty() ? -1 : selected.back();
            return false;
        }
        selected.push_back(hit);
    } else if (found == selected.end()) selected.assign(1, hit);
    primary = hit;
    return true;
}

inline bool join_nearest_texture_island(const PatchUnwrap& layout, int child,
                                       std::vector<TextureIsland>& islands, std::vector<int>& groups)
{
    if (child < 0 || child >= layout.chart_count || size_t(child) >= layout.chart_centroid.size()) return false;
    const Vec2f child_center = apply_island_transform(layout.chart_centroid[size_t(child)], child, layout, islands);
    int best_parent = -1;
    float best_distance = std::numeric_limits<float>::infinity();
    TextureIsland best;
    for (int parent = 0; parent < layout.chart_count; ++parent) {
        if (parent == child || size_t(parent) >= layout.chart_centroid.size()) continue;
        TextureIsland candidate;
        if (!join_chart_placement(layout, islands, child, parent, candidate)) continue;
        const Vec2f parent_center = apply_island_transform(layout.chart_centroid[size_t(parent)], parent, layout, islands);
        const float distance = (parent_center - child_center).squaredNorm();
        if (distance < best_distance) { best_distance = distance; best_parent = parent; best = candidate; }
    }
    if (best_parent < 0) return false;
    islands.resize(size_t(layout.chart_count));
    islands[size_t(child)] = best;
    join_texture_island_groups(groups, child, best_parent, layout.chart_count);
    return true;
}
} // namespace Slic3r::GUI
