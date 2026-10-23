#include "LocalPluginRuntime.hpp"
#include "PluginLoader.hpp"
#include "PluginManager.hpp"
#include "PythonInterpreter.hpp"
#include "host/PluginHostCore.hpp"
#include "host/PluginPagesLocal.hpp"
#include "slic3r/GUI/I18N.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <boost/log/trivial.hpp>
#include <wx/app.h>
#include <wx/dialog.h>
#include <wx/evtloop.h>
#include <wx/modalhook.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/thread.h>
#include <wx/timer.h>
#include <wx/weakref.h>

namespace Slic3r {
namespace {
// A real modal loop keeps WebKit and marshalled permission requests running
// while disabling input to the caller. Do not use selective YieldFor here:
// wx's delayed-event list can be reentered by the macOS CF run-loop observer,
// leaving its global pending-event lock held after wxCHECK_RET returns.
class PluginLoadDialog final : public wxDialog, private wxModalDialogHook
{
public:
    PluginLoadDialog(wxWindow* parent, const std::string& key)
        : wxDialog(nullptr, wxID_ANY, _L("Loading plugin"), wxDefaultPosition, wxDefaultSize, wxCAPTION | wxDIALOG_NO_PARENT),
          m_key(key), m_timer(this), m_parent(parent), m_caller_loop(wxEventLoopBase::GetActive())
    {
        auto* layout = new wxBoxSizer(wxVERTICAL);
        layout->Add(new wxStaticText(this, wxID_ANY, _L("Loading plugin") + ": " + wxString::FromUTF8(key)),
                    0, wxALL, 24);
        SetSizerAndFit(layout);
        SetEscapeId(wxID_NONE);
        Bind(wxEVT_CLOSE_WINDOW, &PluginLoadDialog::on_close, this);
        Bind(wxEVT_TIMER, [this](wxTimerEvent&) {
            if (!m_loop) {
                // Start only after ShowModal has entered its own event loop.
                // Capture it before a load observer or permission callback can
                // enter another modal loop on this same thread.
                if (wxEventLoopBase::GetActive() == m_caller_loop)
                    return;
                m_loop = wxEventLoopBase::GetActive();
                PluginManager::instance().load_plugin(m_key);
            }
            // Load observers are queued before the worker releases its slot.
            // Closing from an observer could return while it still owns Python.
            m_nested.erase(std::remove_if(m_nested.begin(), m_nested.end(), [](const auto& dialog) { return !dialog; }), m_nested.end());
            // WKWebView's handler registration uses synchronous wxYield inside
            // this same loop. Exiting during that yield prevents its JavaScript
            // completion from being dispatched and leaves it spinning forever.
            if (m_nested.empty() && wxEventLoopBase::GetActive() == m_loop && !m_loop->IsYielding() &&
                !PluginManager::instance().is_plugin_load_in_progress(m_key)) {
                m_timer.Stop();
                EndModal(wxID_OK);
            }
        }, m_timer.GetId());
        if (m_parent)
            m_parent->Bind(wxEVT_CLOSE_WINDOW, &PluginLoadDialog::on_close, this);
        Register();
        m_timer.Start(25);
        Centre();
    }
    ~PluginLoadDialog() override
    {
        Unregister();
        m_timer.Stop();
        if (m_parent)
            m_parent->Unbind(wxEVT_CLOSE_WINDOW, &PluginLoadDialog::on_close, this);
    }

private:
    // macOS wxMessageDialog runs an NSAlert without changing the active wx
    // loop. Track modal entry/exit too, so EndModal cannot abort that alert
    // after an asynchronous audit request has already failed the worker.
    int Enter(wxDialog* dialog) override
    {
        if (dialog != this) m_nested.emplace_back(dialog);
        return wxID_NONE;
    }
    void Exit(wxDialog* dialog) override
    {
        m_nested.erase(std::remove_if(m_nested.begin(), m_nested.end(),
            [dialog](const auto& nested) { return !nested || nested.get() == dialog; }), m_nested.end());
    }
    void on_close(wxCloseEvent& event)
    {
        // An explicit app Close() can arrive even while its windows are disabled.
        // Keep the caller and its stack alive until the loader has drained.
        if (event.CanVeto()) event.Veto();
        else event.Skip();
    }
    std::string m_key;
    wxTimer m_timer;
    wxWeakRef<wxWindow> m_parent;
    wxEventLoopBase* m_caller_loop;
    wxEventLoopBase* m_loop = nullptr;
    std::vector<wxWeakRef<wxDialog>> m_nested;
};
} // namespace
struct LocalPluginRuntime::Impl
{
    bool            active = false;
    std::thread::id owner;
    std::atomic<bool> loading{false};
};
LocalPluginRuntime::LocalPluginRuntime() : m_impl(std::make_unique<Impl>()) {}
LocalPluginRuntime::~LocalPluginRuntime()
{
    // Explicit shutdown rejects reentry from a plugin teardown. Destruction is
    // noexcept: retain the manager/interpreter for their outer shutdown instead
    // of letting that rejection terminate the application.
    try { shutdown(); }
    catch (const std::exception& ex) {
        BOOST_LOG_TRIVIAL(error) << "Plugin runtime destructor deferred shutdown: " << ex.what();
    }
    catch (...) { BOOST_LOG_TRIVIAL(error) << "Plugin runtime destructor deferred shutdown"; }
}
bool LocalPluginRuntime::initialize(const std::filesystem::path&, AppInfo provider, std::string& error)
{
    error.clear();
    if (m_impl->active)
        return true;
    m_impl->owner = std::this_thread::get_id();
    set_plugin_app_info_provider(std::move(provider));
    if (!PluginManager::instance().initialize()) {
        error = PythonInterpreter::instance().last_error();
        if (error.empty()) error = "Plugin profile is in use or recovery failed; check the application log";
        set_plugin_app_info_provider({});
        return false;
    }
    PluginManager::instance().discover_plugins();
    m_impl->active = true;
    return true;
}
bool LocalPluginRuntime::inspect(const std::filesystem::path& file, PluginDescriptor& descriptor, std::string& error)
{
    bool existing = false;
    return plugin_loader::inspect_local_plugin_package(boost::filesystem::path(file.u8string()), descriptor, existing, error);
}
bool LocalPluginRuntime::load(const std::filesystem::path& file, std::string& error)
{
    error.clear();
    if (!m_impl->active) {
        error = "Plugin runtime is not initialized";
        return false;
    }
    if (m_impl->loading.exchange(true)) {
        error = "Another local plugin load is in progress";
        return false;
    }
    struct LoadScope {
        std::atomic<bool>& loading;
        ~LoadScope() { loading.store(false); }
    } load_scope{m_impl->loading};
    auto& manager = PluginManager::instance();
    manager.rescan_plugins();
    for (const auto& descriptor : manager.get_plugin_descriptors()) {
        std::error_code ec;
        if (!std::filesystem::equivalent(file, std::filesystem::u8path(descriptor.entry_path), ec) || ec)
            continue;
        auto* app = dynamic_cast<wxApp*>(wxAppConsole::GetInstance());
        if (app && wxIsMainThread() && wxEventLoopBase::GetActive()) {
            PluginLoadDialog wait(app->GetTopWindow(), descriptor.plugin_key);
            wait.ShowModal();
        } else
            manager.load_plugin(descriptor.plugin_key);
        // No GUI queue exists in console/native tests. This also guarantees the
        // worker is drained if it completed before the modal dialog was opened.
        manager.wait_for_plugin_load(descriptor.plugin_key, std::chrono::milliseconds::max(), error);
        error = manager.get_plugin_load_error(descriptor.plugin_key);
        if (manager.is_plugin_loaded(descriptor.plugin_key))
            return true;
        if (error.empty())
            error = "Plugin load was cancelled";
        return false;
    }
    error = "Package must be installed before loading";
    return false;
}
bool LocalPluginRuntime::is_loaded(const std::string& key) const { return PluginManager::instance().is_plugin_loaded(key); }
bool LocalPluginRuntime::unload(const std::string& key, std::string& error)
{
    error.clear();
    if (PluginManager::instance().unload_plugin(key))
        return true;
    error = "Plugin cannot be unloaded while it is in use";
    return false;
}
std::vector<LocalPluginRuntime::Script> LocalPluginRuntime::scripts() const
{
    std::vector<Script> result;
    auto&               manager = PluginManager::instance();
    for (const auto& cap : manager.get_plugin_capabilities("", PluginCapabilityType::Script)) {
        PluginDescriptor descriptor;
        if (manager.try_get_plugin_descriptor(cap->audit_plugin_key(), descriptor))
            result.push_back({descriptor.plugin_key, cap->name(), descriptor.installed_version});
    }
    return result;
}
LocalPluginRuntime::Result LocalPluginRuntime::execute(const std::string& key, const std::string& name)
{
    std::string error;
    auto&       manager = PluginManager::instance();
    if (!manager.get_plugin_capability({PluginCapabilityType::Script, name, key}))
        return {false, "Script capability is not enabled", ""};
    const auto result = manager.run_script_capability(key, name, error);
    return {error.empty() && (result.status == PluginResult::Success || result.status == PluginResult::Skipped),
            error.empty() ? result.message : error, result.data};
}
void LocalPluginRuntime::shutdown()
{
    if (!m_impl->active)
        return;
    // Called on the initializing GUI thread after producers stop.
    shutdown_local_plugin_pages();
    PluginManager::instance().shutdown();
    PythonInterpreter::instance().shutdown();
    set_plugin_app_info_provider({});
    m_impl->active = false;
}
bool LocalPluginRuntime::initialized() const { return m_impl->active && PythonInterpreter::instance().is_initialized(); }
} // namespace Slic3r
