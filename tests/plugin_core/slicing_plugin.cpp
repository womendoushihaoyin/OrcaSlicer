#include "libslic3r/Layer.hpp"
#include "libslic3r/Print.hpp"
#include "slic3r/plugin/PluginManager.hpp"
#include "slic3r/plugin/PluginSlicing.hpp"
#include <filesystem>
#include <fstream>
#include <future>
#include <thread>

using namespace Slic3r;
namespace test_fs = std::filesystem;
namespace {
void require(bool condition, const std::string& message) { if (!condition) throw std::runtime_error(message); }
struct Fixture {
    Model model;
    DynamicPrintConfig config;
    Print print;
    Fixture(const std::string& key)
    {
        auto* object = model.add_object();
        object->add_volume(TriangleMesh(make_cube(10., 20., 1.)));
        object->add_instance();
        object->ensure_on_bed();
        const auto& defaults = FullPrintConfig::defaults();
        config.apply(static_cast<const PrintObjectConfig&>(defaults), true);
        config.apply(static_cast<const PrintRegionConfig&>(defaults), true);
        config.apply(static_cast<const PrintConfig&>(defaults), true);
        config.set_key_value("slicing_pipeline_plugin", new ConfigOptionStrings({"Contour contract"}));
        config.set_key_value("plugins", new ConfigOptionStrings({key + ";;Contour contract"}));
        print.set_status_silent();
        print.set_cancel_callback([] {});
        apply();
    }
    void apply() { print.apply(model, config); print.restart(); }
    void width(double expected) const
    {
        require(!print.objects().empty() && !print.objects().front()->layers().empty(), "empty graph");
        for (const auto* object : print.objects()) for (const auto* layer : object->layers()) {
            for (const auto& box : layer->lslices_bboxes)
                require(std::abs(unscale<double>(box.size().x()) - expected) < .01, "wrong plugin contour width");
            for (const auto* region : layer->regions()) require(!region->perimeters.empty(), "native perimeters were not generated");
        }
    }
};
nlohmann::json report(const test_fs::path& root)
{
    std::ifstream input(root / "report.json");
    require(bool(input), "Python plugin did not produce report");
    return nlohmann::json::parse(input);
}
void overrides(Fixture& fixture, const PluginCapabilityId& id, nlohmann::json value)
{
    CapabilityConfigDocument document;
    document.upsert({id, "1.0.0", std::move(value)});
    fixture.config.set_key_value("print_plugin_config_overrides", new ConfigOptionString(serialize_plugin_overrides(document)));
    fixture.apply();
}
void expect_failure(Fixture& fixture, const std::string& fragment)
{
    bool failed = false;
    try { fixture.print.process(); }
    catch (const std::exception& ex) { failed = true; require(std::string(ex.what()).find(fragment) != std::string::npos, ex.what()); }
    require(failed, "required plugin failure was silently skipped: " + fragment);
}
}

