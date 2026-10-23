#include "LocalPluginStore.hpp"
#include "PluginFsUtils.hpp"
#include "PluginManager.hpp"
#include <stdexcept>

namespace Slic3r {
LocalPluginStore::LocalPluginStore(std::filesystem::path root, Inspector inspector)
    : m_root(std::move(root)), m_inspector(std::move(inspector))
{
    if (std::filesystem::weakly_canonical(m_root) != std::filesystem::weakly_canonical(std::filesystem::u8path(get_orca_plugins_dir())))
        throw std::invalid_argument("Plugin store must use the current application data directory");
}
std::vector<PluginDescriptor> LocalPluginStore::scan(std::string& error) const
{
    return discover_plugin_packages({m_root.u8string()}, error);
}
bool LocalPluginStore::install(const std::filesystem::path& source, PluginDescriptor& installed, std::string& error)
{
    auto& manager = PluginManager::instance();
    if (!manager.install_plugin(boost::filesystem::path(source.u8string()), installed, error))
        return false;
    manager.rescan_plugins();
    if (!manager.try_get_valid_plugin_descriptor(installed.plugin_key, installed)) {
        error = "Installed package was not found after discovery";
        return false;
    }
    return true;
}
bool LocalPluginStore::set_enabled(const std::string& key, bool enabled, std::string& error)
{
    error.clear();
    auto&            manager = PluginManager::instance();
    PluginDescriptor descriptor;
    if (!manager.try_get_plugin_descriptor(key, descriptor) || descriptor.is_cloud_plugin()) {
        error = "Local plugin is not installed";
        return false;
    }
    PluginInstallState state;
    if (!read_install_state(descriptor.plugin_root, state)) {
        error = "Plugin install state is invalid";
        return false;
    }
    state.enabled = enabled;
    if (!write_install_state(descriptor.plugin_root, state)) {
        error = "Cannot persist plugin enabled state";
        return false;
    }
    manager.rescan_plugins();
    return true;
}
bool LocalPluginStore::remove(const std::string& key, std::string& error) { return PluginManager::instance().delete_plugin(key, error); }
} // namespace Slic3r
