#pragma once
#include "TextureEditSession.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r::GUI {

// Private modal history. Mesh and encoded images stay shared and immutable;
// masks, UVs and layer parameters are value snapshots. No live Model is touched.
class TextureEditHistory
{
public:
    explicit TextureEditHistory(const TextureEditSession& initial, size_t limit = 64)
        : m_limit(std::max(size_t(1), limit))
    { m_entries.push_back({initial, {}}); }

    bool can_undo() const { return m_cursor > 0; }
    bool can_redo() const { return m_cursor + 1 < m_entries.size(); }
    const std::string& undo_label() const { return m_entries[m_cursor].label; }
    const std::string& redo_label() const { return m_entries[m_cursor + 1].label; }
    void end_group() { m_group = 0; }

    bool record(const TextureEditSession& state, std::string label, uintptr_t group = 0)
    {
        if (equal(m_entries[m_cursor].state, state)) return false;
        if (group != 0 && group == m_group && !can_redo() && can_undo()) {
            m_entries[m_cursor] = {state, std::move(label)};
        } else {
            m_entries.erase(m_entries.begin() + ptrdiff_t(m_cursor + 1), m_entries.end());
            m_entries.push_back({state, std::move(label)});
            ++m_cursor;
            if (m_entries.size() > m_limit + 1) {
                m_entries.erase(m_entries.begin());
                --m_cursor;
            }
        }
        m_group = group;
        return true;
    }
    bool undo(TextureEditSession& state)
    {
        end_group();
        if (!can_undo()) return false;
        state = m_entries[--m_cursor].state;
        return true;
    }
    bool redo(TextureEditSession& state)
    {
        end_group();
        if (!can_redo()) return false;
        state = m_entries[++m_cursor].state;
        return true;
    }

private:
    static bool equal(const TextureEditSession& a, const TextureEditSession& b)
    {
        if (a.masks != b.masks || a.model_uv != b.model_uv || a.layers.size() != b.layers.size()) return false;
        for (size_t i = 0; i < a.layers.size(); ++i)
            if (a.layers[i].image_data != b.layers[i].image_data) return false;
        return texture_displacement_layers_to_json(a.layers, a.options) ==
               texture_displacement_layers_to_json(b.layers, b.options);
    }
    struct Entry { TextureEditSession state; std::string label; };
    std::vector<Entry> m_entries;
    size_t m_cursor = 0, m_limit;
    uintptr_t m_group = 0;
};
} // namespace Slic3r::GUI
