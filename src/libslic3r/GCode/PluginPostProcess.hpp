#pragma once

#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <utility>

namespace Slic3r {
class DynamicPrintConfig;
// The factory snapshots configuration and pins all selected consumers before any
// script or plugin touches the working file. No Python dependency in libslic3r.
using GCodePluginHookFn = std::function<void(const std::string&, const std::string&, const std::string&)>;
using GCodePluginHookFactory = std::function<GCodePluginHookFn(const DynamicPrintConfig&)>;

namespace detail {
struct GCodePluginHookState {
    std::mutex registration_mutex;
    std::mutex mutex;
    std::condition_variable cv;
    GCodePluginHookFactory factory;
    size_t active_processes = 0;
};
inline GCodePluginHookState& gcode_plugin_hook_state()
{
    static GCodePluginHookState state;
    return state;
}
} // namespace detail

// A factory resolves and pins the preset's capabilities once per complete post-process
// pass. Detach drains even a pass currently between callbacks. Never register or
// detach from a callback; shutdown must first cancel/join the export worker.
inline void set_gcode_plugin_hook_factory(GCodePluginHookFactory factory)
{
    auto& state = detail::gcode_plugin_hook_state();
    std::lock_guard<std::mutex> registration_lock(state.registration_mutex);
    std::unique_lock<std::mutex> lock(state.mutex);
    state.factory = nullptr;
    state.cv.wait(lock, [&] { return state.active_processes == 0; });
    state.factory = std::move(factory);
}

class GCodePluginLease {
public:
    explicit GCodePluginLease(const DynamicPrintConfig& config)
    {
        GCodePluginHookFactory factory;
        auto& state = detail::gcode_plugin_hook_state();
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
                m_fn = factory(config);
            }
        }
        catch (...) { factory = nullptr; release(); throw; }
    }
    ~GCodePluginLease() { release(); }
    GCodePluginLease(const GCodePluginLease&) = delete;
    GCodePluginLease& operator=(const GCodePluginLease&) = delete;
    explicit operator bool() const { return bool(m_fn); }
    void operator()(const std::string& path, const std::string& host, const std::string& name) const { m_fn(path, host, name); }

private:
    void release()
    {
        if (!m_acquired) return;
        // Release captured consumers before announcing that teardown is safe.
        m_fn = nullptr;
        auto& state = detail::gcode_plugin_hook_state();
        std::lock_guard<std::mutex> lock(state.mutex);
        --state.active_processes;
        m_acquired = false;
        state.cv.notify_all();
    }
    bool m_acquired = false;
    GCodePluginHookFn m_fn;
};
} // namespace Slic3r
