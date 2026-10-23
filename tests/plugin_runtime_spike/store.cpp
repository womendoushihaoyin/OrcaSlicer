#include "slic3r/plugin/LocalPluginRuntime.hpp"
#include "slic3r/plugin/LocalPluginStore.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;
using json   = nlohmann::json;

void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

std::string contents(const fs::path& file)
{
    std::ifstream input(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void write(const fs::path& file, const std::string& value)
{
    std::ofstream output(file);
    output << value;
    output.close();
    require(bool(output), "Could not write fixture");
}

std::string fixture(const fs::path& root)
{
    return "MARKER = " + json((root / "executed.txt").u8string()).dump() +
           "\nUNLOADED = " + json((root / "unloaded.txt").u8string()).dump() + R"PY(
# /// script
# dependencies = []
# [tool.orcaslicer.plugin]
# name = "Persistent Script"
# version = "1.2.3"
# ///
import orca
with open(MARKER, "a", encoding="utf-8") as f: f.write("imported\n")
class Script(orca.script.ScriptPluginCapabilityBase):
    def get_name(self): return "Persistent Script"
    def execute(self): return orca.ExecutionResult.success("executed", "persistent")
    def on_unload(self):
        with open(UNLOADED, "a", encoding="utf-8") as f: f.write("unloaded\n")
@orca.plugin
class Package(orca.base):
    def register_capabilities(self): orca.register_capability(Script)
)PY";
}

int main(int argc, char** argv)
{
    if (argc != 3 && argc != 4)
        return 2;
    const bool     owned = argc == 3;
    const fs::path root  = owned ? fs::temp_directory_path() /
                                      ("orca-store-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())) :
                                   fs::u8path(argv[3]);
    fs::create_directories(root);
    try {
        const std::string          scenario = argv[2];
        Slic3r::LocalPluginRuntime runtime;
        std::string                error;
        require(runtime.initialize(argv[1], [] { return json::object(); }, error), error);
        Slic3r::LocalPluginStore store(root / "orca_plugins", [&](const auto& file, auto& descriptor, auto& why) {
            return runtime.inspect(file, descriptor, why);
        });
        const auto               file           = root / "hello world.py";
        const auto               installed_file = root / "orca_plugins/hello_world.py/hello world.py";
        const auto               state_path     = installed_file.parent_path() / ".install_state.json";
        Slic3r::PluginDescriptor installed;
        if (scenario == "install" || owned) {
            write(file, fixture(root));
            require(store.install(file, installed, error), error);
            require(installed.plugin_key == "hello_world", "identity differs from upstream escaped stem");
            require(fs::exists(installed_file), "installation layout differs from upstream filename directory");
            require(!fs::exists(root / "executed.txt"), "metadata inspection executed package body");
            const auto state = json::parse(contents(state_path));
            require(state.at("installed_from") == "local" && state.at("enabled") == true && state.at("installed_version") == "1.2.3",
                    "invalid install state");
            require(state.at("permissions").size() == 7 && state.at("capabilities").is_array(), "missing Orca install-state fields");
        }
        if (scenario == "restart_enabled" || scenario == "restart_disabled") {
            const auto entries = store.scan(error);
            require(error.empty() && entries.size() == 1 && !entries.front().has_error(), error);
            require(entries.front().enabled == (scenario == "restart_enabled"), "enabled state did not survive process restart");
            require(entries.front().version == "1.2.3", "version did not survive process restart");
            const auto before = contents(root / "executed.txt");
            for (const auto& entry : entries)
                if (entry.enabled)
                    require(runtime.load(fs::u8path(entry.entry_path), error), error);
            if (scenario == "restart_enabled") {
                require(runtime.scripts().size() == 1, "installed package not registered after restart");
                require(runtime.execute("hello_world", "Persistent Script").ok, "installed Script did not execute");
                require(contents(root / "executed.txt") == before + "imported\n", "module not imported exactly once");
            } else {
                require(runtime.scripts().empty() && contents(root / "executed.txt") == before, "disabled package executed");
            }
        } else if (scenario == "disable") {
            require(store.set_enabled("hello_world", false, error), error);
        } else if (scenario == "transaction") {
            const auto original = contents(installed_file);
            require(!store.install(file, installed, error), "duplicate installation accepted");
            require(contents(installed_file) == original, "duplicate installation changed original");
            write(root / "broken.py", "raise RuntimeError('must not execute')\n");
            require(!store.install(root / "broken.py", installed, error), "invalid metadata accepted");
            auto dependent = fixture(root);
            dependent.replace(dependent.find("dependencies = []"), std::string("dependencies = []").size(),
                              "dependencies = [\"no-install\"]");
            write(root / "dependent.py", dependent);
            require(!store.install(root / "dependent.py", installed, error), "unsupported dependencies accepted");
            require(!fs::exists(root / "executed.txt"), "rejected package executed");
            require(store.scan(error).size() == 1 && error.empty(), "rejection published partial packages");
            for (const auto& entry : fs::directory_iterator(root / "orca_plugins"))
                require(entry.path().filename().u8string().front() != '.', "staging or lock leaked");
        } else if (scenario == "state") {
            auto state                      = json::parse(contents(state_path));
            state["permissions"]["fs_read"] = {"saved-grant"};
            state["extension_field"]        = "preserve";
            write(state_path, state.dump());
            require(store.set_enabled("hello_world", false, error), error);
            auto entries = store.scan(error);
            require(error.empty() && entries.size() == 1 && !entries.front().enabled, "disable not persisted");
            auto changed = json::parse(contents(state_path));
            require(changed["permissions"] == state["permissions"] && changed["extension_field"] == "preserve", "toggle lost state fields");
            require(store.set_enabled("hello_world", true, error), error);
            require(store.scan(error).front().enabled, "enable not persisted");
            write(state_path, "{broken");
            entries = store.scan(error);
            require(!entries.front().enabled && entries.front().has_error(), "broken sidecar auto-enabled");
            fs::remove(state_path);
            entries = store.scan(error);
            require(!entries.front().enabled && !entries.front().install_state_valid, "missing sidecar auto-enabled");
            require(!fs::exists(root / "executed.txt"), "scan executed package");
        } else if (scenario == "boundaries") {
            const auto outside = root / "outside";
            fs::create_directory(outside);
            write(outside / "untouched", "keep");
            require(!store.remove("../outside", error), "traversal key accepted");
            fs::create_directory_symlink(outside, root / "orca_plugins/link.py");
            auto entries = store.scan(error);
            require(entries.size() == 2 && entries.back().has_error() && !entries.back().enabled, "symlink directory accepted");
            require(!store.remove("link", error), "symlink package removed");
            fs::remove(root / "orca_plugins/link.py");
            fs::remove(installed_file);
            fs::create_symlink(file, installed_file);
            require(store.scan(error).front().has_error(), "symlink entry accepted");
            fs::remove(installed_file);
            fs::copy_file(file, installed_file);
            fs::copy_file(file, installed_file.parent_path() / "second.py");
            require(store.scan(error).front().has_error(), "ambiguous package accepted");
            fs::remove(installed_file.parent_path() / "second.py");
            fs::create_directory(root / "orca_plugins/plugin_data");
            write(root / "orca_plugins/plugin_data/keep", "keep");
            require(store.remove("hello_world", error), error);
            require(contents(root / "orca_plugins/plugin_data/keep") == "keep" && contents(outside / "untouched") == "keep",
                    "uninstall deleted external or plugin data");
            require(store.scan(error).empty(), "uninstall left catalog row");
        } else if (scenario == "lifecycle") {
            require(runtime.load(installed_file, error), error);
            require(runtime.is_loaded("hello_world"), "package missing after load");
            require(runtime.unload("hello_world", error), error);
            require(!runtime.is_loaded("hello_world") && runtime.scripts().empty(), "unload retained capabilities");
            require(contents(root / "unloaded.txt") == "unloaded\n", "unload callback not invoked");
            require(runtime.load(installed_file, error), "reload failed: " + error);
            require(runtime.scripts().size() == 1, "reload duplicated capabilities");
            runtime.shutdown();
            require(contents(root / "unloaded.txt") == "unloaded\nunloaded\n", "shutdown did not unload reloaded package");
        }
        runtime.shutdown();
        if (owned)
            fs::remove_all(root);
        std::cout << "PASS " << scenario << '\n';
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << '\n';
        if (owned)
            fs::remove_all(root);
        return 1;
    }
}
