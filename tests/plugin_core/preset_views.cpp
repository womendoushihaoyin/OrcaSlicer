#include "slic3r/plugin/PluginManager.hpp"
#include "slic3r/plugin/PythonInterpreter.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Utils.hpp"
#include <boost/log/core.hpp>
#include <boost/nowide/args.hpp>
#include <boost/nowide/filesystem.hpp>
#include <boost/nowide/fstream.hpp>
#include <filesystem>
#include <future>
#include <iostream>
#include <pybind11/embed.h>
#include <wx/app.h>
#include <wx/init.h>
#include <wx/thread.h>

namespace test_fs = std::filesystem;
namespace py = pybind11;
using namespace Slic3r;
namespace {
class PresetViewApp final : public wxAppConsole
{
public:
    bool OnInit() override { return true; }
};
void check(bool value, const std::string& message)
{
    if (!value)
        throw std::runtime_error(message);
}
void write(const test_fs::path& path, const nlohmann::json& value)
{
    test_fs::create_directories(path.parent_path());
    boost::nowide::ofstream file(path.u8string());
    file << value.dump(2);
    check(bool(file), "Cannot write preset fixture");
}
DynamicPrintConfig config(double height)
{
    DynamicPrintConfig result;
    result.set_key_value("layer_height", new ConfigOptionFloat(height));
    return result;
}
void run(const test_fs::path& output)
{
    auto& manager = PluginManager::instance();
    check(manager.initialize(), PythonInterpreter::instance().last_error());
    PresetBundle bundle;
    bundle.filament_presets = {bundle.filaments.get_selected_preset_name()};
    bundle.prints.load_preset((output / "A.json").u8string(), "A", config(0.12), false);
    bundle.prints.load_preset((output / "Z.json").u8string(), "Z", config(0.22), false);
    bundle.prints.select_preset_by_name("A", true);
    {
        PythonGILState gil;
        py::module_::import("orca");
        py::dict scope;
        scope["bundle"] = py::cast(&bundle, py::return_value_policy::reference);
        py::exec(R"PY(
import orca, weakref, gc
selected = bundle.prints.selected_preset()
edited = bundle.current_process_preset()
z = bundle.prints.find_preset("Z")
assert selected is bundle.prints.get_selected_preset()
assert edited is bundle.current_print_preset() is bundle.prints.get_edited_preset()
assert isinstance(z, orca.host.Preset) and z.name == "Z"
assert z is bundle.prints.find_preset("Z")
assert bundle.current_filament_presets()[0] is bundle.filaments.get_edited_preset()
def invalid(view):
    for access in (lambda: view.name, lambda: view.type, view.is_user, view.config_keys, lambda: view.config_value("layer_height")):
        try: access()
        except RuntimeError as error: assert "no longer valid" in str(error)
        else: raise AssertionError("Stale preset handle remained readable")
)PY", scope);
        bundle.prints.get_edited_preset().config.set_key_value("layer_height", new ConfigOptionFloat(0.18));
        bundle.prints.find_preset("A", false, true)->config.set_key_value("layer_height", new ConfigOptionFloat(0.14));
        bundle.prints.load_preset((output / "B.json").u8string(), "B", config(0.32), false);
        py::exec(R"PY(
assert float(edited.config_value("layer_height")) == 0.18
assert float(selected.config_value("layer_height")) == 0.14
assert z.name == "Z" and float(z.config_value("layer_height")) == 0.22
assert z is bundle.prints.find_preset("Z")
temporary = bundle.prints.find_preset("B")
weak = weakref.ref(temporary)
del temporary
gc.collect()
assert weak() is None, "Preset cache must not keep a view alive"
)PY", scope);
        check(bundle.prints.delete_preset("Z"), "Actual delete_preset API failed");
        py::exec("invalid(z)", scope);
        bundle.prints.load_preset((output / "replacement-Z.json").u8string(), "Z", config(0.42), false);
        bundle.prints.load_preset((output / "replacement-A.json").u8string(), "A", config(0.44), false);
        py::exec(R"PY(
invalid(z)
invalid(selected)
invalid(edited)
replacement = bundle.prints.find_preset("Z")
assert replacement is not z and float(replacement.config_value("layer_height")) == 0.42
)PY", scope);
        std::cout << "PASS live config / sorted insertion / weak alias cache / delete and source replacement\n";

        bundle.prints.select_preset(0);
        const auto local = output / "profile/user/default/_local/view-test";
        write(local / "bundle_metadata.json", {{"id", "view-test"}, {"name", "View test"}});
        auto process = [](const std::string& name, double height) {
            return nlohmann::json{{"type", "process"}, {"name", name}, {"version", "2.4.1.0"},
                                  {"layer_height", std::to_string(height)}};
        };
        write(local / "process/B.json", process("B", 0.16));
        write(local / "process/Z.json", process("Z", 0.26));
        std::string error;
        check(bundle.reload_local_bundle("default", "view-test", &error), error);
        py::exec(R"PY(
bundled = bundle.prints.find_preset("_local/view-test/B")
removed = bundle.prints.find_preset("_local/view-test/Z")
assert bundled.is_from_bundle() and bundled.bundle_id == "view-test"
)PY", scope);
        write(local / "process/A.json", process("A", 0.11));
        write(local / "process/B.json", process("B", 0.19));
        test_fs::remove(local / "process/Z.json");
        check(bundle.reload_local_bundle("default", "view-test", &error), error);
        py::exec(R"PY(
assert bundled.name == "_local/view-test/B" and float(bundled.config_value("layer_height")) == 0.19
assert bundled is bundle.prints.find_preset("_local/view-test/B")
invalid(removed)
)PY", scope);
        std::cout << "PASS actual local bundle layout reload keeps identity and rejects removed profile\n";

        // Background access is rejected before touching the collection. Release
        // the main GIL while the worker acquires it for the real property call.
        auto view = scope["bundled"].cast<py::object>();
        {
            py::gil_scoped_release release;
            check(std::async(std::launch::async, [&] {
                check(!wxIsMainThread(), "wx thread identity was not initialized by the test application");
                PythonGILState worker_gil;
                try { py::object value = view.attr("name"); }
                catch (const py::error_already_set& error) { return std::string(error.what()).find("UI thread") != std::string::npos; }
                return false;
            }).get(), "Background preset read was not rejected");
        }
        manager.shutdown();
        py::exec(R"PY(
try: bundled.name
except RuntimeError as error: assert "closing" in str(error)
else: raise AssertionError("Preset accessed after shutdown")
)PY", scope);
        std::cout << "PASS worker thread and shutdown guard\n";
    }
    PythonInterpreter::instance().shutdown();
}
} // namespace
int main(int argc, char** argv)
{
    boost::nowide::args utf8_args(argc, argv);
    boost::nowide::nowide_filesystem();
    if (argc != 2)
        return 2;
    const auto output = test_fs::u8path(argv[1]);
    if (test_fs::exists(output))
        return 3;
    test_fs::create_directories(output / "profile");
    set_data_dir((output / "profile").u8string());
    boost::log::core::get()->set_logging_enabled(false);
    // wxThread::IsMain() deliberately returns true on every thread before wx
    // startup. Initialize the real console app so this test uses production
    // thread identity instead of silently accepting a worker as the main thread.
    wxAppConsole::SetInstance(new PresetViewApp);
    if (!wxEntryStart(argc, argv))
        return 4;
    int result = 0;
    try {
        check(wxAppConsole::GetInstance()->CallOnInit(), "wxAppConsole initialization failed");
        run(output);
    }
    catch (const std::exception& error) {
        std::cerr << "FAIL preset_views: " << error.what() << '\n';
        result = 1;
    }
    PluginManager::instance().shutdown();
    PythonInterpreter::instance().shutdown();
    wxAppConsole::GetInstance()->ProcessPendingEvents();
    wxAppConsole::GetInstance()->OnExit();
    wxEntryCleanup();
    return result;
}
