#pragma once
#include <functional>
#include <nlohmann/json.hpp>
namespace Slic3r {
// Main-thread adapter, installed before interpreter initialization and cleared
// after Manager::shutdown. Public Python interfaces remain upstream names.
void set_plugin_app_info_provider(std::function<nlohmann::json()> provider);
} // namespace Slic3r
