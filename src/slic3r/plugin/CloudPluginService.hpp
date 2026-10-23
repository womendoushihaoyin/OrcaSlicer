#pragma once

#include "PluginDescriptor.hpp"
#include <boost/filesystem/path.hpp>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace Slic3r {
struct CloudPluginDownload
{
    boost::filesystem::path package_path;
};

// Host-side extension point. Phase one never creates a backend or an OrcaCloud
// account. A future Snapmaker backend is injected without changing the SDK.
class PluginCloudBackend
{
public:
    virtual ~PluginCloudBackend()                                                                                              = default;
    virtual std::string get_user_id() const                                                                                    = 0;
    virtual bool        fetch_manifests_into_descriptors(std::vector<PluginDescriptor>&,
                                                         std::vector<std::string>&,
                                                         std::vector<std::string>&) const                                      = 0;
    virtual bool        request_cloud_subscribe(const std::string&, std::string&) const                                        = 0;
    virtual bool        request_cloud_unsubscribe(const PluginDescriptor&, std::string&) const                                 = 0;
    virtual bool        download_cloud_plugin(PluginDescriptor&, const std::string&, CloudPluginDownload&, std::string&) const = 0;
    virtual bool        fetch_plugin_changelog(const std::vector<PluginDescriptor>&,
                                               std::unordered_map<std::string, std::vector<PluginChangelog>>&,
                                               std::string&) const                                                             = 0;
};

class CloudPluginService
{
public:
    void                                set_cloud_agent(std::shared_ptr<PluginCloudBackend> backend) { m_backend = std::move(backend); }
    std::shared_ptr<PluginCloudBackend> get_cloud_agent() const { return m_backend; }
    bool                                can_fetch_cloud_plugins() const { return m_backend && !m_backend->get_user_id().empty(); }
    bool                                fetch_manifests_into_descriptors(std::vector<PluginDescriptor>& entries,
                                                                         std::vector<std::string>&      missing,
                                                                         std::vector<std::string>&      unauthorized) const
    {
        return can_fetch_cloud_plugins() && m_backend->fetch_manifests_into_descriptors(entries, missing, unauthorized);
    }
    bool request_cloud_subscribe(const std::string& id, std::string& error) const
    {
        return available(error) && m_backend->request_cloud_subscribe(id, error);
    }
    bool request_cloud_unsubscribe(const PluginDescriptor& entry, std::string& error) const
    {
        return available(error) && m_backend->request_cloud_unsubscribe(entry, error);
    }
    bool download_cloud_plugin(PluginDescriptor& entry, const std::string& version, CloudPluginDownload& result, std::string& error) const
    {
        return available(error) && m_backend->download_cloud_plugin(entry, version, result, error);
    }
    bool fetch_plugin_changelog(const std::vector<PluginDescriptor>&                           entries,
                                std::unordered_map<std::string, std::vector<PluginChangelog>>& result,
                                std::string&                                                   error) const
    {
        return available(error) && m_backend->fetch_plugin_changelog(entries, result, error);
    }
    bool fetch_plugin_changelog(const PluginDescriptor& entry, std::vector<PluginChangelog>& result, std::string& error) const
    {
        std::unordered_map<std::string, std::vector<PluginChangelog>> all;
        if (!fetch_plugin_changelog(std::vector<PluginDescriptor>{entry}, all, error))
            return false;
        result = all[entry.plugin_key];
        return true;
    }

private:
    bool available(std::string& error) const
    {
        if (can_fetch_cloud_plugins())
            return true;
        error = "Plugin cloud is unavailable in local mode";
        return false;
    }
    std::shared_ptr<PluginCloudBackend> m_backend;
};
} // namespace Slic3r
