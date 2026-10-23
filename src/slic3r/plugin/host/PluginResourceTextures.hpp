#pragma once

#include "slic3r/plugin/PythonPluginInterface.hpp"

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace Slic3r {

// Fixed Orca 14a1aa333b6da3ecadf853a4a6285b0f193b7605 ResourceTexture host contract.
struct ResourceTextureEntry
{
    PluginCapabilityId id;
    std::string        plugin_key;
    std::string        display_name;
    std::string        texture_dir;
    std::string        plugin_root;
};

// Persistent entries contain values only. They never own a Python capability, including at exit.
class PluginResourceTextures
{
public:
    static PluginResourceTextures& instance();

    PluginResourceTextures(const PluginResourceTextures&)            = delete;
    PluginResourceTextures& operator=(const PluginResourceTextures&) = delete;

    // Call after PluginManager::initialize(), before discovery/load. Manager shutdown removes the
    // subscriptions; call shutdown() before that and initialize() again for a new manager lifetime.
    void initialize();
    void shutdown();
    void on_plugin_register(const std::string& plugin_key);
    void on_plugin_deregister(const std::string& plugin_key);
    void on_cap_register(const PluginCapabilityId& id);
    void on_cap_deregister(const PluginCapabilityId& id);
    void initial_sweep();

    std::vector<ResourceTextureEntry> entries() const;
    uint64_t generation() const { return m_generation.load(std::memory_order_acquire); }

    // Validate again and pin the contributing package for the entire read. The returned bytes own
    // their storage and remain valid when this pack is disabled, replaced, removed or Python exits.
    // A stale picker entry cannot read a disabled pack, even when the file still exists on disk.
    std::vector<unsigned char> read_texture(const PluginCapabilityId& id, const std::string& path, std::string& error) const;

private:
    PluginResourceTextures() = default;
    mutable std::mutex                                m_mutex;
    std::map<PluginCapabilityId, ResourceTextureEntry> m_entries;
    std::atomic<uint64_t>                             m_generation{0};
    bool                                              m_initialized = false;
};

} // namespace Slic3r
