#include "libslic3r/GCode/PostProcessor.hpp"
#include "libslic3r/GCode/PluginPostProcess.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/Print.hpp"
#include "slic3r/plugin/PluginManager.hpp"
#include "slic3r/plugin/PluginSlicing.hpp"
#include "slic3r/plugin/PythonInterpreter.hpp"
#include <filesystem>
#include <fstream>
#include <future>

using namespace Slic3r;
namespace test_fs = std::filesystem;
namespace {
void require(bool value, const std::string& message) { if (!value) throw std::runtime_error(message); }
std::string read(const test_fs::path& path)
{
    std::ifstream input(path);
    require(bool(input), "cannot read " + path.u8string());
    return {(std::istreambuf_iterator<char>(input)), {}};
}
void clean(const test_fs::path& root)
{
    for (const auto& entry : test_fs::directory_iterator(root))
        require(entry.path().filename() == "source.gcode", "leaked working file: " + entry.path().u8string());
}
}
void test_gcode_plugin(const std::string& scenario, const test_fs::path& fixtures)
{
    auto& manager = PluginManager::instance();
    PluginDescriptor descriptor;
    std::string error;
    require(manager.install_plugin((fixtures / "gcode.py").u8string(), descriptor, error), error);
    manager.rescan_plugins(); manager.load_plugin(descriptor.plugin_key, true);
    require(manager.wait_for_plugin_load(descriptor.plugin_key, std::chrono::seconds(20), error), error);
    const PluginCapabilityId first{PluginCapabilityType::SlicingPipeline, "G-code first", descriptor.plugin_key};
    const PluginCapabilityId second{PluginCapabilityType::SlicingPipeline, "G-code second", descriptor.plugin_key};
    const auto storage = test_fs::u8path(manager.get_storage_dir(descriptor.plugin_key));
    // Outside the plugin storage: exercises the scoped G-code write grant.
    const auto root = storage.parent_path() / "export-中文";
    test_fs::create_directories(root);
    const auto source = root / "source.gcode";
    const std::string original = "; base\nG1 X10 Y20\n";
    std::ofstream(source) << original;
    DynamicPrintConfig config;
    config.set_key_value("wall_loops", new ConfigOptionInt(3));
    config.set_key_value("slicing_pipeline_plugin", new ConfigOptionStrings({first.name, second.name}));
    config.set_key_value("plugins", new ConfigOptionStrings({descriptor.plugin_key + ";;" + first.name, descriptor.plugin_key + ";;" + second.name}));
    auto set_mode = [&](const std::string& mode) {
        require(manager.get_config().store_capability_config(first, {{"mode", mode}, {"value", 1}}), "store G-code config");
    };
    auto run = [&](bool copy, const std::string& host = "File") {
        auto path = source.u8string();
        auto name = host == "File" ? (root / "final.gcode").u8string() : "jobs/final.gcode";
        require(run_post_process_scripts(path, copy, host, name, config), "plugin-only job was skipped");
        return std::make_pair(path, name);
    };
    auto fail = [&](bool copy, const std::string& fragment) {
        auto path = source.u8string(); auto name = (root / "final.gcode").u8string();
        const auto old_name = name;
        bool failed = false;
        try { run_post_process_scripts(path, copy, "File", name, config); }
        catch (const std::exception& ex) { failed = true; require(std::string(ex.what()).find(fragment) != std::string::npos, ex.what()); }
        require(failed, "G-code failure was silently skipped: " + fragment);
        require(path == source.u8string() && name == old_name && read(source) == original, "failed export modified its input/name");
        clean(root);
    };
    if (scenario == "gcode_plugin_export" || scenario == "gcode_plugin_events") {
        Model model;
        auto* object = model.add_object();
        object->name = "gcode-contract-cube";
        object->add_volume(TriangleMesh(make_cube(10., 20., 1.)));
        object->add_instance(); object->ensure_on_bed();
        DynamicPrintConfig full;
        const auto& defaults = FullPrintConfig::defaults();
        full.apply(static_cast<const PrintConfig&>(defaults), true);
        full.apply(static_cast<const PrintObjectConfig&>(defaults), true);
        full.apply(static_cast<const PrintRegionConfig&>(defaults), true);
        full.apply(config);
        Print print;
        print.set_status_silent(); print.set_cancel_callback([] {});
        print.apply(model, full); print.process();
        GCodeProcessorResult processor;
        const auto exported = print.export_gcode(source.u8string(), &processor, nullptr);
        require(exported == source.u8string(), "unexpected native G-code output path");
        const auto native = read(source);
        require(native.find("G1 ") != std::string::npos && !processor.moves.empty(), "native exporter produced no toolpaths");
        auto events = [&] { return nlohmann::json::parse(read(storage / "events.json")); };
        const auto observed = events();
        require(observed.size() == 4, "slice/export lifecycle count");
        const std::vector<std::string> names{"SliceStarted", "SliceGeometryFinished", "GCodeExportStarted", "GCodeExportFinished"};
        for (size_t i = 0; i < names.size(); ++i) {
            require(observed[i].at("event") == names[i] && observed[i].at("code") == "Ok", "slice/export lifecycle order/status");
            require(observed[i].at("id") == std::to_string(print.model().id().id), "lifecycle model identity");
        }
        print.export_gcode(source.u8string(), &processor, nullptr);
        require(events() == observed, "cached native export duplicated lifecycle events");
        for (int i = 0; i < 2; ++i) {
            auto path = exported; auto name = (root / "final.gcode").u8string();
            require(run_post_process_scripts(path, true, "File", name, print.full_print_config()), "native export skipped plugins");
            require(read(test_fs::u8path(path)) == native + "; G-code first:1\n; G-code second:1\n", "real exported toolpath was not processed exactly once");
            require(read(source) == native, "post-processing changed native preview file");
            test_fs::remove(test_fs::u8path(path));
        }
        require(events() == observed, "post-process incorrectly emitted raw G-code lifecycle");
        if (scenario == "gcode_plugin_events") {
            manager.get_config().store_capability_config(first, {{"event_ui", true}, {"event_raise", true}});
            // Observer exceptions are logged without aborting the actual slice.
            print.process();
            require(test_fs::exists(storage / "event_ui_rejected"), "native background lifecycle allowed synchronous UI");
            const auto before_failure = events().size();
            bool failed = false;
            try { print.export_gcode((root / "missing" / "nested" / "bad.gcode").u8string(), &processor, nullptr); }
            catch (const std::exception&) { failed = true; }
            const auto after_failure = events();
            require(failed && after_failure.size() == before_failure + 2 && after_failure.back().at("code") == "Error",
                    "export open failure did not emit one terminal error");
            require(after_failure[before_failure].at("event") == "GCodeExportStarted" &&
                    after_failure.back().at("event") == "GCodeExportFinished", "failed export event ordering");
            manager.get_config().store_capability_config(first, nlohmann::json::object());
            GCodeProcessorResult reloaded;
            print.export_gcode_from_previous_file(source.u8string(), &reloaded, nullptr);
            require(!reloaded.moves.empty() && events().back().at("code") == "Ok", "G-code reload lifecycle");
            const auto before_cancel = events();
            print.cancel();
            bool canceled = false;
            try { print.process(); } catch (const CanceledException&) { canceled = true; }
            require(canceled && events() == before_cancel, "cancelled worker dispatched an observer");
            print.restart();
        }
        clean(root);
        return;
    }
    if (scenario == "gcode_plugin_event_pin") {
        manager.get_config().store_capability_config(first, {{"event_wait", true}});
        auto worker = std::async(std::launch::async, [] {
            LifecycleEventContext context;
            context.cancellation_check = [] { return false; };
            fire_lifecycle_event(LifecycleEvent::SliceStarted, context);
        });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while (!test_fs::exists(storage / "entered") && std::chrono::steady_clock::now() < deadline &&
               worker.wait_for(std::chrono::milliseconds(5)) != std::future_status::ready) {}
        const bool entered = test_fs::exists(storage / "entered");
        const bool unloaded = manager.unload_plugin(first.plugin_key);
        const bool disabled = manager.set_capability_enabled(first, false);
        const bool deleted = manager.delete_plugin(first.plugin_key, error);
        std::ofstream(storage / "release") << "continue";
        worker.get();
        require(entered && !unloaded && !disabled && !deleted, "lifecycle observer did not pin package");
        require(manager.unload_plugin(first.plugin_key), "lifecycle observer leaked pin");
        return;
    }
    if (scenario == "gcode_plugin_errors") {
        for (const auto& mode : {"fatal", "recoverable", "raise", "delete", "ui"}) {
            set_mode(mode);
            const std::string fragment = std::string(mode) == "delete" ? "deleted" : std::string(mode) == "ui" ? "Blocking UI calls" : "G-code contract failure";
            fail(true, fragment); fail(false, fragment);
        }
        set_mode("skip");
        auto result = run(true);
        require(read(test_fs::u8path(result.first)) == original + "; G-code second:1\n", "Skipped status prevented next capability");
        test_fs::remove(test_fs::u8path(result.first));
        const auto valid = config;
        config.set_key_value("slicing_pipeline_plugin", new ConfigOptionStrings({first.name, "missing"}));
        config.set_key_value("plugins", new ConfigOptionStrings({descriptor.plugin_key + ";;" + first.name, "absent;;missing"}));
        test_fs::remove(storage / "G-code first.json");
        fail(true, "missing, unloaded or disabled");
        require(!test_fs::exists(storage / "G-code first.json"), "selection preflight ran first plugin");
        config = valid;
        set_gcode_plugin_hook_factory(nullptr);
        fail(false, "runtime is unavailable");
        install_slicing_plugin_dispatcher();
        require(manager.get_plugin_capability(first)->ref_count() == 0, "failed export leaked capability pin");
        return;
    }
    if (scenario == "gcode_plugin_pin") {
        manager.get_config().store_capability_config(first, {{"wait", true}, {"value", 1}});
        auto worker = std::async(std::launch::async, [&] { return run(false); });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while (!test_fs::exists(storage / "entered") && std::chrono::steady_clock::now() < deadline &&
               worker.wait_for(std::chrono::milliseconds(5)) != std::future_status::ready) {}
        const bool entered = test_fs::exists(storage / "entered");
        const bool unloaded = manager.unload_plugin(first.plugin_key);
        const bool disabled = manager.set_capability_enabled(first, false);
        const bool deleted = manager.delete_plugin(first.plugin_key, error);
        manager.get_config().store_capability_config(second, {{"value", 9}});
        auto detach = std::async(std::launch::async, [] { set_gcode_plugin_hook_factory(nullptr); });
        const bool waited = detach.wait_for(std::chrono::milliseconds(100)) != std::future_status::ready;
        std::ofstream(storage / "release") << "continue";
        worker.get(); detach.get();
        require(entered && !unloaded && !disabled && !deleted && waited, "export did not pin/drain consumers");
        require(read(source) == original + "; G-code first:1\n; G-code second:1\n", "export did not freeze all configs");
        require(manager.unload_plugin(first.plugin_key), "export leaked consumer pin");
        install_slicing_plugin_dispatcher(); clean(root);
        return;
    }
    set_mode("");
    const std::string expected = original + "; G-code first:1\n; G-code second:1\n";
    for (int i = 0; i < 2; ++i) {
        auto result = run(true);
        require(result.first != source.u8string() && read(source) == original, "copy mode changed preview input");
        require(read(test_fs::u8path(result.first)) == expected, "plugin order/repeated export mismatch");
        test_fs::remove(test_fs::u8path(result.first)); clean(root);
    }
    // The upload path requires source_path to remain unchanged with make_copy=false.
    auto uploaded = run(false, "OctoPrint");
    require(uploaded.first == source.u8string() && uploaded.second == "jobs/final.gcode" && read(source) == expected, "upload contract");
    std::ofstream(source) << original;
    auto alias = source.u8string();
    require(run_post_process_scripts(alias, false, "File", alias, config) && alias == source.u8string() && read(source) == expected,
            "BBL source/output alias contract");
    std::ofstream(source) << original;
    const auto python = PythonInterpreter::bundled_python_executable();
    require(!python.empty(), "missing Python executable for classic-script integration");
    config.set_key_value("post_process", new ConfigOptionStrings({"\"" + python + "\" \"" + (fixtures / "classic_postprocess.py").u8string() + "\""}));
    auto result = run(true);
    require(read(test_fs::u8path(result.first)) == original + "; classic\n; G-code first:1\n; G-code second:1\n", "classic/plugin order");
    require(result.second == (root / "renamed.gcode").u8string(), "script rename was not returned");
    auto report = nlohmann::json::parse(read(storage / "G-code first.json"));
    require(report.at("output_name") == result.second && report.at("host") == "File", "plugin did not observe script rename");
    test_fs::remove(test_fs::u8path(result.first));
    set_mode("fatal"); fail(false, "G-code contract failure");
    config.set_key_value("slicing_pipeline_plugin", new ConfigOptionStrings());
    result = run(true);
    require(read(test_fs::u8path(result.first)) == original + "; classic\n", "classic-only regression");
    test_fs::remove(test_fs::u8path(result.first));
    config.set_key_value("post_process", new ConfigOptionStrings());
    auto path = source.u8string(); auto name = (root / "final.gcode").u8string();
    require(!run_post_process_scripts(path, true, "File", name, config) && read(source) == original, "no-selection path changed");
    clean(root);
}
