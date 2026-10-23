#pragma once
#include <filesystem>
#include <nlohmann/json.hpp>
namespace Slic3r::GUI {
nlohmann::json run_plugin_legacy_states_smoke(const std::filesystem::path& fixture, const std::filesystem::path& report);
}
