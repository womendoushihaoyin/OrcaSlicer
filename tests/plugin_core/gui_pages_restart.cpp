// Isolated real GUI helper. No wxAppConsole substitution and no user profile.
#include "slic3r/plugin/PluginManager.hpp"
#include "slic3r/plugin/host/PluginPagesLocal.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Notebook.hpp"
#include <wx/webview.h>
#include <wx/weakref.h>
#include <future>
#include <wx/evtloop.h>
#include <wx/thread.h>
#include <wx/utils.h>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace Slic3r::GUI {
namespace {
void check_pages(bool condition, const char* error)
{
    if (!condition) throw std::runtime_error(error);
}
void pump_pages()
{
    if (auto* loop = wxEventLoopBase::GetActive()) loop->YieldFor(wxEVT_CATEGORY_ALL);
    wxMilliSleep(5);
}
nlohmann::json page_roundtrips(const std::string& key, const nlohmann::json& previous)
{
    auto& manager = PluginManager::instance();
    nlohmann::json result;
    for (const std::string name : {"Phase1 Page Alpha", "Phase1 Page Beta"}) {
        const PluginCapabilityId id{PluginCapabilityType::Pages, name, key};
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        bool done = false;
        while (std::chrono::steady_clock::now() < deadline) {
            pump_pages();
            auto* notebook = wxGetApp().tab_panel();
            check_pages(notebook != nullptr, "Missing real Pages notebook");
            for (size_t index = 0; index < notebook->GetPageCount(); ++index) {
                auto* page = notebook->GetPage(index);
                check_pages(page == notebook->GetCurrentPage() || !page->IsShown(),
                            "An unselected notebook page is covering the current tab");
            }
            if (!select_local_plugin_page(id)) continue;
            const auto stored = manager.get_config().get_config(id);
            if (!stored) continue;
            const auto& config = stored->config;
            if (!config.value("roundtrip", false) || config.value("page", std::string()) != name) continue;
            check_pages(config.value("icon_callback", false), "Public Pages get_icon callback was not invoked");
            const auto session = config.value("current_session", std::string());
            if (session.empty()) continue;
            if (previous.contains(name)) {
                const auto old = previous.at(name).at("current_session").get<std::string>();
                if (session == old) continue;
                const auto& sessions = config.at("sessions");
                check_pages(std::find(sessions.begin(), sessions.end(), old) != sessions.end(), "Restart lost prior page configuration");
                check_pages(sessions.size() > previous.at(name).at("sessions").size(), "New page generation did not persist");
            }
            result[name] = config;
            done = true;
            break;
        }
        check_pages(done, "Independent page bridge did not complete for a new generation");
    }
    check_pages(result.at("Phase1 Page Alpha").at("current_session") == result.at("Phase1 Page Beta").at("current_session"),
                "Pages were attached to different package generations");
    return result;
}
wxWebView* find_webview(wxWindow* parent)
{
    if (auto* view = dynamic_cast<wxWebView*>(parent)) return view;
    for (auto* child : parent->GetChildren())
        if (auto* view = find_webview(child)) return view;
    return nullptr;
}
void stale_page_message_after_worker_reload(const std::string& key, const nlohmann::json& previous)
{
    auto& manager = PluginManager::instance();
    const std::string name = "Phase1 Page Alpha";
    const PluginCapabilityId id{PluginCapabilityType::Pages, name, key};
    auto* notebook = wxGetApp().tab_panel();
    check_pages(notebook != nullptr, "Missing real Pages notebook");
    check_pages(select_local_plugin_page(id), "Could not select old real Page by capability identity");
    // Notebook decorates its displayed labels; select by the actual capability
    // identity instead of treating presentation text as a stable identifier.
    wxWeakRef<wxWebView> old_view = find_webview(notebook->GetCurrentPage());
    check_pages(bool(old_view), "Could not locate old real WebView");

    // Leave the unload observer pending on the real GUI queue, then load the new
    // capability before pumping. This is distinct from overlapping on_unload.
    check_pages(std::async(std::launch::async, [&] { return manager.unload_plugin(key); }).get(),
                "Worker could not unload Pages");
    manager.load_plugin(key);
    std::string error;
    check_pages(manager.wait_for_plugin_load(key, std::chrono::seconds(30), error), "Worker reload failed");
    check_pages(bool(old_view), "Fixture unexpectedly dispatched Pages teardown");
    auto current = previous.at(name);
    current["current_session"] = "new-generation-config-before-observer";
    check_pages(manager.get_config().store_capability_config(id, current), "Cannot persist new-generation sentinel");

    // Dispatch the same wx bridge event used by WebKit while the old native page
    // still exists. The existing roundtrips exercise the actual JavaScript bridge;
    // this explicit dispatch makes the late-arrival boundary deterministic.
    const nlohmann::json payload{{"channel", "orca"}, {"kind", "message"},
        {"data", {{"kind", "ack"}, {"page", name},
                  {"session", previous.at(name).at("current_session")}}}};
    wxWebViewEvent event(wxEVT_WEBVIEW_SCRIPT_MESSAGE_RECEIVED, old_view->GetId(), "", "");
    event.SetString(wxString::FromUTF8(payload.dump()));
    old_view->GetEventHandler()->ProcessEvent(event);
    const auto after = manager.get_config().get_config(id);
    check_pages(after && after->config == current, "Late old Page message overwrote new generation configuration");
    check_pages(manager.get_config().store_capability_config(id, previous.at(name)), "Cannot restore roundtrip fixture state");
}
} // namespace

nlohmann::json test_plugin_pages_restart(const std::string& plugin_key, const std::filesystem::path& state_file, bool restarted)
{
    check_pages(wxIsMainThread() && wxEventLoopBase::GetActive(), "Pages test requires the real active GUI loop");
    auto& manager = PluginManager::instance();
    check_pages(manager.get_plugin_capabilities(plugin_key, PluginCapabilityType::Pages).size() == 2,
                "Multi-page fixture must register exactly two Pages");
    nlohmann::json previous;
    if (restarted) {
        std::ifstream input(state_file);
        check_pages(bool(input), "Missing first-launch page evidence");
        input >> previous;
        previous = previous.at("pages");
    } else {
        check_pages(!std::filesystem::exists(state_file), "First-launch page evidence must be fresh");
    }
    auto pages = page_roundtrips(plugin_key, previous);
    stale_page_message_after_worker_reload(plugin_key, pages);
    pages = page_roundtrips(plugin_key, pages);
    nlohmann::json result{{"pages", pages}, {"inactive_pages_hidden", true}, {"independent_bridges", true}, {"rapid_reload_generation", true},
                          {"late_old_page_message_rejected", true}, {"persisted_across_restart", restarted}};
    std::ofstream output(state_file);
    output << result.dump(2) << '\n';
    check_pages(bool(output), "Could not save page evidence");
    return result;
}
} // namespace Slic3r::GUI
