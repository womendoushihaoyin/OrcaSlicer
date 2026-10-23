// Exercise real plugin-created panes and the host's normal close/save path.
#include "slic3r/GUI/DockPanel.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "libslic3r/AppConfig.hpp"
#include <wx/aui/aui.h>
#include <wx/evtloop.h>
#include <wx/utils.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace Slic3r::GUI {
namespace {
void check(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}
nlohmann::json geometry(const wxAuiPaneInfo& pane)
{
    return {{"name", pane.name.utf8_string()}, {"caption", pane.caption.utf8_string()},
            {"direction", pane.dock_direction}, {"layer", pane.dock_layer},
            {"row", pane.dock_row}, {"position", pane.dock_pos},
            {"width", pane.best_size.GetWidth()}, {"height", pane.best_size.GetHeight()},
            {"shown", pane.IsShown()}};
}
void pump()
{
    if (auto* loop = wxEventLoopBase::GetActive()) loop->YieldFor(wxEVT_CATEGORY_ALL);
    wxMilliSleep(5);
}
} // namespace

nlohmann::json test_plugin_dock_restart(const std::string& plugin_key,
                                        const std::filesystem::path& state_file, bool restarted)
{
    auto* plater = wxGetApp().plater();
    check(plater && plater->is_sidebar_enabled() && wxGetApp().app_config,
          "Dock restart requires the real editor and isolated app config");
    auto* manager = wxAuiManager::GetManager(plater);
    check(manager != nullptr, "Dock restart has no AUI manager");
    const auto name = wxString::FromUTF8(plugin_pane_name(plugin_key, "Restart layout α | ; = \\"));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while ((!manager->GetPane(name).IsOk() || !manager->GetPane(name + "#2").IsOk()) &&
           std::chrono::steady_clock::now() < deadline)
        pump();
    check(manager->GetPane(name).IsOk() && manager->GetPane(name + "#2").IsOk(),
          "Plugin-created duplicate docks did not arrive");
    nlohmann::json expected;
    if (restarted) {
        std::ifstream input(state_file);
        check(bool(input), "Dock restart has no first-process evidence");
        input >> expected;
        check(expected.at("pid").get<long>() != static_cast<long>(wxGetProcessId()),
              "Dock restart reused the first process");
        check(!wxGetApp().app_config->get("window_layout").empty(), "Normal close did not persist AUI layout");
    } else {
        check(!std::filesystem::exists(state_file), "Dock first process requires fresh state");
        manager->GetPane(name).Bottom().Layer(2).Row(1).Position(3).BestSize(wxSize(390, 170));
        manager->GetPane(name + "#2").Left().Layer(1).Row(2).Position(4).BestSize(wxSize(275, 210));
        manager->Update();
        // Allow actual dock-size tracking to settle before preserving the expected state.
        for (int i = 0; i < 20; ++i) pump();
    }
    nlohmann::json panes = nlohmann::json::array({geometry(manager->GetPane(name)), geometry(manager->GetPane(name + "#2"))});
    if (restarted) {
        // Persist both sides before throwing: the outer smoke report intentionally captures only
        // the exception text, so without this file a failed restart loses the observed AUI state.
        const nlohmann::json diagnostic = {
            {"pid", static_cast<long>(wxGetProcessId())}, {"first_pid", expected.at("pid")},
            {"expected", expected.at("panes")}, {"actual", panes},
            {"differences", nlohmann::json::diff(expected.at("panes"), panes)},
            {"saved_perspective", wxGetApp().app_config->get("window_layout")},
            {"current_perspective", manager->SavePerspective().utf8_string()}};
        auto observation = state_file;
        observation += ".observed-" + std::to_string(static_cast<long>(wxGetProcessId())) + ".json";
        std::ofstream output(observation);
        output << diagnostic.dump(2) << '\n';
        output.close();
        check(bool(output), "Could not write dock restart comparison evidence");
        check(panes == expected.at("panes"), "Normal close/restart did not restore independent dock placement and size");
    } else {
        std::ofstream output(state_file);
        output << nlohmann::json{{"pid", static_cast<long>(wxGetProcessId())}, {"panes", panes}}.dump(2) << '\n';
        check(bool(output), "Could not write expected dock layout");
    }
    // Keep both plugin-owned panes alive. MainFrame::Close, not this helper, saves the
    // perspective; GUI_App::OnExit subsequently shuts down the interpreter.
    return {{"panes", panes}, {"normal_close_requested", true}, {"restored_after_restart", restarted}};
}
} // namespace Slic3r::GUI
