#pragma once
#include "slic3r/plugin/PythonPluginInterface.hpp"
class Notebook;
namespace Slic3r {
void initialize_local_plugin_pages(Notebook* notebook);
void shutdown_local_plugin_pages();
bool select_local_plugin_page(const PluginCapabilityId& id);
} // namespace Slic3r
