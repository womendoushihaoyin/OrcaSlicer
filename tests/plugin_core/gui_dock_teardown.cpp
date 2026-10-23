// Call from the isolated GUI smoke application, with its real GUI_App/Plater.
// This file deliberately has no fake wxApp and never runs in the headless target.
#include "slic3r/GUI/DockPanel.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/plugin/host/PluginDockLocal.hpp"
#include "libslic3r/AppConfig.hpp"
#include <memory>
#include <stdexcept>
#include <wx/aui/aui.h>
#include <wx/panel.h>
#include <wx/weakref.h>

namespace Slic3r::GUI {
namespace {
class TestDockPanel final : public DockPanel
{
public:
    using DockPanel::DockPanel;
    using DockPanel::on_page_message;
};
void check(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}
} // namespace
void test_plugin_dock_reentrant_teardown(Plater* plater)
{
    check(plater != nullptr, "Dock teardown requires the real GUI Plater");
    int closed = 0, destroyed = 0;
    TestDockPanel* closing = nullptr;
    closing = new TestDockPanel(plater, "<p>Close callback teardown</p>", {}, [&] {
        ++closed;
        closing->destroy_silently();
    }, [&] { ++destroyed; });
    const wxWeakRef<TestDockPanel> closing_ref(closing);
    closing->request_close();
    check(!closing_ref && closed == 1 && destroyed == 1, "Self-unloading close callback left a live dock");

    auto owner = std::make_shared<int>(7);
    const std::weak_ptr<int> weak_owner(owner);
    bool alive_in_callback = false;
    TestDockPanel* messaging = nullptr;
    messaging = new TestDockPanel(plater, "<p>Message callback teardown</p>",
        [owned = owner, &weak_owner, &messaging, &alive_in_callback](const nlohmann::json&) {
            messaging->destroy_silently();
            alive_in_callback = !weak_owner.expired();
        }, {}, [&] { ++destroyed; });
    owner.reset();
    const wxWeakRef<TestDockPanel> messaging_ref(messaging);
    check(messaging->on_page_message("message", nlohmann::json::object()), "Dock rejected a normal message");
    check(!messaging_ref && destroyed == 2 && alive_in_callback && weak_owner.expired(),
          "Self-unloading message callback lost its callable before returning");
}

nlohmann::json test_plugin_dock_layout_restore(Plater* plater)
{
    check(plater && wxGetApp().app_config, "Dock layout test requires the real GUI and isolated app config");
    auto* manager = wxAuiManager::GetManager(plater);
    check(manager != nullptr, "Dock layout test has no AUI manager");
    const auto before = manager->SavePerspective();
    const auto saved = wxGetApp().app_config->get("window_layout");
    std::vector<wxWeakRef<wxWindow>> panels;
    struct Restore {
        Plater* plater;
        wxAuiManager* manager;
        wxString before;
        std::string saved;
        std::vector<wxWeakRef<wxWindow>>& panels;
        ~Restore()
        {
            for (auto& panel : panels)
                if (panel) remove_plugin_dock_pane(plater, panel.get());
            manager->LoadPerspective(before);
            wxGetApp().app_config->set("window_layout", saved);
        }
    } restore{plater, manager, before, saved, panels};
    const std::string name = "plugin:isolated-layout-contract:duplicate";
    const auto create = [&] {
        auto* panel = new wxPanel(plater);
        panels.emplace_back(panel);
        add_plugin_dock_pane(plater, panel, name, wxString::FromUTF8("Layout α | ; = \\"), "right", wxSize(240, 180), {});
        return panel;
    };
    auto* first = create();
    auto* second = create();
    check(manager->GetPane(first).name == wxString::FromUTF8(name) &&
          manager->GetPane(second).name == wxString::FromUTF8(name + "#2"), "Duplicate dock titles shared a pane identity");
    manager->GetPane(first).Bottom().Layer(2).Row(1).Position(3).BestSize(wxSize(390, 170));
    manager->GetPane(second).Left().Layer(1).Row(2).Position(4).BestSize(wxSize(275, 210));
    manager->Update();
    const auto first_info = manager->GetPane(first);
    const auto second_info = manager->GetPane(second);
    check(first_info.dock_direction == wxAUI_DOCK_BOTTOM && second_info.dock_direction == wxAUI_DOCK_LEFT,
          "Dock fixture did not move panes away from their defaults");
    wxGetApp().app_config->set("window_layout", manager->SavePerspective().utf8_string());
    remove_plugin_dock_pane(plater, first);
    remove_plugin_dock_pane(plater, second);
    first = create();
    second = create();
    const auto& a = manager->GetPane(first);
    const auto& b = manager->GetPane(second);
    check(a.name == wxString::FromUTF8(name) && b.name == wxString::FromUTF8(name + "#2"), "Reopened duplicate docks changed identity");
    check(a.dock_direction == first_info.dock_direction && a.dock_layer == first_info.dock_layer &&
          a.dock_row == first_info.dock_row && a.dock_pos == first_info.dock_pos &&
          a.best_size == first_info.best_size && a.IsShown(), "First dock did not restore saved placement/size");
    check(b.dock_direction == second_info.dock_direction && b.dock_layer == second_info.dock_layer &&
          b.dock_row == second_info.dock_row && b.dock_pos == second_info.dock_pos &&
          b.best_size == second_info.best_size && b.IsShown(), "Second dock did not restore its independent placement/size");
    return {{"duplicate_identity", true}, {"saved_placement_restored", true}, {"saved_size_restored", true},
            {"escaped_caption", true}};
}
} // namespace Slic3r::GUI
