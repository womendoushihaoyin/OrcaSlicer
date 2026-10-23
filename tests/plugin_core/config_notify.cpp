#include "slic3r/plugin/PluginManager.hpp"
#include "slic3r/plugin/PluginSlicingInvalidation.hpp"
#include "slic3r/plugin/PythonInterpreter.hpp"
#include <filesystem>
#include <stdexcept>

using namespace Slic3r;
namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
}

void test_config_notify()
{
    auto& config = PluginManager::instance().get_config();
    const PluginCapabilityId id{PluginCapabilityType::SlicingPipeline, "Height", "notify"};
    const PluginCapabilityId other{PluginCapabilityType::SlicingPipeline, "Height", "other"};
    unsigned calls = 0;
    bool callback_read_ok = true, gil_free = true, nested_ok = true;
    std::vector<PluginCapabilityId> delivered;
    const auto token = config.subscribe_on_change([&](const auto& changed) {
        ++calls;
        delivered = changed;
        gil_free &= !PyGILState_Check();
        // Re-entering reads, writes, and subscription changes must not hold the config mutex.
        callback_read_ok &= !config.dirty();
        auto temporary = config.subscribe_on_change([](const auto&) {});
        config.unsubscribe_on_change(temporary);
        if (changed.size() == 1 && changed.front() == id) {
            const auto value = config.get_config(id);
            if (value) nested_ok &= config.store_capability_config(id, value->config);
        }
    });
    require(config.store_capability_config(id, {{"height", 1}}), "first commit failed");
    require(calls == 1 && delivered == std::vector<PluginCapabilityId>{id}, "wrong committed identity notification");
    require(config.store_capability_config(id, {{"height", 1}}) && calls == 1, "unchanged value notified");
    {
        PythonGILState gil;
        require(bool(gil), "test runtime unavailable");
        require(config.store_capability_config(id, {{"height", 2}}), "GIL-held save failed");
        require(PyGILState_Check(), "observer did not restore caller GIL");
    }
    require(calls == 2 && gil_free && callback_read_ok && nested_ok, "observer held GIL/config lock or reentry failed");
    config.save_config({id, "version-only", {{"height", 2}}});
    require(config.save() && calls == 2, "version-only change notified");
    config.save_config({id, "version-only", {{"height", 3}}});
    require(calls == 2 && config.dirty(), "staged unsaved config notified");
    require(config.save() && calls == 3, "deferred save did not notify");
    const auto snapshot = config.snapshot_plugin_config(id.plugin_key);
    require(config.store_capability_config(other, {{"height", 9}}), "unrelated save failed");
    require(config.store_capability_config(id, {{"height", 4}}), "second save failed");
    const auto before_restore = calls;
    require(config.restore_plugin_config(id.plugin_key, snapshot) && calls == before_restore + 1,
            "rollback did not notify changed package");
    require(config.get_config(other)->config.at("height") == 9, "rollback changed another package");
    const auto config_path = std::filesystem::u8path(PluginConfig::plugin_config_file());
    const auto saved_path = config_path.string() + ".notify-saved";
    std::filesystem::rename(config_path, saved_path);
    std::filesystem::create_directory(config_path);
    const auto before_failure = calls;
    const bool failed = !config.store_capability_config(id, {{"height", 100}});
    std::filesystem::remove(config_path);
    std::filesystem::rename(saved_path, config_path);
    require(failed && calls == before_failure && config.get_config(id)->config.at("height") == 3,
            "failed commit changed memory or notified");
    require(config.erase_capability_config(id) && calls == before_failure + 1, "erase did not notify");
    require(config.erase_capability_config(id) && calls == before_failure + 1, "absent erase notified");
    config.unsubscribe_on_change(token);
    require(config.store_capability_config(id, {{"height", 6}}) && calls == before_failure + 1, "unsubscribe failed");

    DynamicPrintConfig print;
    print.set_key_value("slicing_pipeline_plugin", new ConfigOptionStrings({"Height"}));
    print.set_key_value("plugins", new ConfigOptionStrings({"notify;;Height", "other;;Unused"}));
    require(slicing_plugins_affected(print, {{"notify", "Height", true}}), "selected package was not matched");
    require(!slicing_plugins_affected(print, {{"other", "Height", false}}), "same-named unrelated package matched");
    require(!slicing_plugins_affected(print, {{"notify", "Unused", false}}), "unselected capability matched");
    require(slicing_plugins_affected(print, {{"notify", {}, false}}), "package unload was not matched");
    CapabilityConfigDocument overrides;
    overrides.upsert({id, {}, {{"height", 7}}});
    print.set_key_value("print_plugin_config_overrides", new ConfigOptionString(serialize_plugin_overrides(overrides)));
    require(!slicing_plugins_affected(print, {{"notify", "Height", true}}), "base change invalidated preset override");
    require(slicing_plugins_affected(print, {{"notify", "Height", false}}), "reload failed to invalidate preset override");
    print.set_key_value("plugins", new ConfigOptionStrings({"DisplayName;uuid-123;Height"}));
    require(slicing_plugins_affected(print, {{"uuid-123", "Height", false}}), "UUID identity was not matched");
    require(!slicing_plugins_affected(print, {{"DisplayName", "Height", false}}), "display name incorrectly matched UUID package");
    print.set_key_value("slicing_pipeline_plugin", new ConfigOptionStrings());
    require(!slicing_plugins_affected(print, {{"uuid-123", {}, false}}), "unused manifest invalidated plate");
}

