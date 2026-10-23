#pragma once

namespace Slic3r {
// Background lifecycle observers cannot synchronously wait for the UI: the UI
// may already be joining this slicing/export worker. Nesting preserves the guard.
class PluginBackgroundScope {
public:
    explicit PluginBackgroundScope(bool enabled) : m_enabled(enabled) { if (m_enabled) ++depth(); }
    ~PluginBackgroundScope() { if (m_enabled) --depth(); }
    PluginBackgroundScope(const PluginBackgroundScope&) = delete;
    PluginBackgroundScope& operator=(const PluginBackgroundScope&) = delete;
    static bool active() { return depth() != 0; }
private:
    static unsigned& depth() { static thread_local unsigned value = 0; return value; }
    bool m_enabled;
};
} // namespace Slic3r
