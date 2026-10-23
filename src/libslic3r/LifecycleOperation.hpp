#pragma once
#include "LifecycleEvents.hpp"

namespace Slic3r {
// Pair an actual operation with exactly one terminal event, including exceptions
// outside its usual inner try/catch (opening a file, processing, final rename).
// A cancelled Print's probe suppresses dispatch; the GUI owns its Warn job event.
class LifecycleOperation {
public:
    LifecycleOperation(LifecycleEvent start, LifecycleEvent finish, LifecycleEventContext context)
        : m_finish(finish), m_context(std::move(context)) { fire_lifecycle_event(start, m_context); }
    ~LifecycleOperation()
    {
        if (!m_finished) {
            m_context.code = LifecycleEvtCode::Error;
            try { fire_lifecycle_event(m_finish, m_context); } catch (...) {}
        }
    }
    LifecycleOperation(const LifecycleOperation&) = delete;
    LifecycleOperation& operator=(const LifecycleOperation&) = delete;
    void finish()
    {
        m_finished = true;
        fire_lifecycle_event(m_finish, m_context);
    }
private:
    LifecycleEvent m_finish;
    LifecycleEventContext m_context;
    bool m_finished = false;
};
} // namespace Slic3r
