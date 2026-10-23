#pragma once

namespace Slic3r::GUI {
class Plater;
// Initialize and shut down on the UI thread. Subscription callbacks retain only a weak state.
void initialize_plugin_slicing_gui(Plater* plater);
void shutdown_plugin_slicing_gui();
}
