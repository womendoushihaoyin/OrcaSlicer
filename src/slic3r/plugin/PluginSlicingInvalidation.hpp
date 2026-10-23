#pragma once

#include "PluginCapabilityRef.hpp"
#include "libslic3r/PrintConfig.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace Slic3r {
// Values only: safe to enqueue after a Python capability or its package has been destroyed.
struct PluginSlicingChange {
    std::string plugin_key;
    std::string capability_name; // Empty means every capability in the package.
    bool base_config_only = false;
};

inline bool slicing_plugins_affected(const DynamicPrintConfig& config, const std::vector<PluginSlicingChange>& changes)
{
    const auto* selected = config.option<ConfigOptionStrings>("slicing_pipeline_plugin");
    const auto* references = config.option<ConfigOptionStrings>("plugins");
    if (!selected || !references || selected->values.empty()) return false;
    nlohmann::json overrides;
    if (const auto* raw = config.option<ConfigOptionString>("print_plugin_config_overrides"))
        overrides = nlohmann::json::parse(raw->value, nullptr, false);
    for (const auto& value : references->values) {
        const auto ref = parse_capability_ref(value);
        if (!ref || std::find(selected->values.begin(), selected->values.end(), ref->capability_name) == selected->values.end()) continue;
        const auto& key = ref->uuid.empty() ? ref->name : ref->uuid;
        for (const auto& change : changes) {
            if (key != change.plugin_key || (!change.capability_name.empty() && ref->capability_name != change.capability_name)) continue;
            bool overridden = false;
            if (change.base_config_only && overrides.is_array()) {
                for (const auto& entry : overrides) {
                    if (!entry.is_object()) continue;
                    const auto matches = [&](const char* name, const std::string& expected) {
                        const auto it = entry.find(name);
                        return it != entry.end() && it->is_string() && it->get_ref<const std::string&>() == expected;
                    };
                    const auto type = entry.find("capability_type");
                    std::string kind = type != entry.end() && type->is_string() ? type->get<std::string>() : std::string();
                    std::transform(kind.begin(), kind.end(), kind.begin(), [](unsigned char c) { return char(std::tolower(c)); });
                    // Match CapabilityConfigDocument's legacy Unknown fallback as well as typed entries.
                    const bool slicing_type = kind != "printer-connection" && kind != "pages" && kind != "analysis" &&
                        kind != "importer" && kind != "exporter" && kind != "visualization" && kind != "script" && kind != "resource-texture";
                    if (slicing_type && matches("plugin_key", key) && matches("capability", ref->capability_name)) {
                        overridden = true;
                        break;
                    }
                }
            }
            if (!overridden) return true;
        }
    }
    return false;
}
} // namespace Slic3r
