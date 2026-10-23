#include "PluginHost.hpp"
#include "PluginHostBindings.hpp"
#include "PluginHostCore.hpp"
#include "PluginHostUi.hpp"
#include "slic3r/plugin/PluginAuditManager.hpp"
#include "slic3r/plugin/PluginFsUtils.hpp"
#include "slic3r/plugin/PluginManager.hpp"
#include <stdexcept>
#include "libslic3r/Model.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Plater.hpp"
#include <wx/thread.h>

namespace Slic3r {
namespace {
std::function<nlohmann::json()> app_info_provider;
}
void set_plugin_app_info_provider(std::function<nlohmann::json()> provider) { app_info_provider = std::move(provider); }
void PluginHost::RegisterBindings(pybind11::module_& module)
{
    auto host = module.def_submodule("host", "Host application API");
    host_bindings::register_geometry(host);
    host_bindings::register_mesh(host);
    host_bindings::register_model(host);
    host_bindings::register_slicing(host);
    PluginHostUi::RegisterBindings(host);
    host_bindings::register_presets(host);
    host_bindings::register_app(host);
    host.def("app_info", [] {
        if (!app_info_provider)
            throw std::runtime_error("Application is not initialized");
        return json_to_py(app_info_provider());
    }, "The running app: version, build, mode, language, app_config_version, stealth_mode, "
       "is_signed_in (the latest received Snapmaker connection-gateway account snapshot; retained across transport disconnects, "
       "false before any signed-in snapshot; not plugin-cloud entitlement), is_bambu_signed_in (legacy Bambu Cloud), "
       "network_plugin_version (empty when no Bambu module/version is available), and network_plugin_error "
       "(observed load, version-compatibility or unload diagnostic; empty if none). UI thread only.");
    host.def_submodule("plugin").def("storage", [] {
        const auto key = PluginAuditManager::instance().current_plugin();
        if (key.empty())
            throw std::runtime_error("plugin.storage() must be called from a plugin callback");
        return PluginManager::instance().get_storage_dir(key);
    });
}
} // namespace Slic3r
