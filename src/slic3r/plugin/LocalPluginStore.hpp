#pragma once

#include "PluginDescriptor.hpp"

#include <filesystem>
#include <functional>

namespace Slic3r {

// Local filesystem backend. Uses Orca's descriptor, filename-based directory,
// filename-stem key and install-state schema. No plugin code runs in this class.
// The caller unloads live capabilities before disabling or removing a package.
class LocalPluginStore
{
public:
    using Inspector = std::function<bool(const std::filesystem::path&, PluginDescriptor&, std::string&)>;

    LocalPluginStore(std::filesystem::path root, Inspector inspector);
    std::vector<PluginDescriptor> scan(std::string& error) const;
    bool                          install(const std::filesystem::path& source, PluginDescriptor& installed, std::string& error);
    bool                          set_enabled(const std::string& key, bool enabled, std::string& error);
    bool                          remove(const std::string& key, std::string& error);

private:
    std::filesystem::path resolve_package(const std::string& key) const;
    std::filesystem::path m_root;
    Inspector             m_inspector;
};

} // namespace Slic3r
