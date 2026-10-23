#include "slic3r/plugin/LocalPluginRuntime.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;

void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

int main(int argc, char** argv)
{
    if (argc != 3)
        return 2;
    const auto path = fs::temp_directory_path() /
                      ("orca-plugin-spike-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(path);
    try {
        const std::string scenario = argv[2];
        const fs::path    marker   = path / "unloaded.txt";
        std::string       source   = R"PY(# /// script
# dependencies = []
# [tool.orcaslicer.plugin]
# name = "Native Host Contract"
# version = "1.0.0"
# ///
import orca
class Hello(orca.script.ScriptPluginCapabilityBase):
    def get_name(self): return "Native Host Hello"
    def execute(self):
        info = orca.host.app_info()
        assert info["version"] == "host-test-version"
        return orca.ExecutionResult.success("C++ to Python to host succeeded", info["version"])
    def on_unload(self):
        with open(MARKER, "w", encoding="utf-8") as f: f.write("unloaded")
@orca.plugin
class Package(orca.base):
    def register_capabilities(self): orca.register_capability(Hello)
)PY";
        source                     = "MARKER = " + nlohmann::json(marker.u8string()).dump() + "\n" + source;
        if (scenario == "exception")
            source += "\nHello.execute = lambda self: (_ for _ in ()).throw(RuntimeError('expected exception'))\n";
        if (scenario == "duplicate")
            source += "\nPackage.register_capabilities = lambda self: (orca.register_capability(Hello), orca.register_capability(Hello))\n";
        if (scenario == "dependency") {
            const auto at = source.find("# dependencies = []");
            source.replace(at, std::string("# dependencies = []").size(), "# dependencies = [\"must-not-install-anything\"]");
            source += "\nraise RuntimeError('package body must not execute')\n";
        }
        if (scenario == "on_load_failure")
            source += "\nHello.on_load = lambda self: (_ for _ in ()).throw(RuntimeError('on_load failed'))\n";
        const auto file = path / "hello.py";
        std::ofstream(file) << source;
        Slic3r::LocalPluginRuntime runtime;
        std::string                error;
        require(runtime.initialize(argv[1], [] { return nlohmann::json{{"version", "host-test-version"}}; }, error), error);
        const bool loaded = runtime.load(file, error);
        if (scenario == "duplicate" || scenario == "dependency" || scenario == "on_load_failure") {
            require(!loaded, "invalid package was accepted");
            require(runtime.scripts().empty(), "failed load left registered capabilities");
            if (scenario == "dependency")
                require(error.find("S03") != std::string::npos, "dependency package was executed before rejection: " + error);
            runtime.shutdown();
            require(!runtime.initialized(), "runtime remains initialized");
        } else {
            require(loaded, error);
            require(runtime.scripts().size() == 1, "expected one registered capability");
            const auto info   = runtime.scripts().front();
            const auto result = runtime.execute(info.package_key, info.name);
            if (scenario == "exception") {
                require(!result.ok && result.message.find("expected exception") != std::string::npos,
                        "Python exception was not propagated");
            } else {
                require(result.ok, result.message);
                require(result.data == "host-test-version", "host callback did not execute");
            }
            runtime.shutdown();
            require(fs::exists(marker), "on_unload was not called before interpreter finalization");
            require(!runtime.initialized(), "runtime remains initialized");
            require(!runtime.execute(info.package_key, info.name).ok, "execution after shutdown succeeded");
        }
        fs::remove_all(path);
        std::cout << "PASS " << scenario << '\n';
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << '\n';
        fs::remove_all(path);
        return 1;
    }
}
