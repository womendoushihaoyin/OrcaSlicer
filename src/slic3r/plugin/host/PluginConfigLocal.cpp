#include "PluginConfigLocal.hpp"
#include "slic3r/plugin/PluginConfig.hpp"
#include "slic3r/plugin/PluginManager.hpp"
#include "slic3r/GUI/WebDialog.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "libslic3r/Utils.hpp"
#include <boost/nowide/fstream.hpp>
#include <boost/filesystem/path.hpp>
#include <wx/msgdlg.h>
#include <wx/thread.h>
#include <stdexcept>

namespace Slic3r::GUI {
void show_local_plugin_config(wxWindow* parent, const PluginCapabilityId& id)
{
    if (!wxIsMainThread())
        throw std::runtime_error("Plugin configuration must be opened on the UI thread");
    const auto path = boost::filesystem::path(resources_dir()) / "web/dialog/LocalPluginConfig/index.html";
    boost::nowide::ifstream input(path.string());
    if (!input)
        throw std::runtime_error("Plugin configuration page is missing");
    const std::string html((std::istreambuf_iterator<char>(input)), {});
    WebDialog* window = nullptr;
    WebDialog dialog(parent, wxString::FromUTF8(id.name), html, wxSize(800, 600),
        [&](const nlohmann::json& message) {
            if (!message.is_object()) return;
            const auto action = message.value("action", std::string());
            nlohmann::json response;
            if (action == "get") {
                response = PluginConfig::get_config_response(id);
            } else if (action == "save") {
                response = PluginConfig::save_config_response(id, message.value("config", nlohmann::json::object()));
            } else if (action == "restore") {
                if (wxMessageBox(_L("Restore the default configuration?"), _L("Plugin configuration"),
                                 wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION, window) != wxYES)
                    return;
                response = PluginConfig::restore_config_response(id);
            } else return;
            // Identity always comes from the selected capability, never from HTML.
            window->push_message(response);
        }, {}, {}, {});
    window = &dialog;
    dialog.ShowModal();
}

void show_preset_plugin_config(wxWindow* parent, const PluginCapabilityId& id, CapabilityConfigDocument& draft)
{
    if (!wxIsMainThread()) throw std::runtime_error("Plugin configuration must be opened on the UI thread");
    const auto path = boost::filesystem::path(resources_dir()) / "web/dialog/LocalPluginConfig/index.html";
    boost::nowide::ifstream input(path.string());
    if (!input) throw std::runtime_error("Plugin configuration page is missing");
    const std::string html((std::istreambuf_iterator<char>(input)), {});
    PresetPluginConfigService service;
    WebDialog* window = nullptr;
    WebDialog dialog(parent, wxString::FromUTF8(id.name) + " — " + _L("Process preset"), html, wxSize(800, 600),
        [&](const nlohmann::json& message) {
            if (!message.is_object()) return;
            const auto action = message.value("action", std::string());
            nlohmann::json response;
            if (action == "get") {
                SlicingPluginExecutionScope::Configs configs{{id, service.get_effective_config(draft, id)}};
                SlicingPluginExecutionScope scope(configs);
                response = PluginConfig::get_config_response(id);
                response["config"] = configs.at(id).config;
                response["has_preset_override"] = configs.at(id).has_preset_override;
                response["scope"] = "preset";
                response["restore_label"] = _u8L("Use global configuration");
            } else if (action == "save" || action == "restore") {
                response = {{"command", "capability_config_saved"}, {"ok", false}, {"error", ""}};
                if (!PluginManager::instance().get_plugin_capability(id, false)) {
                    response["error"] = _u8L("This capability is no longer available. Your changes were not saved.");
                } else {
                    auto value = message.value("config", nlohmann::json::object());
                    if (action == "save" && value.is_string())
                        value = nlohmann::json::parse(value.get<std::string>(), nullptr, false);
                    if (value.is_discarded()) {
                        response["error"] = _u8L("The configuration is not valid JSON. Your changes were not saved.");
                    } else {
                        const auto result = action == "restore" ? service.remove_preset_override(draft, id) :
                                                                  service.set_preset_override(draft, id, value);
                        response["ok"] = result.ok;
                        response["error"] = result.error;
                        response["config"] = result.effective.config;
                        response["has_preset_override"] = result.effective.has_preset_override;
                        response["scope"] = "preset";
                    }
                }
            } else return;
            // Only the caller's draft changes. Cancel in the process editor discards
            // it, and neither this dialog nor the iframe can write global config.
            window->push_message(response);
        }, {}, {}, {});
    window = &dialog;
    dialog.ShowModal();
}
} // namespace Slic3r::GUI
