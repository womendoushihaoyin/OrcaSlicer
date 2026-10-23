#pragma once
#include <cstddef>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>
namespace Slic3r::GUI {
// Development-only workflow. Refuses to run without workflow mode and the exact isolated profile.
nlohmann::json run_plugin_workflow_smoke(const std::filesystem::path& report);
nlohmann::json run_plugin_texture_smoke(const std::filesystem::path& report);
// Passive observations of the actual dialog/render path; no-op outside the isolated smoke run.
bool plugin_texture_smoke_active();
void record_plugin_texture_preview(std::size_t triangles, bool rendered, unsigned gl_error);
void record_plugin_texture_source(const std::string& path, bool plugin_resource);
}
