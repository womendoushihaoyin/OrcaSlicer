#pragma once
#include "TextureEditingTools.hpp"

namespace Slic3r::GUI {

// Selection is by UV primitive, not by its flattened endpoint union. Removing
// one edge must retain a shared endpoint that belongs to another selected edge.
// Per-corner aliases within a chart are one vertex; copies across seams stay
// independent even when they occupy the same pixel.
class TextureUVSelection
{
public:
    void clear() { m_primitives.clear(); }
    const std::vector<std::pair<int,int>>& primitives() const { return m_primitives; }

    bool select(const PatchUnwrap& layout, int first, int second, bool add, bool toggle)
    {
        const auto hit = primitive(layout, first, second);
        if (hit.first < 0) {
            if (!add && !toggle) clear();
            return false;
        }
        auto found = std::find(m_primitives.begin(), m_primitives.end(), hit);
        if (add) {
            if (found == m_primitives.end()) m_primitives.push_back(hit);
        } else if (toggle) {
            if (found == m_primitives.end()) m_primitives.push_back(hit);
            else { m_primitives.erase(found); return false; }
        } else if (found == m_primitives.end()) {
            m_primitives.assign(1, hit);
        }
        return true;
    }

    bool contains(const PatchUnwrap& layout, int first, int second) const
    {
        const auto hit = primitive(layout, first, second);
        return hit.first >= 0 && std::find(m_primitives.begin(), m_primitives.end(), hit) != m_primitives.end();
    }

    std::vector<int> vertices(const PatchUnwrap& layout) const
    {
        std::vector<int> result;
        for (const auto& item : m_primitives)
            for (int endpoint : {item.first, item.second}) {
                const auto copies = texture_uv_vertex_copies(layout, endpoint);
                result.insert(result.end(), copies.begin(), copies.end());
            }
        std::sort(result.begin(), result.end());
        result.erase(std::unique(result.begin(), result.end()), result.end());
        return result;
    }

private:
    static std::pair<int,int> primitive(const PatchUnwrap& layout, int first, int second)
    {
        const auto a = texture_uv_vertex_copies(layout, first);
        if (a.empty()) return {-1,-1};
        if (second < 0) return {a.front(),a.front()};
        const auto b = texture_uv_vertex_copies(layout, second);
        if (b.empty()) return {-1,-1};
        return {std::min(a.front(),b.front()),std::max(a.front(),b.front())};
    }
    std::vector<std::pair<int,int>> m_primitives;
};
} // namespace Slic3r::GUI
