#pragma once
#include <functional>
#include <string>
#include <wx/gdicmn.h>
#include <wx/string.h>
class wxWindow;
namespace Slic3r::GUI {
class Plater;
void add_plugin_dock_pane(Plater*               plater,
                          wxWindow*             panel,
                          const std::string&    name,
                          const wxString&       title,
                          const std::string&    dock,
                          const wxSize&         size,
                          std::function<void()> on_close);
void remove_plugin_dock_pane(Plater* plater, wxWindow* panel);
void show_plugin_dock_pane(Plater* plater, wxWindow* panel, bool show);
} // namespace Slic3r::GUI
