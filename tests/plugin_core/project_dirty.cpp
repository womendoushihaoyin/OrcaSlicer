#include "slic3r/GUI/ProjectDirtyStateManager.hpp"
#include "libslic3r/LifecycleEvents.hpp"
#include <stdexcept>
#include <vector>

using namespace Slic3r;
void test_project_dirty()
{
    GUI::ProjectDirtyStateManager state;
    std::vector<bool> events;
    set_lifecycle_hook_fn([&](LifecycleEvent event, const LifecycleEventContext& context) {
        if (event == LifecycleEvent::ProjectDirtyChanged) events.push_back(context.dirty);
    });
    struct Detach { ~Detach() { set_lifecycle_hook_fn(nullptr); } } detach;
    auto require = [](bool ok) { if (!ok) throw std::runtime_error("project dirty lifecycle coalescing failed"); };
    state.set_plater_dirty(true);
    state.set_plater_dirty(true);
    require(events == std::vector<bool>{true});
    {
        GUI::ProjectDirtyStateManager::NotificationSuppressor batch(state);
        state.set_plater_dirty(false);
        {
            GUI::ProjectDirtyStateManager::NotificationSuppressor nested(state);
            state.set_plater_dirty(true);
        }
        require(events.size() == 1);
    }
    require(events.size() == 1);
    try {
        GUI::ProjectDirtyStateManager::NotificationSuppressor batch(state);
        state.set_plater_dirty(false);
        throw std::runtime_error("fixture unwind");
    } catch (const std::runtime_error&) {}
    require(events == std::vector<bool>({true, false}) && !state.is_dirty());
}