void test_slicing_plugin(const std::string& scenario, const test_fs::path& fixtures)
{
    auto& manager = PluginManager::instance();
    PluginDescriptor descriptor;
    std::string error;
    require(manager.install_plugin((fixtures / "slicing.py").u8string(), descriptor, error), error);
    manager.rescan_plugins();
    manager.load_plugin(descriptor.plugin_key, true);
    require(manager.wait_for_plugin_load(descriptor.plugin_key, std::chrono::seconds(20), error), error);
    const PluginCapabilityId id{PluginCapabilityType::SlicingPipeline, "Contour contract", descriptor.plugin_key};
    const auto storage = test_fs::u8path(manager.get_storage_dir(descriptor.plugin_key));
    require(manager.get_config().store_capability_config(id, {{"factor", .5}}), "store base config");
    Fixture fixture(descriptor.plugin_key);

    if (scenario == "slicing_plugin_pin") {
        require(manager.get_config().store_capability_config(id, {{"factor", .5}, {"wait", true}}), "store gate config");
        auto worker = std::async(std::launch::async, [&] { fixture.print.process(); });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while (!test_fs::exists(storage / "entered") && std::chrono::steady_clock::now() < deadline &&
               worker.wait_for(std::chrono::milliseconds(5)) != std::future_status::ready) {}
        const bool entered = test_fs::exists(storage / "entered");
        // Always release the gate before any assertion unwinds the future.
        const bool unloaded = manager.unload_plugin(id.plugin_key);
        const bool disabled = manager.set_capability_enabled(id, false);
        const bool deleted = manager.delete_plugin(id.plugin_key, error);
        PluginDescriptor candidate;
        const bool replaced = manager.install_plugin((fixtures / "slicing.py").u8string(), candidate, error);
        manager.get_config().store_capability_config(id, {{"factor", .9}});
        std::ofstream(storage / "release") << "continue";
        worker.get();
        require(entered && !unloaded && !disabled && !deleted && !replaced, "active slice did not pin its package");
        fixture.width(5.);
        const auto completed = report(storage);
        for (const auto& factor : completed.at("factors")) require(factor == .5, "slice read a changing global config");
        require(manager.unload_plugin(id.plugin_key), "consumer pin leaked after completion");
        return;
    }
    if (scenario == "slicing_plugin_errors") {
        const auto good = fixture.config;
        fixture.config.set_key_value("slicing_pipeline_plugin", new ConfigOptionStrings({id.name, "missing second"}));
        fixture.config.set_key_value("plugins", new ConfigOptionStrings({id.plugin_key + ";;" + id.name, "missing;;missing second"}));
        fixture.apply();
        expect_failure(fixture, "missing, unloaded or disabled");
        require(!test_fs::exists(storage / "report.json") && !fixture.print.is_step_done(posSlice), "partial selection executed before preflight");
        fixture.config = good;
        fixture.config.set_key_value("plugins", new ConfigOptionStrings()); fixture.apply();
        expect_failure(fixture, "No package reference");
        fixture.config = good;
        fixture.config.set_key_value("plugins", new ConfigOptionStrings({id.plugin_key + ";;" + id.name, "missing;;" + id.name})); fixture.apply();
        expect_failure(fixture, "Ambiguous");
        fixture.config = good;
        fixture.config.set_key_value("plugins", new ConfigOptionStrings({"missing;;" + id.name})); fixture.apply();
        expect_failure(fixture, "missing, unloaded or disabled");
        fixture.config = good; fixture.apply();
        require(manager.set_capability_enabled(id, false), "disable idle plugin");
        expect_failure(fixture, "missing, unloaded or disabled");
        manager.set_capability_enabled(id, true);
        fixture.config.set_key_value("print_plugin_config_overrides", new ConfigOptionString("{broken")); fixture.apply();
        expect_failure(fixture, "invalid plugin capability configuration JSON");
        fixture.config = good;
        for (const auto& mode : {"fatal", "raise", "ui"}) {
            overrides(fixture, id, {{"factor", .5}, {"mode", mode}});
            expect_failure(fixture, std::string(mode) == "ui" ? "Blocking UI calls" : "contract failure");
            require(!fixture.print.is_step_done(posSlice) && !fixture.print.is_step_done(psSkirtBrim), "failed Python graph remained valid");
        }
        overrides(fixture, id, {{"factor", .5}});
        fixture.print.process(); fixture.width(5.);
        require(manager.get_plugin_capability(id)->ref_count() == 0, "exception leaked consumer pin");
        // A feature-disabled/unavailable runtime must not silently make a different part.
        set_slicing_pipeline_hook_factory(nullptr);
        expect_failure(fixture, "runtime is unavailable");
        install_slicing_plugin_dispatcher();
        return;
    }
    fixture.print.process(); fixture.width(5.);
    auto first = report(storage);
    require(first.at("steps").size() == 11 && first.at("steps").at(0) == "posSlice", "real Python dispatcher missed stages");
    fixture.print.process();
    require(report(storage) == first, "warm cache reran selected plugin");
    // No PrintConfig edit: the effective global settings still require a cold rebuild.
    require(manager.get_config().store_capability_config(id, {{"factor", .6}}), "edit global config");
    fixture.print.process(); fixture.width(6.);
    require(manager.unload_plugin(id.plugin_key), "unload idle pipeline");
    manager.load_plugin(id.plugin_key, true);
    require(manager.wait_for_plugin_load(id.plugin_key, std::chrono::seconds(20), error), error);
    test_fs::remove(storage / "report.json");
    fixture.print.process(); fixture.width(6.);
    require(report(storage).at("steps").at(0) == "posSlice", "same-version reload reused previous instance geometry");
    fixture.config.set_key_value("wall_loops", new ConfigOptionInt(4)); fixture.apply();
    fixture.print.process(); fixture.width(6.);
    overrides(fixture, id, {{"factor", .75}});
    require(!fixture.print.is_step_done(posSlice), "override change did not invalidate slice");
    fixture.print.process(); fixture.width(7.5);
    // Config serialization, as used in preset/project INI payloads, keeps refs and overrides.
    DynamicPrintConfig restored;
    for (const auto& key : {"slicing_pipeline_plugin", "plugins", "print_plugin_config_overrides"})
        restored.set_deserialize_strict(key, fixture.config.option(key)->serialize());
    for (const auto& key : restored.keys()) require(restored.option(key)->serialize() == fixture.config.option(key)->serialize(), "config roundtrip lost plugin refs");
    fixture.config.set_key_value("slicing_pipeline_plugin", new ConfigOptionStrings()); fixture.apply();
    const auto before = report(storage);
    fixture.print.process(); fixture.width(10.);
    require(report(storage) == before, "cleared selection still dispatched");
}
