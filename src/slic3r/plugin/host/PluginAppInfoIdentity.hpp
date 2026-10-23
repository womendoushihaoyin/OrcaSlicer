#pragma once

#include "slic3r/Utils/NetworkAgent.hpp"
#include <nlohmann/json.hpp>

namespace Slic3r {

// Keep the host's own account separate from Bambu's legacy network identity.
// Only booleans cross this boundary: no gateway profile, token or entitlement.
inline void append_plugin_app_info_identity(nlohmann::json& info, bool snapmaker_signed_in, bool bambu_signed_in,
                                           const NetworkModuleSnapshot& network)
{
    info["is_signed_in"] = snapmaker_signed_in;
    info["is_bambu_signed_in"] = bambu_signed_in;
    info["network_plugin_version"] = network.loaded ? network.version : std::string{};
    info["network_plugin_error"] = network.error;
}

} // namespace Slic3r
