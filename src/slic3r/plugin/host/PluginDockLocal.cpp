#include "PluginDockLocal.hpp"
#include "PluginDockLayout.hpp"
#include "libslic3r/AppConfig.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Plater.hpp"
#include <wx/aui/aui.h>
#include <wx/clntdata.h>
#include <wx/weakref.h>
#include <stdexcept>
#include <utility>

namespace Slic3r::GUI {
namespace {
class DockLifetime final : public wxClientData, public wxEvtHandler
{
public:
    DockLifetime(wxWindow* parent, wxWindow* panel, std::function<void()> on_close)
        : m_parent(parent), m_panel(panel), m_on_close(std::move(on_close))
    {
        manager()->Bind(wxEVT_AUI_PANE_CLOSE, &DockLifetime::close, this);
    }
    ~DockLifetime() override
    {
        if (auto* mgr = manager()) {
            mgr->Unbind(wxEVT_AUI_PANE_CLOSE, &DockLifetime::close, this);
            if (m_panel && mgr->GetPane(m_panel.get()).IsOk())
                mgr->DetachPane(m_panel.get());
        }
    }

private:
    wxAuiManager* manager() const { return m_parent ? wxAuiManager::GetManager(m_parent.get()) : nullptr; }
    void          close(wxAuiManagerEvent& event)
    {
        if (!m_panel || event.GetPane()->window != m_panel.get()) {
            event.Skip();
            return;
        }
        event.Veto();
        // Close callbacks can unload their own plugin. Never touch this object
        // again after invoking one, and leave the AUI event stack first.
        const auto panel    = m_panel;
        const auto parent   = m_parent;
        const auto callback = m_on_close;
        m_parent->CallAfter([panel, parent, callback] {
            if (!panel)
                return;
            if (callback)
                callback();
            if (panel && parent) {
                if (auto* mgr = wxAuiManager::GetManager(parent.get())) {
                    mgr->DetachPane(panel.get());
                    mgr->Update();
                }
                panel->Destroy();
            }
        });
    }
    wxWeakRef<wxWindow>   m_parent;
    wxWeakRef<wxWindow>   m_panel;
    std::function<void()> m_on_close;
};
} // namespace
void add_plugin_dock_pane(Plater*               plater,
                          wxWindow*             panel,
                          const std::string&    name,
                          const wxString&       title,
                          const std::string&    dock,
                          const wxSize&         size,
                          std::function<void()> on_close)
{
    auto* manager = wxAuiManager::GetManager(plater);
    if (!manager)
        throw std::runtime_error("Plater docking manager is not available");
    const auto base_name = wxString::FromUTF8(name);
    auto unique_name = base_name;
    for (int suffix = 2; manager->GetPane(unique_name).IsOk(); ++suffix)
        unique_name = base_name + wxString::Format("#%d", suffix);
    wxAuiPaneInfo pane;
    pane.Name(unique_name)
        .Caption(title)
        .BestSize(plater->FromDIP(size))
        .FloatingSize(plater->FromDIP(size))
        .MinSize(plater->FromDIP(wxSize(160, 120)))
        .CloseButton(true)
        .DestroyOnClose(false)
        .Floatable(true);
    if (dock == "left")
        pane.Left();
    else if (dock == "right")
        pane.Right();
    else if (dock == "bottom")
        pane.Bottom();
    else if (dock == "float")
        pane.Float();
    else
        throw std::invalid_argument("Unsupported plugin dock position");
    // Plugin panels are created after Plater loads its initial perspective.
    // Restore their own record when they arrive, as the fixed upstream does.
    if (wxGetApp().app_config) {
        const auto saved = plugin_saved_pane_layout(wxGetApp().app_config->get("window_layout"), unique_name.utf8_string());
        if (!saved.empty()) {
            manager->LoadPaneInfo(wxString::FromUTF8(saved), pane);
            pane.Name(unique_name).Caption(title).DestroyOnClose(false).Show();
        }
    }
    if ((manager->GetFlags() & wxAUI_MGR_ALLOW_FLOATING) == 0) {
        pane.Dock().Floatable(false);
        if (pane.dock_direction == wxAUI_DOCK_NONE)
            pane.Right();
    }
    if (!manager->AddPane(panel, pane))
        throw std::runtime_error("Cannot add plugin dock pane");
    panel->SetClientObject(new DockLifetime(plater, panel, std::move(on_close)));
    const wxWeakRef<wxWindow> parent_ref(plater), panel_ref(panel);
    panel->Bind(wxEVT_IDLE, [parent_ref, panel_ref](wxIdleEvent& event) {
        if (parent_ref && panel_ref) {
            if (auto* mgr = wxAuiManager::GetManager(parent_ref.get())) {
                auto& info = mgr->GetPane(panel_ref.get());
                if (info.IsOk() && info.IsDocked() && info.IsShown() && info.rect.GetWidth() > 0 && info.rect.GetHeight() > 0)
                    info.BestSize(info.rect.GetSize());
            }
        }
        event.Skip();
    });
    manager->Update();
}
void remove_plugin_dock_pane(Plater* plater, wxWindow* panel)
{
    if (auto* manager = wxAuiManager::GetManager(plater)) {
        manager->DetachPane(panel);
        manager->Update();
    }
    // Called outside WebView callbacks for forced teardown; destruct Python
    // message holders before interpreter shutdown, not at the next idle event.
    delete panel;
}
void show_plugin_dock_pane(Plater* plater, wxWindow* panel, bool show)
{
    if (auto* manager = wxAuiManager::GetManager(plater)) {
        auto& pane = manager->GetPane(panel);
        if (pane.IsOk()) {
            pane.Show(show);
            manager->Update();
        }
    }
}
} // namespace Slic3r::GUI
