#pragma once
#include "libslic3r/TextureDisplacement.hpp"
#include <algorithm>

namespace Slic3r::GUI {

// Enabling applies the current unwrap immediately. Disabling keeps the user's
// placements, and an already-enabled refresh must not erase subsequent edits.
inline bool set_texture_islands_connected(const PatchUnwrap& layout, bool enabled, bool& auto_connect,
                                          std::vector<TextureIsland>& islands)
{
    if (auto_connect == enabled)
        return false;
    if (enabled && !layout.empty()) {
        const auto net = compute_connected_net(layout);
        if (net.size() == size_t(layout.chart_count)) {
            if (islands.size() < net.size())
                islands.resize(net.size());
            std::copy(net.begin(), net.end(), islands.begin());
        }
    }
    auto_connect = enabled;
    return true;
}

// The fixed upstream command averages every stored scale, preserving all
// offsets, rotations, group identities and per-vertex edits.
inline bool average_texture_island_scale(std::vector<TextureIsland>& islands)
{
    if (islands.empty() || std::all_of(islands.begin(), islands.end(), [&](const TextureIsland& island) {
            return island.scale == islands.front().scale;
        }))
        return false;
    average_island_scales(islands);
    return true;
}

} // namespace Slic3r::GUI
