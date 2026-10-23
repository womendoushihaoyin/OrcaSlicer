#include "PluginSlicingGui.hpp"
#include "Plater.hpp"
#include "NotificationManager.hpp"
#include "I18N.hpp"
#include <boost/log/trivial.hpp>
#include "slic3r/plugin/PluginManager.hpp"
#include "slic3r/plugin/PluginSlicingInvalidation.hpp"
#include "slic3r/plugin/PythonInterpreter.hpp"
#include <wx/app.h>
#include <wx/weakref.h>
#include <atomic>
#include <memory>
#include <mutex>

namespace Slic3r::GUI {
namespace {
struct QueueState {
    std::atomic<bool> alive{true};
    std::mutex mutex;
    std::vector<PluginSlicingChange> pending;
    bool posted = false;
};
struct Bridge {
    explicit Bridge(Plater* target) : plater(target), queue(std::make_shared<QueueState>()) {}
    // Only the UI-owned Bridge contains a wxWeakRef: its tracker must also be destroyed on UI.
    wxWeakRef<Plater> plater;
    std::shared_ptr<QueueState> queue;
    PluginConfig::Subscription subscription = 0;
};
std::unique_ptr<Bridge> bridge;
void enqueue(const std::weak_ptr<QueueState>& weak, std::vector<PluginSlicingChange> changes)
{
    const auto state = weak.lock();
    if (!state || !state->alive.load() || changes.empty()) return;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->pending.insert(state->pending.end(), std::make_move_iterator(changes.begin()), std::make_move_iterator(changes.end()));
        if (state->posted) return;
        state->posted = true;
    }
    // Lifecycle callbacks can arrive on a loader thread. They only copy identities and enqueue;
    // cancellation/restart is always later, outside the Manager lock and Python callback/GIL.
    // GUI shutdown disables this queue before it joins loader threads, while wxApp is still alive.
    if (auto* app = wxApp::GetInstance()) app->CallAfter([weak] {
        const auto state = weak.lock();
        if (!state || !state->alive.load() || !bridge || bridge->queue != state || !bridge->plater) return;
        std::vector<PluginSlicingChange> pending;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            pending.swap(state->pending);
            state->posted = false;
        }
        try {
            auto runtime = PythonInterpreter::instance().acquire_runtime_lease();
            if (runtime && PyGILState_Check()) {
                pybind11::gil_scoped_release release;
                bridge->plater->invalidate_slicing_plugins(pending);
            } else {
                bridge->plater->invalidate_slicing_plugins(pending);
            }
            return;
        } catch (const std::exception& ex) {
            BOOST_LOG_TRIVIAL(error) << "Plugin slicing update failed: " << ex.what();
        } catch (...) {
            BOOST_LOG_TRIVIAL(error) << "Plugin slicing update failed";
        }
        // Never propagate an observer exception through the wx event loop.
        try {
            if (bridge && bridge->queue == state && bridge->plater)
                bridge->plater->get_notification_manager()->push_notification(
                    _u8L("Plugin settings changed, but the slicing update failed. Slice again before exporting."));
        } catch (...) {}
    });
}
}
void initialize_plugin_slicing_gui(Plater* plater)
{
    if (!plater || bridge) return;
    bridge = std::make_unique<Bridge>(plater);
    const std::weak_ptr<QueueState> weak = bridge->queue;
    auto& manager = PluginManager::instance();
    bridge->subscription = manager.get_config().subscribe_on_change([weak](const auto& ids) {
        std::vector<PluginSlicingChange> changes;
        for (const auto& id : ids)
            if (id.type == PluginCapabilityType::SlicingPipeline || id.type == PluginCapabilityType::Unknown)
                changes.push_back({id.plugin_key, id.name, true});
        enqueue(weak, std::move(changes));
    });
    const auto package_changed = [weak](const std::string& key) { enqueue(weak, {{key, {}, false}}); };
    const auto capability_changed = [weak](const PluginCapabilityId& id) {
        if (id.type == PluginCapabilityType::SlicingPipeline)
            enqueue(weak, {{id.plugin_key, id.name, false}});
    };
    manager.subscribe_on_load_callback(package_changed);
    manager.subscribe_on_unload_callback(package_changed);
    manager.subscribe_on_capability_load_callback(capability_changed);
    manager.subscribe_on_capability_unload_callback(capability_changed);
    // Also invalidate a project opened before the runtime was initialized.
    std::vector<PluginSlicingChange> loaded;
    for (const auto& descriptor : manager.get_plugin_descriptors())
        loaded.push_back({descriptor.plugin_key, {}, false});
    enqueue(weak, std::move(loaded));
}
void shutdown_plugin_slicing_gui()
{
    if (!bridge) return;
    bridge->queue->alive.store(false);
    PluginManager::instance().get_config().unsubscribe_on_change(bridge->subscription);
    bridge.reset();
}
} // namespace Slic3r::GUI
