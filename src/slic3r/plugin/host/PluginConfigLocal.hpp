#pragma once
namespace Slic3r { struct PluginCapabilityId; class CapabilityConfigDocument; }
class wxWindow;
namespace Slic3r::GUI {
void show_local_plugin_config(wxWindow* parent, const PluginCapabilityId& id);
void show_preset_plugin_config(wxWindow* parent, const PluginCapabilityId& id, CapabilityConfigDocument& draft);
}
