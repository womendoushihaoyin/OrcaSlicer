#include "slic3r/plugin/PluginManager.hpp"
#include "slic3r/plugin/LocalPluginRuntime.hpp"
#include "slic3r/plugin/PythonInterpreter.hpp"
#include "libslic3r/Utils.hpp"
#include <boost/log/core.hpp>
#include <boost/nowide/args.hpp>
#include <boost/nowide/filesystem.hpp>
#include <boost/nowide/fstream.hpp>
#include <filesystem>
#include <future>
#include <iostream>
#include <thread>
#include <wx/app.h>
#include <wx/init.h>

namespace fs = std::filesystem;
using namespace Slic3r;
namespace {
class QueueApp final : public wxAppConsole { public: bool OnInit() override { return true; } };
void check(bool value, const std::string& message) { if (!value) throw std::runtime_error(message); }
void touch(const fs::path& path) { boost::nowide::ofstream out(path.u8string()); out << "ready\n"; }
void wait_file(const fs::path& path) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!fs::exists(path) && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    check(fs::exists(path), "Timed out waiting for " + path.filename().string());
}
void load(const std::string& key) {
    auto& manager = PluginManager::instance(); std::string error;
    manager.load_plugin(key, true);
    check(manager.wait_for_plugin_load(key, std::chrono::seconds(20), error), "Load wait: " + error);
    check(manager.is_plugin_loaded(key), "Load failed: " + manager.get_plugin_load_error(key));
}
void run(const fs::path& output, const std::string& scenario) {
    const auto source = output / "overlap.py";
    boost::nowide::ofstream file(source.u8string());
    file << R"PY(# /// script
# requires-python = ">=3.12"
# dependencies = []
# [tool.orcaslicer.plugin]
# name = "Lifecycle overlap contract"
# version = "1.0.0"
# ///
import json, pathlib, time, orca
CONTROL = pathlib.Path()PY" << nlohmann::json(output.u8string()).dump() << R"PY()
counter = CONTROL / "counter"
GENERATION = int(counter.read_text()) + 1 if counter.exists() else 1
counter.write_text(str(GENERATION))
class Page(orca.pages.PagesPluginCapabilityBase):
    def get_name(self): return "Overlap Page"
    def get_ui(self): return "<p>Lifecycle fixture</p>"
    def on_load(self):
        if not self.save_config(json.dumps({"generation": GENERATION, "stage": "loaded"})):
            raise RuntimeError("Cannot save load generation")
    def on_unload(self):
        if GENERATION != 1: return
        (CONTROL / "entered").write_text("old on_unload entered")
        deadline = time.monotonic() + 30
        while not (CONTROL / "release").exists():
            if time.monotonic() > deadline: raise RuntimeError("Fixture gate timed out")
            time.sleep(0.005)
        self.save_config(json.dumps({"generation": GENERATION, "stage": "unloaded"}))
        (CONTROL / "finished").write_text("old on_unload finished")
@orca.plugin
class Package(orca.base):
    def register_capabilities(self): orca.register_capability(Page)
)PY";
    file.close(); check(bool(file), "Cannot write fixture");
    auto& manager = PluginManager::instance();
    check(manager.initialize(), PythonInterpreter::instance().last_error());
    manager.discover_plugins(); PluginDescriptor descriptor; std::string error;
    check(manager.install_plugin(source.u8string(), descriptor, error), "Install: " + error);
    // Grant only this isolated fixture's control files through the real sidecar.
    PluginInstallState state;
    check(read_install_state(descriptor.plugin_root, state), "Cannot read fixture install state");
    for (const char* name : {"counter", "entered", "finished", "release"}) {
        state.permissions.fs_read.push_back((output / name).u8string());
        state.permissions.fs_readwrite.push_back((output / name).u8string());
    }
    check(write_install_state(descriptor.plugin_root, state), "Cannot grant fixture control-file access");
    manager.rescan_plugins(); const auto key = descriptor.plugin_key;
    load(key);
    bool reentered = false, reentrant_unload_rejected = false, reentrant_load_rejected = false, reentrant_shutdown_rejected = false;
    bool destructor_returned = false;
    std::unique_ptr<LocalPluginRuntime> nested_runtime;
    PluginConfig::Subscription subscription = 0;
    if (scenario == "destructor") {
        nested_runtime = std::make_unique<LocalPluginRuntime>();
        check(nested_runtime->initialize({}, {}, error), "Cannot activate runtime wrapper: " + error);
    }
    if (scenario == "reentrant" || scenario == "destructor") {
        subscription = manager.get_config().subscribe_on_change([&](const auto&) {
            const auto config = manager.get_config().get_config({PluginCapabilityType::Pages, "Overlap Page", key});
            if (!config || config->config.value("stage", "") != "unloaded") return;
            reentered = true;
            if (scenario == "destructor") {
                nested_runtime.reset();
                destructor_returned = true;
                return;
            }
            reentrant_unload_rejected = !manager.unload_plugin(key);
            manager.load_plugin(key, true);
            std::string wait_error;
            reentrant_load_rejected = !manager.is_plugin_loaded(key) &&
                !manager.is_plugin_load_in_progress(key) &&
                !manager.wait_for_plugin_load(key, std::chrono::milliseconds::max(), wait_error);
            try { manager.shutdown(); }
            catch (const std::runtime_error&) { reentrant_shutdown_rejected = true; }
        });
    }
    auto unloading = std::async(std::launch::async, [&] {
        if (scenario == "bulk_overlap" || scenario == "bulk_shutdown") { manager.unload_all_plugins(); return true; }
        return manager.unload_plugin(key);
    });
    // Always release the plugin before waiting on the async operation, including assertions.
    struct Release { fs::path path; ~Release() { touch(path); } } release{output / "release"};
    wait_file(output / "entered");
    check(unloading.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout,
          "Fixture did not suspend real on_unload");
    if (scenario == "overlap" || scenario == "bulk_overlap") {
        manager.load_plugin(key, true);
        std::string wait_error;
        const bool wait_done = manager.wait_for_plugin_load(key, std::chrono::milliseconds(500), wait_error);
        const bool new_loaded_while_old_running = manager.is_plugin_loaded(key);
        std::cout << "OBSERVED reload_while_old_unload=" << new_loaded_while_old_running
                  << " load_wait_done=" << wait_done << std::endl;
        touch(output / "release");
        check(unloading.get(), "Unload failed");
        if (!new_loaded_while_old_running) load(key);
        const auto config = manager.get_config().get_config({PluginCapabilityType::Pages, "Overlap Page", key});
        check(config.has_value(), "Capability configuration disappeared");
        std::cout << "OBSERVED final_config=" << config->config.dump() << std::endl;
        check(!new_loaded_while_old_running, "Same-key reload entered while old Python teardown was still running");
        check(config->config.at("generation") == 2 && config->config.at("stage") == "loaded",
              "Old generation overwrote replacement configuration");
    } else if (scenario == "shutdown" || scenario == "bulk_shutdown") {
        auto stopping = std::async(std::launch::async, [&] { manager.shutdown(); });
        const bool stopped_while_old_running = stopping.wait_for(std::chrono::milliseconds(500)) == std::future_status::ready;
        std::cout << "OBSERVED shutdown_returned_while_old_unload=" << stopped_while_old_running << std::endl;
        touch(output / "release");
        check(unloading.get(), "Unload failed"); stopping.get();
        check(!stopped_while_old_running, "Manager shutdown returned while old Python teardown was still running");
    } else if (scenario == "replacement" || scenario == "delete") {
        PluginDescriptor replacement;
        const bool accepted = scenario == "replacement" ? manager.install_plugin(source.u8string(), replacement, error) :
                                                         manager.delete_plugin(key, error);
        std::cout << "OBSERVED " << scenario << "_while_old_unload=" << accepted << std::endl;
        const bool original_exists = fs::exists(fs::u8path(descriptor.entry_path));
        touch(output / "release"); check(unloading.get(), "Unload failed");
        check(!accepted && original_exists, "Package mutation entered while old Python teardown was running");
        if (scenario == "replacement") {
            check(manager.install_plugin(source.u8string(), replacement, error), "Replacement after teardown: " + error);
            manager.rescan_plugins(); load(key);
            const auto config = manager.get_config().get_config({PluginCapabilityType::Pages, "Overlap Page", key});
            check(config && config->config.at("generation") == 2, "Replacement did not publish its own generation");
        } else {
            check(manager.delete_plugin(key, error), "Delete after teardown: " + error);
            check(!fs::exists(fs::u8path(descriptor.plugin_root)), "Delete left package files behind");
        }
    } else if (scenario == "reentrant" || scenario == "destructor") {
        touch(output / "release"); check(unloading.get(), "Unload failed");
        manager.get_config().unsubscribe_on_change(subscription);
        check(reentered, "Real on_unload did not invoke its configuration observer");
        if (scenario == "reentrant")
            check(reentrant_unload_rejected && reentrant_load_rejected && reentrant_shutdown_rejected,
                  "Same-thread unload/load/shutdown reentry was not safely rejected");
        else
            check(destructor_returned && PythonInterpreter::instance().is_initialized(),
                  "Runtime wrapper destruction finalized Python inside old on_unload");
        // Rejection must leave the outer owner able to perform a complete cleanup.
        manager.shutdown(); PythonInterpreter::instance().shutdown();
        check(!PythonInterpreter::instance().is_initialized(), "Later normal cleanup did not finalize Python");
    } else if (scenario == "shutdown_gil") {
        auto release_when_closing = std::async(std::launch::async, [&] {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
            while (!manager.is_shutting_down() && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            touch(output / "release");
        });
        bool finished_at_shutdown = false;
        {
            PythonGILState held_gil;
            check(bool(held_gil), "Cannot acquire real GIL");
            manager.shutdown();
            finished_at_shutdown = fs::exists(output / "finished");
        }
        release_when_closing.get(); check(unloading.get(), "Unload failed");
        check(finished_at_shutdown, "Shutdown did not drain Python teardown while caller held GIL");
    } else { throw std::runtime_error("Unknown scenario"); }
    check(fs::exists(output / "finished"), "Real on_unload did not finish");
    std::cout << "PASS " << scenario << std::endl;
}
}
int main(int argc, char** argv) {
    boost::nowide::args args(argc, argv); boost::nowide::nowide_filesystem();
    if (argc != 3) return 2;
    const auto scenario = std::string(argv[1]); const auto output = fs::u8path(argv[2]);
    if (fs::exists(output)) return 3;
    fs::create_directories(output); set_data_dir((output / "profile").u8string());
    boost::log::core::get()->set_logging_enabled(false);
    wxAppConsole::SetInstance(new QueueApp); if (!wxEntryStart(argc, argv)) return 4;
    int result = 0;
    try { check(wxAppConsole::GetInstance()->CallOnInit(), "wx init failed"); run(output, scenario); }
    catch (const std::exception& error) { std::cerr << "FAIL " << scenario << ": " << error.what() << std::endl; result = 1; }
    PluginManager::instance().shutdown(); PythonInterpreter::instance().shutdown();
    auto* app = wxAppConsole::GetInstance();
    for (int i = 0; app->HasPendingEvents() && i < 100; ++i) app->ProcessPendingEvents();
    app->OnExit(); wxEntryCleanup(); return result;
}
