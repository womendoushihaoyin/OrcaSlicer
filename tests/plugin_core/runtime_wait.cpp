#include "slic3r/plugin/LocalPluginRuntime.hpp"
#include "slic3r/plugin/PluginManager.hpp"
#include "slic3r/plugin/PythonInterpreter.hpp"
#include "libslic3r/Utils.hpp"
#include <boost/log/core.hpp>
#include <boost/nowide/args.hpp>
#include <boost/nowide/filesystem.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <thread>
#include <wx/app.h>
#include <wx/init.h>

namespace fs = std::filesystem;
using namespace Slic3r;
using json = nlohmann::json;
namespace {
class QueueApp final : public wxAppConsole {
public:
    bool OnInit() override { return true; }
};
void check(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}
void write_json(const fs::path& file, const json& value)
{
    std::ofstream stream(file);
    stream << value.dump(2) << '\n';
    check(bool(stream), "Cannot write runtime wait report");
}
void pump()
{
    auto* app = wxAppConsole::GetInstance();
    check(app && wxIsMainThread(), "Real console queue must be pumped on its main thread");
    for (int count = 0; app->HasPendingEvents() && count < 100; ++count)
        app->ProcessPendingEvents();
    check(!app->HasPendingEvents(), "Pending observer queue did not drain");
}
fs::path fixture(const fs::path& directory, const std::string& name, bool broken)
{
    const auto file = directory / (name + ".py");
    std::ofstream stream(file);
    stream << "# /// script\n# requires-python = \">=3.12\"\n# dependencies = []\n"
              "# [tool.orcaslicer.plugin]\n# name = \"Runtime wait " << name << "\"\n# version = \"1.0.0\"\n# ///\n";
    if (broken)
        stream << "raise RuntimeError('intentional runtime-wait import failure')\n";
    else
        stream << R"PY(import orca
class Probe(orca.script.ScriptPluginCapabilityBase):
    def get_name(self): return "Runtime wait probe"
    def execute(self): return orca.ExecutionResult.success("real Script callback", "runtime-wait-payload")
@orca.plugin
class Package(orca.base):
    def register_capabilities(self): orca.register_capability(Probe)
)PY";
    check(bool(stream), "Cannot write plugin fixture");
    return file;
}
struct Observations {
    std::mutex mutex;
    int failed = 0;
    int successful = 0;
    bool all_on_main = true;
    bool nested_attempted = false;
    bool nested_rejected = false;
    std::string nested_error;
};
void exercise(const fs::path& output, bool console, json& report)
{
    set_data_dir((output / "profile-中文").u8string());
    LocalPluginRuntime runtime;
    std::string error;
    check(runtime.initialize({}, [] { return json::object(); }, error), "Runtime initialization failed: " + error);
    auto& manager = PluginManager::instance();
    check(!runtime.load(output / "not-installed.py", error) && error == "Package must be installed before loading",
          "Uninstalled package was not rejected");
    report["missing_package_rejected"] = true;
    const auto broken = fixture(output, "broken", true);
    const auto good = fixture(output, "good", false);
    PluginDescriptor broken_descriptor, good_descriptor;
    check(manager.install_plugin(broken.u8string(), broken_descriptor, error), "Broken fixture install failed: " + error);
    check(manager.install_plugin(good.u8string(), good_descriptor, error), "Good fixture install failed: " + error);
    const fs::path broken_entry = fs::u8path(broken_descriptor.entry_path);
    const fs::path good_entry = fs::u8path(good_descriptor.entry_path);
    const auto main_thread = std::this_thread::get_id();
    const auto observations = std::make_shared<Observations>();
    manager.subscribe_on_load_callback([observations, main_thread, console, &runtime, good_entry,
                                       broken_key = broken_descriptor.plugin_key, good_key = good_descriptor.plugin_key](const std::string& key) {
        if (key != broken_key && key != good_key) return;
        std::lock_guard<std::mutex> lock(observations->mutex);
        observations->all_on_main &= std::this_thread::get_id() == main_thread;
        if (key == broken_key) {
            ++observations->failed;
            // With no application, production observers really execute inline
            // on the loader worker before its slot is released. The outer load
            // is still active here, so this exercises the actual reentry guard.
            if (!console) {
                observations->nested_attempted = true;
                const bool nested = runtime.load(good_entry, observations->nested_error);
                observations->nested_rejected = !nested && observations->nested_error == "Another local plugin load is in progress";
            }
        } else {
            ++observations->successful;
        }
    });
    check(!runtime.load(broken_entry, error), "Broken plugin unexpectedly loaded");
    check(!error.empty() && error == manager.get_plugin_load_error(broken_descriptor.plugin_key),
          "Runtime did not preserve the actual Manager load error");
    check(!runtime.is_loaded(broken_descriptor.plugin_key), "Failed import left the package loaded");
    report["import_failure_error"] = error;
    check(!manager.is_plugin_load_in_progress(broken_descriptor.plugin_key), "Failed load returned before releasing its worker slot");
    report["import_failure_observed"] = true;
    if (console) {
        check(observations->failed == 0 && wxAppConsole::GetInstance()->HasPendingEvents(),
              "Console Runtime::load incorrectly dispatched GUI observers");
        pump();
        check(observations->failed == 1 && observations->all_on_main, "Failed-load observer did not dispatch on the main thread");
    } else {
        check(observations->failed == 1 && observations->nested_attempted && observations->nested_rejected,
              "No-app inline observer did not reject a genuine nested Runtime::load");
        report["nested_inline_observer_load_rejected"] = true;
    }
    // Holding the GIL is intentional: Runtime::load's native wait must release
    // it while the real loader worker imports and registers Python capabilities.
    {
        pybind11::gil_scoped_acquire gil;
        check(runtime.load(good_entry, error), "Valid load after failure failed: " + error);
    }
    check(error.empty() && runtime.is_loaded(good_descriptor.plugin_key), "Successful load retained a stale error/state");
    check(!manager.is_plugin_load_in_progress(good_descriptor.plugin_key), "Successful load returned before releasing its worker slot");
    report["load_after_failure_with_gil"] = true;
    if (console) {
        check(observations->successful == 0 && wxAppConsole::GetInstance()->HasPendingEvents(),
              "Console Runtime::load pumped its pending observer queue");
        pump();
        check(observations->successful == 1 && observations->all_on_main, "Successful observer did not run on main");
        report["observer_queued_until_main_thread_dispatch"] = true;
    } else {
        check(observations->successful == 1 && !observations->all_on_main, "No-app observers did not retain inline worker semantics");
    }
    const auto execution = runtime.execute(good_descriptor.plugin_key, "Runtime wait probe");
    check(execution.ok && execution.data == "runtime-wait-payload", "Actual Script capability did not execute");
    report["actual_script_callback"] = true;
    check(runtime.unload(good_descriptor.plugin_key, error), "Unload failed: " + error);
    check(runtime.load(good_entry, error), "Load after successful unload failed: " + error);
    check(!manager.is_plugin_load_in_progress(good_descriptor.plugin_key), "Reload left a worker slot active");
    report["unload_then_reload"] = true;
    if (console) {
        check(observations->successful == 1, "Reload observer ran before main queue dispatch");
        // Shutdown before pumping must invalidate the queued final observer.
    } else {
        check(observations->successful == 2, "No-app reload observer did not run");
    }
    runtime.shutdown();
    check(!runtime.initialized() && !PythonInterpreter::instance().is_initialized(), "Runtime shutdown did not stop interpreter");
    if (console) {
        pump();
        check(observations->successful == 1 && observations->all_on_main, "Late observer ran after runtime shutdown");
        report["pending_observer_invalidated_on_shutdown"] = true;
        report["nested_load_during_console_wait"] = "not attempted: no main-thread pump occurs while Runtime::load blocks";
    }
    report["interpreter_stopped"] = true;
}
} // namespace
int main(int argc, char** argv)
{
    boost::nowide::args utf8_args(argc, argv);
    boost::nowide::nowide_filesystem();
    if (argc != 3) return 2;
    const std::string mode = argv[1];
    if (mode != "no-app" && mode != "console") return 2;
    const auto output = fs::u8path(argv[2]);
    if (fs::exists(output)) return 3;
    fs::create_directories(output);
    boost::log::core::get()->set_logging_enabled(false);
    const bool console = mode == "console";
    json report{{"schema", 1}, {"mode", mode}, {"passed", false}, {"gui_tested", false}};
    int result = 0;
    bool wx_started = false;
    try {
        check(wxAppConsole::GetInstance() == nullptr, "Unexpected preexisting wx application");
        if (console) {
            wxAppConsole::SetInstance(new QueueApp);
            check(wxEntryStart(argc, argv), "wxEntryStart failed");
            wx_started = true;
            check(wxAppConsole::GetInstance()->CallOnInit(), "Console app initialization failed");
            check(dynamic_cast<wxApp*>(wxAppConsole::GetInstance()) == nullptr, "Fixture is a GUI wxApp instead of wxAppConsole");
        }
        exercise(output, console, report);
        report["passed"] = true;
    } catch (const std::exception& error) {
        report["error"] = error.what();
        result = 1;
    }
    PluginManager::instance().shutdown();
    PythonInterpreter::instance().shutdown();
    if (wx_started) {
        pump();
        wxAppConsole::GetInstance()->OnExit();
        wxEntryCleanup();
    }
    report["interpreter_stopped_at_exit"] = !PythonInterpreter::instance().is_initialized();
    write_json(output / "results.json", report);
    std::cout << (result == 0 ? "PASS " : "FAIL ") << "runtime_wait " << mode;
    if (report.contains("error")) std::cout << ": " << report["error"].get<std::string>();
    std::cout << '\n';
    return result;
}
