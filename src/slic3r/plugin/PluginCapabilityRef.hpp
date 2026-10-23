#pragma once
#include <optional>
#include <string>
#include <utility>
namespace Slic3r {
struct PluginCapabilityRef
{
    std::string name;
    std::string capability_name;
    std::string uuid;
};

inline std::optional<PluginCapabilityRef> parse_capability_ref(const std::string& value)
{
    // Capability references are stored as "<plugin_name>;<cloud_uuid>;<capability_name>".
    // The cloud UUID is empty for local plugins (two consecutive semicolons).
    if (value.empty())
        return std::nullopt;

    const size_t first = value.find(';');
    if (first == std::string::npos)
        return std::nullopt;
    const size_t second = value.find(';', first + 1);
    if (second == std::string::npos)
        return std::nullopt;

    std::string name            = value.substr(0, first);
    std::string uuid            = value.substr(first + 1, second - first - 1);
    std::string capability_name = value.substr(second + 1);
    if (name.empty() || capability_name.empty())
        return std::nullopt;

    return PluginCapabilityRef{std::move(name), std::move(capability_name), std::move(uuid)};
}

} // namespace Slic3r
