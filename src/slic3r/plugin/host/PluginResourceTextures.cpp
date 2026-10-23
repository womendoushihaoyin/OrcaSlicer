#include "PluginResourceTextures.hpp"

#include "slic3r/plugin/PluginAuditManager.hpp"
#include "slic3r/plugin/PluginFsUtils.hpp"
#include "slic3r/plugin/PluginManager.hpp"
#include "slic3r/plugin/PythonInterpreter.hpp"
#include "slic3r/plugin/pluginTypes/resourceTexture/ResourceTexturePluginCapability.hpp"

#include <algorithm>
#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>
#include <limits>
#include <memory>

namespace Slic3r {

PluginResourceTextures& PluginResourceTextures::instance()
{
    static PluginResourceTextures registry;
    return registry;
}

void PluginResourceTextures::initialize()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_initialized)
            return;
        m_initialized = true;
    }
    auto& manager = PluginManager::instance();
    manager.subscribe_on_load_callback([](const std::string& key) { instance().on_plugin_register(key); });
    manager.subscribe_on_unload_callback([](const std::string& key) { instance().on_plugin_deregister(key); });
    manager.subscribe_on_capability_load_callback([](const PluginCapabilityId& id) { instance().on_cap_register(id); });
    manager.subscribe_on_capability_unload_callback([](const PluginCapabilityId& id) { instance().on_cap_deregister(id); });
    initial_sweep();
}

void PluginResourceTextures::shutdown()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_initialized = false;
    m_entries.clear();
    m_generation.fetch_add(1, std::memory_order_release);
}

void PluginResourceTextures::on_plugin_register(const std::string& plugin_key)
{
    auto runtime = PythonInterpreter::instance().acquire_runtime_lease();
    if (!runtime)
        return;
    // Copy identities, then drop the unpinned capabilities before doing work. Each registration
    // resolves the current instance atomically and pins it through publication below.
    std::vector<PluginCapabilityId> ids;
    for (const auto& capability : PluginManager::instance().get_plugin_capabilities(plugin_key, PluginCapabilityType::ResourceTexture))
        if (capability)
            ids.push_back(capability->identity());
    for (const auto& id : ids)
        on_cap_register(id);
}

void PluginResourceTextures::on_plugin_deregister(const std::string& plugin_key)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    bool changed = false;
    for (auto it = m_entries.begin(); it != m_entries.end();) {
        if (it->first.plugin_key == plugin_key) {
            it = m_entries.erase(it);
            changed = true;
        } else {
            ++it;
        }
    }
    if (changed)
        m_generation.fetch_add(1, std::memory_order_release);
}

void PluginResourceTextures::on_cap_register(const PluginCapabilityId& id)
{
    if (id.type != PluginCapabilityType::ResourceTexture)
        return;
    auto runtime = PythonInterpreter::instance().acquire_runtime_lease();
    if (!runtime)
        return;
    std::string error;
    // Keep the aliasing consumer handle alive until after m_mutex is released. Otherwise an unload
    // could deregister between validation and publication and leave a stale entry behind.
    const auto capability = PluginManager::instance().acquire_capability(id, error);
    const auto* texture = dynamic_cast<const ResourceTexturePluginCapability*>(capability.get());
    if (!texture || texture->texture_dir().empty())
        return;
    PluginDescriptor descriptor;
    if (!PluginManager::instance().try_get_plugin_descriptor(id.plugin_key, descriptor))
        return;
    ResourceTextureEntry entry{id, id.plugin_key, texture->name(), texture->texture_dir(), descriptor.plugin_root};
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_initialized)
        return;
    m_entries[id] = std::move(entry);
    // Even a same-path re-registration may have different bytes: invalidate failed thumbnails too.
    m_generation.fetch_add(1, std::memory_order_release);
}

void PluginResourceTextures::on_cap_deregister(const PluginCapabilityId& id)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_entries.erase(id))
        m_generation.fetch_add(1, std::memory_order_release);
}

void PluginResourceTextures::initial_sweep()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_entries.clear();
        m_generation.fetch_add(1, std::memory_order_release);
    }
    on_plugin_register({});
}

std::vector<ResourceTextureEntry> PluginResourceTextures::entries() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<ResourceTextureEntry> result;
    result.reserve(m_entries.size());
    for (const auto& entry : m_entries)
        result.push_back(entry.second);
    return result;
}

std::vector<unsigned char> PluginResourceTextures::read_texture(const PluginCapabilityId& id, const std::string& path,
                                                               std::string& error) const
{
    error.clear();
    if (id.type != PluginCapabilityType::ResourceTexture) {
        error = "The selected capability does not provide textures.";
        return {};
    }
    auto runtime = PythonInterpreter::instance().acquire_runtime_lease();
    if (!runtime) {
        error = "The plugin runtime is no longer available.";
        return {};
    }
    const auto capability = PluginManager::instance().acquire_capability(id, error);
    const auto* texture = dynamic_cast<const ResourceTexturePluginCapability*>(capability.get());
    if (!texture) {
        if (error.empty())
            error = "The texture pack is no longer available.";
        return {};
    }
    PluginDescriptor descriptor;
    if (!PluginManager::instance().try_get_plugin_descriptor(id.plugin_key, descriptor)) {
        error = "The texture pack is no longer available.";
        return {};
    }
    const auto files = collect_resource_texture_files(texture->texture_dir(), descriptor.plugin_root, error);
    if (!error.empty())
        return {};
    // Compare the scanner's paths before canonicalizing the selected file: this refuses aliases and
    // files outside the supported image set, and repeats the per-file symlink containment check.
    if (std::find(files.begin(), files.end(), path) == files.end()) {
        error = "The selected image is no longer in the texture pack.";
        return {};
    }
    boost::system::error_code ec;
    const auto resolved = boost::filesystem::canonical(boost::filesystem::path(path), ec);
    if (ec || !is_inside_allowed_root(resolved, boost::filesystem::path(descriptor.plugin_root))) {
        error = "The texture image is outside the plugin folder or cannot be resolved.";
        return {};
    }
    boost::nowide::ifstream input(resolved.string(), std::ios::binary | std::ios::ate);
    const auto size = input.tellg();
    // Bounded before allocation; dimensions/decoding are checked by the image consumer.
    constexpr std::streamoff max_encoded_size = 128 * 1024 * 1024;
    if (!input || size <= 0 || size > max_encoded_size) {
        error = "The texture image cannot be read or exceeds the 128 MiB limit.";
        return {};
    }
    std::vector<unsigned char> bytes(static_cast<size_t>(size));
    input.seekg(0);
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!input || input.peek() != std::char_traits<char>::eof()) {
        error = "The texture image changed or could not be read completely.";
        return {};
    }
    return bytes;
}

} // namespace Slic3r