#include "libslic3r/Print.hpp"
#include "libslic3r/SlicingPipeline.hpp"
#include <atomic>
#include <chrono>
#include <future>
#include <thread>

// Exercise actual Print::process on a worker. The headless harness has no wx MainFrame;
// this callback mirrors BackgroundSlicingProcess::stop_internal's lock handoff protocol.
void test_slicing_invalidation()
{
    class TestPrint : public Print {
    public:
        std::mutex& worker_state_mutex() { return state_mutex(); }
    };
    struct Detach { ~Detach() { set_slicing_pipeline_hook_factory(nullptr); } } detach;
    Model model;
    auto* object = model.add_object();
    object->add_volume(TriangleMesh(make_cube(10., 10., 1.)));
    object->add_instance();
    object->ensure_on_bed();
    DynamicPrintConfig config;
    const auto& defaults = FullPrintConfig::defaults();
    config.apply(static_cast<const PrintObjectConfig&>(defaults), true);
    config.apply(static_cast<const PrintRegionConfig&>(defaults), true);
    config.apply(static_cast<const PrintConfig&>(defaults), true);
    TestPrint print;
    print.apply(model, config);
    print.set_status_silent();
    for (const bool before_first_milestone : {true, false}) {
        print.restart();
        std::promise<void> entered;
        auto entered_future = entered.get_future();
        std::atomic<bool> canceled{false};
        const auto block_until_cancel = [&](Print& active) {
            entered.set_value();
            while (!active.canceled()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            throw CanceledException();
        };
        set_slicing_pipeline_hook_factory([&](Print& active) {
            if (before_first_milestone) block_until_cancel(active);
            return SlicingPipelinePlan{[&](Print& active, const PrintObject*, SlicingPipelineStepPlugin step) {
                if (step == SlicingPipelineStepPlugin::posSlice) block_until_cancel(active);
            }, {}};
        });
        auto worker = std::async(std::launch::async, [&] {
            try { print.process(); }
            catch (const CanceledException&) { canceled = true; }
        });
        if (entered_future.wait_for(std::chrono::seconds(15)) != std::future_status::ready) {
            print.cancel();
            worker.get();
            throw std::runtime_error("real slicing worker never reached cancellation fixture");
        }
        unsigned cancellation_calls = 0;
        print.set_cancel_callback([&] {
            ++cancellation_calls;
            print.cancel_internal();
            print.worker_state_mutex().unlock();
            const auto status = worker.wait_for(std::chrono::seconds(15));
            print.worker_state_mutex().lock();
            print.set_cancel_callback([] {});
            require(status == std::future_status::ready, "worker cancellation deadlocked on Print state mutex");
        });
        print.invalidate_plugin_state();
        worker.get();
        require(canceled && cancellation_calls == 1, "invalidation did not cancel the real worker exactly once");
        require(!print.get_object(0)->is_step_done(posSlice) && !print.is_step_done(psSlicingFinished),
                "canceled plugin milestones remained reusable");
    }
    size_t hooks = 0;
    set_slicing_pipeline_hook_fn([&](Print&, const PrintObject*, SlicingPipelineStepPlugin) { ++hooks; });
    print.restart();
    print.process();
    require(print.get_object(0)->is_step_done(posSlice) && hooks > 5, "invalidated print did not slice successfully again");
    const auto first_hooks = hooks;
    print.invalidate_plugin_state();
    require(!print.get_object(0)->is_step_done(posSlice) && !print.is_step_done(psSlicingFinished),
            "completed result survived plugin invalidation");
    print.restart();
    print.process();
    require(hooks >= 2 * first_hooks, "re-slicing reused invalidated plugin output");
}
