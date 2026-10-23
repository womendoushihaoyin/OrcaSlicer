#include "slic3r/plugin/PluginManager.hpp"
#include "slic3r/plugin/PluginAuditManager.hpp"
#include "slic3r/plugin/PluginDependencyInstaller.hpp"
#include "slic3r/plugin/PluginInstallTransaction.hpp"
#include "slic3r/plugin/PythonInterpreter.hpp"
#include "slic3r/plugin/pluginTypes/pages/PagesPluginCapability.hpp"
#include "slic3r/plugin/pluginTypes/resourceTexture/ResourceTexturePluginCapability.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Layer.hpp"
#include <boost/log/core.hpp>
#include <boost/nowide/args.hpp>
#include <boost/nowide/filesystem.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <future>
#include <thread>

namespace test_fs = std::filesystem;
void test_gcode_plugin(const std::string& scenario, const test_fs::path& fixtures);
void test_project_dirty();
void test_config_notify();
void test_slicing_invalidation();
void test_contour(const std::string& scenario, const test_fs::path& output);
void test_texture_plugin(const std::string& scenario, const test_fs::path& fixtures);
void test_texture_layers();
void test_texture_kernel(const std::string& scenario, const test_fs::path& fixtures);
void test_texture_obj(const test_fs::path& output);
void test_texture_editor(const std::string& scenario, const test_fs::path& fixtures);
void test_texture_color_editor(const std::string& scenario, const test_fs::path& fixtures);
void test_texture_history(const test_fs::path& fixtures);
void test_texture_cut(const test_fs::path& fixtures);
void test_texture_layout_tools(const test_fs::path& fixtures);
void test_texture_image_adjust(const test_fs::path& fixtures);
void test_texture_island_groups(const test_fs::path& fixtures);
void test_texture_authoring(const test_fs::path& fixtures);
void test_texture_vertex_indices(const test_fs::path& fixtures);
void test_texture_seam_subdivision(const test_fs::path& fixtures);
void test_texture_edge_origins(const test_fs::path& fixtures);
void test_texture_visibility(const test_fs::path& fixtures);
void test_texture_mix_recipes(const test_fs::path& fixtures, const test_fs::path& output);
void test_texture_boundaries(const test_fs::path& fixtures, const test_fs::path& output);
void test_dependency_environment(const test_fs::path& fixtures);
void test_texture_model(const std::string& scenario, const test_fs::path& fixtures, const test_fs::path& output);
void test_path3_projection();
void test_path3(const std::string& scenario);
void test_slicing_stages(const std::string& scenario);
void test_slicing_plugin(const std::string& scenario, const test_fs::path& fixtures);
using namespace Slic3r;
using json = nlohmann::json;
namespace {
void check(bool ok, const std::string& message)
{
    if (!ok)
        throw std::runtime_error(message);
}
PluginDescriptor install(const test_fs::path& file)
{
    PluginDescriptor entry;
    std::string      error;
    check(PluginManager::instance().install_plugin(boost::filesystem::path(file.u8string()), entry, error), "install: " + error);
    PluginManager::instance().rescan_plugins();
    return entry;
}
void load(const std::string& key, bool expected = true)
{
    auto&       manager = PluginManager::instance();
    std::string error;
    manager.load_plugin(key, true);
    const bool ok = manager.wait_for_plugin_load(key, std::chrono::seconds(20), error);
    check(ok == expected && manager.is_plugin_loaded(key) == expected, "load: " + error + " / " + manager.get_plugin_load_error(key));
}
json execute(const std::string& key)
{
    std::string error;
    const auto  result = PluginManager::instance().run_script_capability(key, "Counter", error);
    check(error.empty() && result.status == PluginResult::Success, "execute: " + error);
    return json::parse(result.data);
}
void test(const std::string& scenario, const test_fs::path& fixtures, const test_fs::path& data)
{
    auto& manager = PluginManager::instance();
    manager.discover_plugins();
    check(manager.is_discovery_complete(), "discovery incomplete");
    std::string error;
    if (scenario == "config_notify") {
        test_config_notify();
    } else if (scenario == "slicing_invalidation") {
        test_slicing_invalidation();
    } else if (scenario == "project_dirty") {
        test_project_dirty();
    } else if (scenario.rfind("contour_", 0) == 0) {
        test_contour(scenario, data / "contour-output");
    } else if (scenario == "texture_layers") {
        test_texture_layers();
    } else if (scenario == "texture_model" || scenario == "texture_3mf") {
        test_texture_model(scenario, fixtures, data / "texture-output");
    } else if (scenario == "texture_kernel" || scenario == "texture_projection" || scenario == "texture_lscm") {
        test_texture_kernel(scenario, fixtures);
    } else if (scenario == "texture_obj") {
        test_texture_obj(data / "texture-obj-output");
    } else if (scenario == "texture_mix_recipes") {
        test_texture_mix_recipes(fixtures, data / "texture-mix-recipes");
    } else if (scenario == "texture_visibility") {
        test_texture_visibility(fixtures);
    } else if (scenario == "texture_edge_origins") {
        test_texture_edge_origins(fixtures);
    } else if (scenario == "texture_seam_subdivision") {
        test_texture_seam_subdivision(fixtures);
    } else if (scenario == "texture_vertex_indices") {
        test_texture_vertex_indices(fixtures);
    } else if (scenario == "texture_authoring") {
        test_texture_authoring(fixtures);
    } else if (scenario == "texture_island_groups") {
        test_texture_island_groups(fixtures);
    } else if (scenario == "texture_image_adjust") {
        test_texture_image_adjust(fixtures);
    } else if (scenario == "texture_layout_tools") {
        test_texture_layout_tools(fixtures);
    } else if (scenario == "texture_cut") {
        test_texture_cut(fixtures);
    } else if (scenario == "texture_history") {
        test_texture_history(fixtures);
    } else if (scenario == "texture_color_palette" || scenario == "texture_color_commit" || scenario == "texture_color_v2") {
        test_texture_color_editor(scenario, fixtures);
    } else if (scenario == "texture_boundaries") {
        test_texture_boundaries(fixtures, data / "texture-boundaries-output");
    } else if (scenario == "texture_editor" || scenario == "texture_editor_invalid" || scenario == "texture_editor_bake" || scenario == "texture_editor_result") {
        test_texture_editor(scenario, fixtures);
    } else if (scenario.rfind("texture_", 0) == 0) {
        test_texture_plugin(scenario, fixtures);
    } else if (scenario == "path3_projection") {
        test_path3_projection();
    } else if (scenario.rfind("path3_", 0) == 0) {
        test_path3(scenario);
    } else if (scenario.rfind("gcode_plugin", 0) == 0) {
        test_gcode_plugin(scenario, fixtures);
    } else if (scenario == "slicing_plugin" || scenario == "slicing_plugin_errors" || scenario == "slicing_plugin_pin") {
        test_slicing_plugin(scenario, fixtures);
    } else if (scenario == "slicing_stages" || scenario == "slicing_shared" || scenario == "slicing_failure" ||
        scenario == "slicing_cancel" || scenario == "slicing_drain" || scenario == "slicing_noop") {
        test_slicing_stages(scenario);
    } else if (scenario == "stderr") {
        PythonGILState gil;
        auto           stream = pybind11::module_::import("sys").attr("stderr");
        check(pybind11::hasattr(stream, "_path"), "Python stderr redirect was not installed");
        const auto path = stream.attr("_path").cast<std::string>();
        stream.attr("write")("plugin stderr contract marker\n");
        stream.attr("flush")();
        std::ifstream     input(test_fs::u8path(path));
        const std::string contents((std::istreambuf_iterator<char>(input)), {});
        check(contents.find("plugin stderr contract marker") != std::string::npos, "Python error log was not written");
    } else if (scenario == "metadata") {
        PluginDescriptor descriptor;
        bool             existing = false;
        check(manager.inspect_local_plugin_package((fixtures / "metadata_only.py").u8string(), descriptor, existing, error), error);
        check(!existing && descriptor.name == "Core Contract" && descriptor.version == "1.0.0", "metadata was not inspected");
        error.clear();
        check(!manager.install_plugin((fixtures / "invalid.py").u8string(), error), "invalid metadata accepted");
        check(manager.get_plugin_capabilities().empty(), "metadata scan ran code");
        CloudPluginService cloud;
        check(!cloud.can_fetch_cloud_plugins(), "local mode unexpectedly has cloud backend");
        check(!cloud.request_cloud_subscribe("test", error) && !error.empty(), "disabled cloud must explain failure");
    } else if (scenario == "config") {
        const auto entry = install(fixtures / "counter.py");
        load(entry.plugin_key);
        check(execute(entry.plugin_key).at("count") == 1, "initial count");
        check(execute(entry.plugin_key).at("count") == 2, "stored count");
        const PluginCapabilityId id{PluginCapabilityType::Script, "Counter", entry.plugin_key};
        const PluginCapabilityId other{PluginCapabilityType::Script, "Counter", "another_package"};
        check(manager.get_config().store_capability_config(other, {{"count", 99}}), "other config save");
        const auto invalid = PluginConfig::save_config_response(id, json("{broken"));
        check(invalid.contains("error"), "invalid config accepted");
        check(manager.get_config().get_config(id)->config.at("count") == 2, "invalid save overwrote config");
        check(manager.unload_plugin(entry.plugin_key), "unload failed");
        load(entry.plugin_key);
        check(execute(entry.plugin_key).at("count") == 3, "reload lost config");
        check(manager.get_config().get_config(other)->config.at("count") == 99, "package config isolation");
        manager.set_capability_enabled(id, false);
        check(!manager.get_plugin_capability(id), "disabled capability resolved");
        check(manager.unload_plugin(entry.plugin_key), "unload disabled failed");
        load(entry.plugin_key);
        check(!manager.get_plugin_capability(id), "capability disabled state not durable");
        manager.set_capability_enabled(id, true);
        LifecycleEventContext context;
        context.name = "contract object";
        context.id   = "object-7";
        fire_lifecycle_event(LifecycleEvent::ObjectAdded, context);
        check(manager.get_config().get_config(id)->config.at("event") == "contract object", "lifecycle not dispatched");
        check(manager.delete_plugin(entry.plugin_key, error), error);
        check(manager.get_config().get_config(id).has_value(), "uninstall erased configuration");
    } else if (scenario == "replacement" || scenario == "replacement_crash") {
        const auto entry = install(fixtures / "counter.py");
        load(entry.plugin_key);
        check(execute(entry.plugin_key).at("count") == 1, "old plugin config");
        const PluginCapabilityId id{PluginCapabilityType::Script, "Counter", entry.plugin_key};
        const PluginCapabilityId other{PluginCapabilityType::Script, "Counter", "unrelated"};
        check(manager.get_config().store_capability_config(other, {{"count", 71}}), "other config before replacement");
        check(plugin_loader::configure_dependency_install({true, fixtures.u8string()}, error), error);
        if (scenario == "replacement_crash") {
            install(fixtures / "crash/counter.py");
            load(entry.plugin_key); // on_load terminates this isolated test process with code 82.
            throw std::runtime_error("crashing candidate unexpectedly survived");
        }
        for (const char* variant : {"broken", "import_error", "dependency_error"}) {
            const auto updated = install(fixtures / variant / "counter.py");
            check(has_pending_plugin_replacement(updated.plugin_root), "backup removed before activation");
            check(manager.get_config().store_capability_config(other, {{"count", 72}}), "other concurrent config");
            manager.load_plugin(entry.plugin_key, false);
            check(!manager.wait_for_plugin_load(entry.plugin_key, std::chrono::seconds(30), error), "invalid candidate activated");
            check(!manager.is_plugin_loaded(entry.plugin_key), "failed candidate still loaded");
            check(!has_pending_plugin_replacement(updated.plugin_root), "rollback journal remained");
            check(manager.get_config().get_config(id)->config.at("count") == 1, "failed candidate changed old config");
            check(manager.get_config().get_config(other)->config.at("count") == 72, "rollback changed another package");
            PluginDescriptor restored;
            check(manager.try_get_valid_plugin_descriptor(entry.plugin_key, restored) && restored.version == "1.0.0", "old descriptor not restored");
            PluginInstallState state;
            check(read_install_state(restored.plugin_root, state) && state.enabled, "old auto-load flag lost");
            load(entry.plugin_key);
        }
        install(fixtures / "updated/counter.py");
        load(entry.plugin_key);
        check(!has_pending_plugin_replacement(entry.plugin_root), "successful activation kept a pending transaction");
        PluginDescriptor active;
        check(manager.try_get_valid_plugin_descriptor(entry.plugin_key, active) && active.version == "2.0.0", "new version not active");
        check(execute(entry.plugin_key).at("count") == 2, "update lost configuration");
    } else if (scenario == "replacement_edges") {
        const auto entry = install(fixtures / "counter.py");
        load(entry.plugin_key);
        check(execute(entry.plugin_key).at("count") == 1, "initial config");
        const PluginCapabilityId id{PluginCapabilityType::Script, "Counter", entry.plugin_key};
        auto capability = manager.get_plugin_capability(id);
        capability->increment();
        PluginDescriptor candidate;
        check(!manager.install_plugin((fixtures / "updated/counter.py").u8string(), candidate, error), "replaced active capability");
        capability->decrement();
        capability.reset();
        check(manager.is_plugin_loaded(entry.plugin_key), "rejected install unloaded active package");
        install(fixtures / "broken/counter.py");
        // Make config publication fail without relying on user/root permissions.
        const auto config_path = test_fs::u8path(PluginConfig::plugin_config_file());
        const auto saved = data / "saved-config.json";
        test_fs::rename(config_path, saved);
        test_fs::create_directory(config_path);
        manager.load_plugin(entry.plugin_key, true);
        check(!manager.wait_for_plugin_load(entry.plugin_key, std::chrono::seconds(30), error), "failed write accepted");
        check(has_pending_plugin_replacement(entry.plugin_root), "failed recovery discarded journal");
        check(!can_activate_plugin_replacement(entry.plugin_root, error), "incomplete rollback allowed execution");
        test_fs::remove(config_path);
        test_fs::rename(saved, config_path);
        check(recover_plugin_replacements(manager.get_config(), error), error);
        manager.rescan_plugins();
        load(entry.plugin_key);
        check(execute(entry.plugin_key).at("count") == 2, "retry recovery lost old config");
        // Disabled capability flags survive a successful replacement.
        manager.set_capability_enabled(id, false);
        install(fixtures / "updated/counter.py");
        load(entry.plugin_key);
        check(!manager.get_plugin_capability(id), "replacement enabled disabled capability");
        // A candidate removed before activation cannot be resurrected on restart.
        install(fixtures / "broken/counter.py");
        check(manager.delete_plugin(entry.plugin_key, error), error);
        check(!test_fs::exists(test_fs::u8path(entry.plugin_root)), "pending remove kept package");
        check(!has_pending_plugin_replacement(entry.plugin_root), "pending remove kept recovery journal");
    } else if (scenario == "replacement_journal") {
        const auto entry = install(fixtures / "counter.py");
        load(entry.plugin_key);
        check(execute(entry.plugin_key).at("count") == 1, "initial journal fixture config");
        install(fixtures / "updated/counter.py");
        const auto txn = data / "orca_plugins/.install-transactions/counter.py";
        const auto journal = txn / "journal.json";
        std::ifstream stream(journal);
        const std::string original((std::istreambuf_iterator<char>(stream)), {});
        stream.close();
        std::ofstream(journal, std::ios::trunc) << "{corrupted";
        check(!recover_plugin_replacements(manager.get_config(), error), "corrupt journal was accepted");
        check(test_fs::exists(txn / "previous/counter.py") && test_fs::exists(test_fs::u8path(entry.plugin_root) / "counter.py"),
              "corrupt recovery removed package files");
        std::ofstream(journal, std::ios::trunc) << original;
        check(recover_plugin_replacements(manager.get_config(), error), error);
        manager.rescan_plugins();
        load(entry.plugin_key);
        check(execute(entry.plugin_key).at("count") == 2, "valid journal did not restore previous config");
        test_fs::create_directories(txn / "candidate");
        std::ofstream(txn / "candidate/counter.py") << "unfinished staging";
        check(recover_plugin_replacements(manager.get_config(), error) && !test_fs::exists(txn),
              "unpublished staging blocked subsequent replacement");
        test_fs::create_directories(txn / "previous");
        std::ofstream(txn / "previous/counter.py") << "must retain backup";
        check(!recover_plugin_replacements(manager.get_config(), error) && test_fs::exists(txn / "previous/counter.py"),
              "missing journal destroyed backup");
        test_fs::remove_all(txn); // fixture-owned files only
#ifndef _WIN32
        test_fs::create_directories(txn / "candidate");
        test_fs::create_symlink(txn / "absent", journal);
        check(!recover_plugin_replacements(manager.get_config(), error) && test_fs::is_symlink(journal),
              "dangling journal symlink treated as absent");
        test_fs::remove(journal);
        check(recover_plugin_replacements(manager.get_config(), error), error);
#endif
        install(fixtures / "updated/counter.py");
        load(entry.plugin_key);
        check(execute(entry.plugin_key).at("count") == 3, "recovery prevented a later valid replacement");
    } else if (scenario == "profile_hold") {
        std::ofstream(data / "ready") << "ready";
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while (!test_fs::exists(data / "release") && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        check(test_fs::exists(data / "release"), "profile holder timed out");
    } else if (scenario == "replacement_recover") {
        PluginDescriptor old;
        check(manager.try_get_valid_plugin_descriptor("counter", old) && old.version == "1.0.0", "startup did not restore previous package");
        check(!has_pending_plugin_replacement(old.plugin_root), "startup recovery journal remained");
        load(old.plugin_key);
        check(execute(old.plugin_key).at("count") == 2, "startup did not restore previous config");
        const PluginCapabilityId other{PluginCapabilityType::Script, "Counter", "unrelated"};
        check(manager.get_config().get_config(other)->config.at("count") == 71, "startup recovery changed unrelated config");
    } else if (scenario == "wheel") {
        const auto entry = install(fixtures / "wheelcounter-1.0.0-py3-none-any.whl");
        load(entry.plugin_key);
        check(execute(entry.plugin_key).at("count") == 1, "wheel execution");
        check(manager.unload_plugin(entry.plugin_key), "wheel unload");
        {
            PythonGILState gil;
            auto           modules = pybind11::module_::import("sys").attr("modules");
            check(!modules.contains("wheelcounter"), "wheel module leaked after unload");
        }
        load(entry.plugin_key);
        check(execute(entry.plugin_key).at("count") == 2, "wheel reload");
    } else if (scenario == "multicap") {
        const auto entry = install(fixtures / "multicap-1.0.0-py3-none-any.whl");
        load(entry.plugin_key);
        auto page = std::dynamic_pointer_cast<PagesPluginCapability>(
            manager.get_plugin_capability({PluginCapabilityType::Pages, "Contract Page", entry.plugin_key}));
        auto texture = std::dynamic_pointer_cast<ResourceTexturePluginCapability>(
            manager.get_plugin_capability({PluginCapabilityType::ResourceTexture, "Contract Texture", entry.plugin_key}));
        check(page && texture, "multiple capability registration failed");
        check(page->get_ui().find("Local page") != std::string::npos, "page HTML");
        std::string reply;
        page->set_message_sender([&reply](const std::string& message) { reply = message; });
        page->on_message(R"({"value":7})");
        check(json::parse(reply)["reply"]["value"] == 7, "page bridge payload");
        page->clear_message_sender();
        reply.clear();
        page->on_message("null");
        check(reply.empty(), "detached page still received messages");
        check(collect_resource_texture_files(texture->get_texture_dir(), entry.plugin_root, error).size() == 1,
              "texture file discovery: " + error);
        page.reset();
        texture.reset();
        check(manager.unload_plugin(entry.plugin_key), "multi capability unload");
    } else if (scenario == "audit") {
        const auto entry = install(fixtures / "audit.py");
        load(entry.plugin_key);
        const auto storage = test_fs::u8path(manager.get_storage_dir(entry.plugin_key));
        std::ofstream(storage / "Snapmaker_Orca.conf") << "contract fixture, no credentials";
        PluginAuditManager::set_permission_prompt_for_testing([](const auto&, const auto&) { return false; });
        manager.run_script_capability(entry.plugin_key, "Audit", error);
        check(!error.empty(), "denied config read succeeded");
        check(test_fs::is_regular_file(storage / "result.txt"), "allowed storage write failed");
        PluginAuditManager::set_permission_prompt_for_testing({});
    } else if (scenario == "failure") {
        for (const char* name : {"duplicate.py", "failure.py"}) {
            const auto entry = install(fixtures / name);
            load(entry.plugin_key, false);
            check(!manager.get_plugin_load_error(entry.plugin_key).empty(), "load failure lost traceback");
            check(manager.get_plugin_capabilities().empty(), "failed load leaked capability");
        }
        const auto good = install(fixtures / "counter.py");
        load(good.plugin_key);
        check(execute(good.plugin_key).at("count") == 1, "registration capture poisoned by failure");
    } else if (scenario == "paths") {
        check(!extract_zip_to_directory((fixtures / "traversal-1.0.0-py3-none-any.whl").u8string(), (data / "extract").u8string(), error),
              "zip traversal accepted");
        check(!test_fs::exists(data.parent_path() / "escaped.txt") && !test_fs::exists(data / "escaped.txt"), "zip escaped target");
        const auto entry = install(fixtures / "counter.py");
        std::ofstream(test_fs::u8path(entry.plugin_root) / "second.py") << "raise RuntimeError('do not run')";
        manager.rescan_plugins();
        const auto descriptors = manager.get_plugin_descriptors(true);
        check(descriptors.size() == 1 && descriptors.front().is_invalid_package(), "multiple entries not rejected");
    } else if (scenario == "dependency_environment") {
        test_dependency_environment(fixtures);
    } else if (scenario == "dependencies") {
        check(plugin_loader::configure_dependency_install({true, fixtures.u8string()}, error), error);
        check(plugin_loader::dependency_install_options().offline, "offline policy not persisted");
        check(plugin_loader::install_packages({"contract-dep==1.0.0"}, error), "offline install: " + error);
        const auto target = test_fs::u8path(PythonInterpreter::shared_packages_dir());
        check(test_fs::is_regular_file(target / "contract_dep/__init__.py"), "offline dependency missing");
        error.clear();
        check(!plugin_loader::install_packages({"contract-dep==2.0.0"}, error), "conflicting dependency replaced active version");
        std::ifstream source(target / "contract_dep/__init__.py");
        std::string   code((std::istreambuf_iterator<char>(source)), {});
        check(code.find("1.0.0") != std::string::npos, "conflict damaged old dependency");
        error.clear();
        check(!plugin_loader::install_packages({"definitely-missing-contract-package==1.0.0"}, error), "missing offline package accepted");
        check(test_fs::is_regular_file(target / "contract_dep/__init__.py"), "failed install lost old dependency");
        {
            PythonGILState gil;
            pybind11::module_::import("importlib").attr("invalidate_caches")();
            check(pybind11::module_::import("contract_dep").attr("VALUE").cast<std::string>() == "1.0.0", "installed dependency import");
        }
    } else if (scenario == "slicing_graph") {
        Model model;
        auto* model_object = model.add_object();
        model_object->add_volume(TriangleMesh(make_cube(10., 20., 1.)));
        model_object->add_instance();
        model_object->ensure_on_bed();
        DynamicPrintConfig config;
        const auto& defaults = FullPrintConfig::defaults();
        config.apply(static_cast<const PrintObjectConfig&>(defaults), true);
        config.apply(static_cast<const PrintRegionConfig&>(defaults), true);
        config.apply(static_cast<const PrintConfig&>(defaults), true);
        Print print;
        print.auto_assign_extruders(model_object);
        print.apply(model, config);
        print.set_status_silent();
        check(print.objects().size() == 1, "print graph not created");
        auto* object = print.get_object(size_t(0));
        object->slice();
        check(!object->layers().empty(), "no sliced layers");
        {
            PythonGILState gil;
            auto scope = pybind11::dict();
            pybind11::module_::import("orca");
            scope["graph"] = pybind11::cast(&print, pybind11::return_value_policy::reference);
            pybind11::exec(R"PY(
import orca
obj = graph.objects()[0]
assert len(graph.model().objects()) == 1
assert obj.model_object() is not None
assert obj.config_value("missing-contract-option") is None
layers = obj.layers()
assert layers[0].lower_layer is None
assert layers[-1].upper_layer is None
for layer in layers:
    for region in layer.regions():
        assert region.layer().print_z == layer.print_z
        assert region.config_value("missing-contract-option") is None
        surfaces = region.slices.surfaces
        assert surfaces
        for surface in surfaces:
            before = surface.area()
            for point in surface.expolygon.contour.points:
                point.x = round(point.x / 2)
            assert abs(surface.area() - before / 2) < 100000000
    layer.make_slices()
    assert layer.lslices()
# Value-owned Surface and mutable collection use the same SDK as the live graph.
polygon = orca.host.Polygon()
for x,y in [(0,0),(1000,0),(1000,1000),(0,1000)]: polygon.append(orca.host.Point(x,y))
owned = orca.host.Surface(orca.host.SurfaceType.stTop, orca.host.ExPolygon(polygon))
assert owned.is_top() and owned.area() == 1000000
)PY", scope);
        }
        for (const auto* layer : object->layers()) {
            check(layer->lslices_bboxes.size() == layer->lslices.size(), "slice bbox cache not refreshed");
            for (const auto& bounds : layer->lslices_bboxes)
                check(bounds.size().x() <= scale_(5.01), "Python did not change native slice geometry");
        }
        print.process();
        size_t paths = 0;
        std::function<void(const ExtrusionEntity*, const BoundingBox&)> verify_paths;
        verify_paths = [&](const ExtrusionEntity* entity, const BoundingBox& bounds) {
            if (const auto* collection = dynamic_cast<const ExtrusionEntityCollection*>(entity)) {
                for (const auto* child : collection->entities) verify_paths(child, bounds);
            } else if (const auto* loop = dynamic_cast<const ExtrusionLoop*>(entity)) {
                for (const auto& path : loop->paths) verify_paths(&path, bounds);
            } else if (const auto* multi = dynamic_cast<const ExtrusionMultiPath*>(entity)) {
                for (const auto& path : multi->paths) verify_paths(&path, bounds);
            } else if (const auto* path = dynamic_cast<const ExtrusionPath*>(entity)) {
                ++paths;
                for (const auto& point : path->polyline.points)
                    check(point.x() >= bounds.min.x() - scale_(0.1) && point.x() <= bounds.max.x() + scale_(0.1),
                          "perimeters ignored Python slice mutation");
            }
        };
        for (const auto* layer : object->layers()) {
            check(layer->lslices_bboxes.size() == 1, "expected one cube island");
            for (const auto* region : layer->regions()) verify_paths(&region->perimeters, layer->lslices_bboxes.front());
        }
        check(paths > 0, "no downstream extrusion paths");
        {
            PythonGILState gil;
            auto scope = pybind11::dict();
            pybind11::module_::import("orca");
            scope["graph"] = pybind11::cast(&print, pybind11::return_value_policy::reference);
            pybind11::exec(R"PY(
paths = graph.objects()[0].layers()[0].regions()[0].perimeters.flatten_paths()
assert paths and all(p.width > 0 and p.mm3_per_mm > 0 for p in paths)
)PY", scope);
        }
    } else if (scenario == "presets") {
        PythonGILState gil;
        namespace py = pybind11;
        py::module_::import("orca");
        PresetBundle bundle;
        bundle.prints.get_edited_preset().config.set_key_value("layer_height", new ConfigOptionFloat(0.173));
        py::dict globals;
        globals["bundle"] = py::cast(&bundle, py::return_value_policy::reference);
        py::exec(R"PY(
for method in (bundle.full_config_keys, lambda: bundle.full_config_value("layer_height")):
    try: method()
    except RuntimeError as error: assert "selections are not initialized" in str(error)
    else: raise AssertionError("Uninitialized FFF selection was accepted")
)PY", globals);
        bundle.filament_presets.push_back(bundle.filaments.get_selected_preset_name());
        py::exec(R"PY(
import orca
assert bundle.prints.size() > 0 and bundle.printers.size() > 0
assert bundle.prints.selected_preset_name() == bundle.prints.get_selected_preset_name()
assert bundle.prints.selected_preset() is bundle.prints.get_selected_preset()
assert bundle.current_process_preset() is bundle.current_print_preset()
preset = bundle.current_process_preset()
assert preset.type == orca.host.PresetType.Print
assert "layer_height" in preset.config_keys()
assert float(preset.config_value("layer_height")) == 0.173
assert preset.config_value("not_a_real_option") is None
assert bundle.prints.find_preset("not_a_real_preset") is None
assert len(bundle.prints.preset_names()) == bundle.prints.size()
assert bundle.printers.preset(0).type == orca.host.PresetType.Printer
assert bundle.full_config_value("not_a_real_option") is None
try: bundle.prints.preset(bundle.prints.size())
except IndexError: pass
else: raise AssertionError("preset index not checked")
for access in (orca.host.plater, orca.host.model, orca.host.preset_bundle):
    try: access()
    except RuntimeError: pass
    else: raise AssertionError("headless GUI access unexpectedly succeeded")
)PY", globals);
    } else if (scenario == "geometry" || scenario == "numpy") {
        if (scenario == "numpy") {
            check(plugin_loader::configure_dependency_install({true, fixtures.u8string()}, error), error);
            check(plugin_loader::install_packages({"numpy==2.2.6"}, error), "native offline wheel: " + error);
        }
        PythonGILState gil;
        namespace py = pybind11;
        py::module_::import("orca");
        auto  model  = std::make_unique<Model>();
        auto* object = model->add_object();
        object->name = "contract cube";
        object->add_volume(make_cube(10.0, 20.0, 30.0));
        object->add_instance()->set_offset(Vec3d(5.0, 6.0, 0.0));
        auto* mirrored = object->add_instance();
        mirrored->set_mirror(Vec3d(-1.0, 1.0, 1.0));
        py::dict globals;
        globals["model"] = py::cast(model.get(), py::return_value_policy::reference);
        py::exec(R"PY(
import orca
assert model.object_count() == 1
obj = model.object(0)
assert obj.name == "contract cube" and obj.instance_count() == 2
assert obj.instance(0).offset() == (5.0, 6.0, 0.0)
assert obj.instance(1).is_left_handed()
volume = obj.volume(0)
mesh = volume.mesh()
assert mesh.vertex_count() == 8 and mesh.triangle_count() == 12
assert len(mesh.vertex(0)) == 3 and len(mesh.triangle(0)) == 3
try: mesh.vertex(8)
except IndexError: pass
else: raise AssertionError("vertex bounds were not checked")
try: model.object(1)
except IndexError: pass
else: raise AssertionError("model bounds were not checked")
)PY",
                 globals);
        if (scenario == "numpy") {
            auto spatial = std::make_unique<ExtrusionPath>(erTopSolidInfill);
            spatial->set_polyline3(Polyline3(Points3{Vec3crd(10, 20, -100), Vec3crd(30, 40, 200)}));
            auto planar = std::make_unique<ExtrusionPath>(erPerimeter);
            planar->set_polyline(Polyline{Point(1, 2), Point(3, 4)});
            globals["spatial"] = py::cast(spatial.get(), py::return_value_policy::reference);
            globals["planar"] = py::cast(planar.get(), py::return_value_policy::reference);
            py::exec(R"PY(
import numpy as np
xyz = spatial.points()
xy0 = planar.points()
assert xyz.shape == (2,3) and xyz.dtype == np.int64 and not xyz.flags.writeable
assert xyz.tolist() == [[10,20,-100], [30,40,200]]
assert xy0.tolist() == [[1,2,0], [3,4,0]]
try: xyz[0,2] = 9
except ValueError: pass
else: raise AssertionError("extrusion snapshot was writable")
del spatial, planar
)PY", globals);
            spatial.reset(); planar.reset();
            py::exec("assert xyz.tolist() == [[10,20,-100], [30,40,200]]", globals);
            py::exec(R"PY(
import numpy as np
vertices = mesh.vertices()
triangles = mesh.triangles()
assert vertices.shape == (8,3) and vertices.dtype == np.float32
assert triangles.shape == (12,3) and triangles.dtype == np.int32
assert not vertices.flags.writeable and not triangles.flags.writeable
assert vertices.base is not None and triangles.base is not None
normals = mesh.face_normals()
assert normals.shape == (12,3) and normals.dtype == np.float32
matrix = obj.instance(0).matrix()
assert matrix.shape == (4,4) and matrix.dtype == np.float64
assert tuple(matrix[:3,3]) == (5.0, 6.0, 0.0)
retained_vertices = vertices.copy()
retained_triangles = triangles.copy()
copied = vertices.copy()
copied[0,0] += 100
assert copied[0,0] != vertices[0,0]
try: vertices[0,0] = 12
except ValueError: pass
else: raise AssertionError("zero-copy mesh view was writable")
del volume, obj, model, mesh
)PY",
                     globals);
            model.reset();
            py::exec("assert np.array_equal(vertices, retained_vertices)\nassert np.array_equal(triangles, retained_triangles)", globals);
        } else {
            py::exec(R"PY(
try: mesh.vertices()
except ImportError as error: assert "numpy is required" in str(error)
else: raise AssertionError("isolated geometry test unexpectedly used system numpy")
)PY",
                     globals);
        }
    } else if (scenario == "lease" || scenario == "lease_out_of_order") {
        std::promise<void> entered;
        auto               ready = entered.get_future();
        std::thread        worker([&entered, &scenario] {
            auto first  = std::make_unique<PythonRuntimeLease>(PythonInterpreter::instance().acquire_runtime_lease());
            auto nested = PythonInterpreter::instance().acquire_runtime_lease();
            if (scenario == "lease_out_of_order")
                first.reset();
            entered.set_value();
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        });
        ready.wait();
        const auto start = std::chrono::steady_clock::now();
        manager.shutdown();
        PythonInterpreter::instance().shutdown();
        const auto elapsed = std::chrono::steady_clock::now() - start;
        worker.join();
        check(elapsed >= std::chrono::milliseconds(200), "interpreter finalized with a live runtime lease");
        check(!PythonInterpreter::instance().acquire_runtime_lease(), "lease issued after shutdown");
    } else
        throw std::runtime_error("unknown scenario");
}
} // namespace
int main(int argc, char** argv)
{
    boost::nowide::args utf8_args(argc, argv);
    boost::nowide::nowide_filesystem();
    if (argc != 4)
        return 2;
    const auto data = test_fs::u8path(argv[3]);
    if (test_fs::exists(data) && std::string(argv[1]) != "replacement_recover" && std::string(argv[1]) != "profile_probe")
        return 3;
    test_fs::create_directories(data);
    set_data_dir(data.u8string());
    boost::log::core::get()->set_logging_enabled(false);
    auto& manager = PluginManager::instance();
    int   result  = 0;
    try {
        if (std::string(argv[1]) == "profile_probe") {
            const bool initialized = manager.initialize();
            manager.shutdown();
            PythonInterpreter::instance().shutdown();
            if (!initialized) { std::cout << "PASS second profile owner rejected\n"; return 0; }
            return 1;
        }
        check(manager.initialize(), PythonInterpreter::instance().last_error());
        test(argv[1], test_fs::u8path(argv[2]), data);
        std::cout << "PASS " << argv[1] << '\n';
    } catch (const std::exception& ex) {
        std::cerr << "FAIL " << argv[1] << ": " << ex.what() << '\n';
        result = 1;
    }
    manager.shutdown();
    PythonInterpreter::instance().shutdown();
    return result;
}
