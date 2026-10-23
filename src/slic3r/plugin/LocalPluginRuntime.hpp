#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace Slic3r {

struct PluginDescriptor;

// S02 integration seam. This development-only host deliberately accepts only
// explicitly selected, trusted, dependency-free .py packages. The production
// installer/audit/other capabilities are separate migration steps.
class LocalPluginRuntime
{
public:
    struct Script
    {
        std::string package_key;
        std::string name;
        std::string package_version;
    };

    struct Result
    {
        bool        ok = false;
        std::string message;
        std::string data;
    };

    using AppInfo = std::function<nlohmann::json()>;

    LocalPluginRuntime();
    ~LocalPluginRuntime();
    LocalPluginRuntime(const LocalPluginRuntime&)            = delete;
    LocalPluginRuntime& operator=(const LocalPluginRuntime&) = delete;

    bool initialize(const std::filesystem::path& python_home, AppInfo app_info, std::string& error);
    bool load(const std::filesystem::path& file, std::string& error);
    // Parse metadata only; never import or execute the package body.
    bool inspect(const std::filesystem::path& file, PluginDescriptor& descriptor, std::string& error);
    bool is_loaded(const std::string& package_key) const;
    // Always drops the package. False reports an on_unload exception after cleanup.
    bool                unload(const std::string& package_key, std::string& error);
    std::vector<Script> scripts() const;
    Result              execute(const std::string& package_key, const std::string& capability_name);
    void                shutdown();
    bool                initialized() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace Slic3r
