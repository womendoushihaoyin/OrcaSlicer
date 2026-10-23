#pragma once

#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <utility>

namespace Slic3r {
class Print;
class PrintObject;

// Names/order follow Orca 14a1aa333b6da3ecadf853a4a6285b0f193b7605.
// posContouring runs after the real Z-contour kernel, when contouring is enabled.
// psGCodePostProcess is emitted by GCode/PostProcessor, outside Print::process().
// Curled-extrusion estimation emits only after its completed or empty-work seam.
enum class SlicingPipelineStepPlugin {
    posSlice, posPerimeters, posEstimateCurledExtrusions, posPrepareInfill, posInfill, posIroning, posContouring,
    posSupportMaterial, posDetectOverhangsForLift, posSimplifyPath, psWipeTower, psSkirtBrim, psGCodePostProcess
};
using SlicingPipelineHookFn = std::function<void(Print&, const PrintObject*, SlicingPipelineStepPlugin)>;
struct SlicingPipelinePlan {
    SlicingPipelineHookFn fn;
    std::string signature;
};
using SlicingPipelineHookFactory = std::function<SlicingPipelinePlan(Print&)>;

namespace detail {
struct SlicingPipelineHookState {
    std::mutex registration_mutex;
    std::mutex mutex;
    std::condition_variable cv;
    SlicingPipelineHookFactory factory;
    size_t active_processes = 0;
};
inline SlicingPipelineHookState& slicing_pipeline_hook_state()
{
    static SlicingPipelineHookState state;
    return state;
}
} // namespace detail

// A factory resolves and pins the preset's capabilities once per complete process
// pass. Detach drains even a pass currently between callbacks. Never register or
// detach from a callback; shutdown must first cancel/join the slicing worker.
inline void set_slicing_pipeline_hook_factory(SlicingPipelineHookFactory factory)
{
    auto& state = detail::slicing_pipeline_hook_state();
    std::lock_guard<std::mutex> registration_lock(state.registration_mutex);
    std::unique_lock<std::mutex> lock(state.mutex);
    state.factory = nullptr;
    state.cv.wait(lock, [&] { return state.active_processes == 0; });
    state.factory = std::move(factory);
}

inline void set_slicing_pipeline_hook_fn(SlicingPipelineHookFn fn)
{
    set_slicing_pipeline_hook_factory(fn ? SlicingPipelineHookFactory([fn = std::move(fn)](Print&) { return SlicingPipelinePlan{fn, {}}; }) : nullptr);
}

class SlicingPipelineLease {
public:
    explicit SlicingPipelineLease(Print& print)
    {
        SlicingPipelineHookFactory factory;
        auto& state = detail::slicing_pipeline_hook_state();
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            factory = state.factory;
            if (factory) {
                ++state.active_processes;
                m_acquired = true;
            }
        }
        // Never acquire Python/registry locks while holding the core hook lock.
        try {
            if (factory) {
                auto plan = factory(print);
                m_fn = std::move(plan.fn);
                m_signature = std::move(plan.signature);
            }
        }
        catch (...) { factory = nullptr; release(); throw; }
    }
    ~SlicingPipelineLease() { release(); }
    SlicingPipelineLease(const SlicingPipelineLease&) = delete;
    SlicingPipelineLease& operator=(const SlicingPipelineLease&) = delete;
    explicit operator bool() const { return bool(m_fn); }
    const std::string& signature() const { return m_signature; }
    void operator()(Print& print, const PrintObject* object, SlicingPipelineStepPlugin step) const { m_fn(print, object, step); }

private:
    void release()
    {
        if (!m_acquired) return;
        // Release captured consumers before announcing that teardown is safe.
        m_fn = nullptr;
        auto& state = detail::slicing_pipeline_hook_state();
        std::lock_guard<std::mutex> lock(state.mutex);
        --state.active_processes;
        m_acquired = false;
        state.cv.notify_all();
    }
    bool m_acquired = false;
    SlicingPipelineHookFn m_fn;
    std::string m_signature;
};
} // namespace Slic3r
