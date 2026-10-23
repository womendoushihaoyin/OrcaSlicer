#include "slic3r/plugin/PluginManager.hpp"
#include "slic3r/plugin/PythonInterpreter.hpp"
#include "slic3r/plugin/host/PluginResourceTextures.hpp"
#include "libslic3r/Utils.hpp"
#include <boost/log/core.hpp>
#include <boost/nowide/args.hpp>
#include <boost/nowide/filesystem.hpp>
#include <boost/nowide/fstream.hpp>
#include <filesystem>
#include <future>
#include <iostream>
#include <memory>
#include <wx/app.h>
#include <wx/init.h>

namespace fs = std::filesystem;
using namespace Slic3r;
namespace {
class QueueApp final : public wxAppConsole
{
public:
    bool OnInit() override { return true; }
};
void check(bool ok, const std::string& message)
{
    if (!ok)
        throw std::runtime_error(message);
}
void pump()
{
    auto* app = wxAppConsole::GetInstance();
    for (int i = 0; app->HasPendingEvents() && i < 100; ++i)
        app->ProcessPendingEvents();
    check(!app->HasPendingEvents(), "Observer queue failed to drain");
}
void load(const std::string& key)
{
    auto& manager = PluginManager::instance();
    std::string error;
    manager.load_plugin(key, true);
    check(manager.wait_for_plugin_load(key, std::chrono::seconds(20), error), "Load did not finish: " + error);
    check(manager.is_plugin_loaded(key), "Plugin is not loaded: " + manager.get_plugin_load_error(key));
}
void state(const std::string& key, bool enabled, bool texture_enabled = true)
{
    PluginInstallState current;
    check(PluginManager::instance().get_install_state(key, current), "Cannot read install state");
    check(current.enabled == enabled, "Package enabled state was not committed before completion");
    check(current.capabilities == std::vector<std::pair<std::string, bool>>{{"Queue Texture", texture_enabled}},
          "Capability state was not committed before completion");
}
struct Counts { int old_load = 0, load = 0, unload = 0, enable = 0, disable = 0; };
void run(const fs::path& output)
{
    const auto source = output / "queue_plugin.py";
    {
        boost::nowide::ofstream file(source.u8string());
        file << R"PY(# /// script
# requires-python = ">=3.12"
# dependencies = []
# [tool.orcaslicer.plugin]
# name = "Observer Queue Contract"
# version = "1.0.0"
# ///
import orca, pathlib
class Texture(orca.resource_texture.ResourceTexturePluginCapabilityBase):
    def get_name(self): return "Queue Texture"
    def get_texture_dir(self): return str(pathlib.Path(__file__).parent)
@orca.plugin
class Package(orca.base):
    def register_capabilities(self): orca.register_capability(Texture)
)PY";
        check(bool(file), "Cannot write plugin fixture");
    }
    auto& manager = PluginManager::instance();
    auto& textures = PluginResourceTextures::instance();
    const auto counts = std::make_shared<Counts>();
    check(manager.initialize(), PythonInterpreter::instance().last_error());
    manager.discover_plugins();
    PluginDescriptor descriptor;
    std::string error;
    check(manager.install_plugin(source.u8string(), descriptor, error), "Cannot install fixture: " + error);
    manager.rescan_plugins();
    const auto key = descriptor.plugin_key;
    manager.subscribe_on_load_callback([counts](const auto&) { ++counts->old_load; });
    load(key);
    check(counts->old_load == 0 && wxAppConsole::GetInstance()->HasPendingEvents(), "Worker did not queue its real wx event");
    check(textures.entries().empty(), "Consumer unexpectedly ran without pumping the wx queue");
    state(key, true);
    manager.shutdown();
    state(key, true);
    check(textures.entries().empty(), "Shutdown retained texture resources");
    std::cout << "PASS sidecar durable before pending load observer / immediate shutdown\n";

    check(manager.initialize(), "Cannot reinitialize manager");
    manager.subscribe_on_load_callback([counts](const auto&) { ++counts->load; });
    manager.subscribe_on_unload_callback([counts](const auto&) { ++counts->unload; });
    manager.subscribe_on_capability_load_callback([counts](const auto&) { ++counts->enable; });
    manager.subscribe_on_capability_unload_callback([counts](const auto&) { ++counts->disable; });
    load(key);
    pump();
    check(counts->old_load == 0 && counts->load == 1, "Previous runtime observer reached the new runtime");
    check(textures.entries().size() == 1, "Current runtime texture consumer did not register");
    state(key, true);
    std::cout << "PASS shutdown / reinitialize invalidates previous runtime queue\n";

    const auto generation = textures.generation();
    check(std::async(std::launch::async, [&] { return manager.unload_plugin(key); }).get(), "Worker unload failed");
    state(key, false);
    check(counts->unload == 0, "Worker unload observer ran before pumping");
    load(key);
    pump();
    check(counts->unload == 0 && counts->load == 2, "Old unload observer reached the replacement instance");
    check(textures.entries().size() == 1 && textures.generation() == generation + 1,
          "Old unload mutated the new texture registry generation");
    state(key, true);
    std::cout << "PASS queued worker unload / reload keeps new registry and sidecar\n";

    const PluginCapabilityId id{PluginCapabilityType::ResourceTexture, "Queue Texture", key};
    std::async(std::launch::async, [&] { manager.set_capability_enabled(id, false); }).get();
    state(key, true, false);
    check(counts->disable == 0, "Worker capability observer ran before pumping");
    manager.set_capability_enabled(id, true);
    pump();
    check(counts->disable == 0 && counts->enable == 1 && textures.entries().size() == 1,
          "Old disable observer removed the re-enabled texture capability");
    state(key, true);
    std::cout << "PASS disable / re-enable isolates pending capability observer\n";

    check(manager.unload_plugin(key), "Final unload failed");
    load(key);
    check(counts->load == 2, "Final load observer unexpectedly ran synchronously");
    manager.shutdown();
    PythonInterpreter::instance().shutdown();
    const auto stopped_generation = textures.generation();
    pump();
    check(counts->old_load == 0 && counts->load == 2 && textures.entries().empty() &&
          textures.generation() == stopped_generation, "Pending observer rebuilt resources after interpreter shutdown");
    state(key, true);
    std::cout << "PASS pending load / interpreter shutdown / real wx event drain\n";
}
} // namespace
int main(int argc, char** argv)
{
    boost::nowide::args utf8_args(argc, argv);
    boost::nowide::nowide_filesystem();
    if (argc != 2)
        return 2;
    const auto output = fs::u8path(argv[1]);
    if (fs::exists(output))
        return 3;
    fs::create_directories(output);
    set_data_dir((output / "profile").u8string());
    boost::log::core::get()->set_logging_enabled(false);
    // A real base wx application owns the pending-event queue. The manager uses
    // wxAppConsole directly and never casts this object to GUI_App or wxApp.
    wxAppConsole::SetInstance(new QueueApp);
    if (!wxEntryStart(argc, argv))
        return 4;
    int result = 0;
    try {
        check(wxAppConsole::GetInstance()->CallOnInit(), "wxAppConsole initialization failed");
        run(output);
    } catch (const std::exception& error) {
        std::cerr << "FAIL observer_queue: " << error.what() << '\n';
        result = 1;
    }
    PluginManager::instance().shutdown();
    PythonInterpreter::instance().shutdown();
    pump();
    wxAppConsole::GetInstance()->OnExit();
    wxEntryCleanup();
    return result;
}
