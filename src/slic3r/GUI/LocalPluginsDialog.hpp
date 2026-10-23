#pragma once

#include <filesystem>
#include <functional>

class wxWindow;

namespace Slic3r::GUI {

void show_local_plugin_spike(wxWindow* parent);
void start_local_plugin_spike();
void schedule_local_plugin_start(std::function<bool()> ready, std::function<void()> run);
void shutdown_local_plugin_spike();
void record_local_plugin_smoke_exit(int result);
void run_local_plugin_smoke(const std::filesystem::path& file, const std::filesystem::path& report);

} // namespace Slic3r::GUI
