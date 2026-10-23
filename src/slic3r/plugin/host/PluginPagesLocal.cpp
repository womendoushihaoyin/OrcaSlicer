#include "PluginPagesLocal.hpp"
#include "slic3r/plugin/PluginManager.hpp"
#include "slic3r/plugin/pluginTypes/pages/PagesPluginCapability.hpp"
#include "slic3r/GUI/Notebook.hpp"
#include "slic3r/GUI/Widgets/WebView.hpp"
#include "slic3r/GUI/WebPanel.hpp"
#include "slic3r/GUI/wxExtensions.hpp"
#include <boost/filesystem/path.hpp>
#include <boost/log/trivial.hpp>
#include <wx/panel.h>
#include <wx/sizer.h>
#include <wx/webview.h>
#include <wx/app.h>
#include <wx/weakref.h>
#include <map>
#include <atomic>

namespace Slic3r {
namespace {
// The upstream Pages protocol, independent from Snapmaker's SSWCP WebViews.
constexpr char bridge[] = R"JS(
(function () {
  if (window.top !== window.self || window.orca) return;
  var handlers = [];
  function deliver(payload, attempts) {
    if (window.wx && typeof window.wx.postMessage === 'function') { window.wx.postMessage(payload); return; }
    if (attempts < 100) setTimeout(function () { deliver(payload, attempts + 1); }, 25);
  }
  window.orca = {
    postMessage: function (data) { deliver(JSON.stringify({channel:'orca', kind:'message', data:data === undefined ? null : data}), 0); },
    onMessage: function (fn) { if (typeof fn === 'function') handlers.push(fn); }
  };
  window.__orcaDispatch = function (payload) {
    handlers.forEach(function (fn) { try { fn(payload.data); } catch (e) {} });
  };
})();
)JS";

class LocalPluginPage final : public GUI::WebPanel
{
public:
    LocalPluginPage(Notebook* parent, std::shared_ptr<PagesPluginCapability> cap)
        : GUI::WebPanel(parent, bridge), m_cap(std::move(cap)), m_lifetime(std::make_shared<std::atomic<LocalPluginPage*>>(this))
    {
        if (!browser())
            throw std::runtime_error("No WebView backend is available for plugin pages");
        browser()->Bind(wxEVT_WEBVIEW_NEWWINDOW, [](wxWebViewEvent& event) { event.Veto(); });
        const auto lifetime = m_lifetime;
        m_cap->set_message_sender([lifetime](const std::string& message) {
            if (!wxTheApp)
                return;
            wxTheApp->CallAfter([lifetime, message] {
                if (auto* page = lifetime->load(std::memory_order_acquire))
                    page->send(message);
            });
        });
    }
    ~LocalPluginPage() override { detach(); }
    bool owns(const PluginCapabilityInterface* capability) const { return m_cap.get() == capability; }
    void detach()
    {
        m_lifetime->store(nullptr, std::memory_order_release);
        if (m_cap)
            m_cap->clear_message_sender();
        m_cap.reset();
    }

protected:
    std::optional<std::string> page_html() override
    {
        const auto cap = acquire_current();
        if (!cap)
            return std::nullopt;
        try {
            return cap->get_ui();
        } catch (const std::exception& ex) {
            PluginManager::instance().set_plugin_error(cap->audit_plugin_key(), std::string("Plugin page: ") + ex.what());
            return std::nullopt;
        }
    }
    bool on_page_message(const std::string& kind, const nlohmann::json& data) override
    {
        const auto cap = acquire_current();
        if (kind != "message" || !cap)
            return false;
        try {
            cap->on_message(data.dump());
        } catch (const std::exception& ex) {
            BOOST_LOG_TRIVIAL(error) << "Plugin page message failed: " << ex.what();
        }
        return true;
    }

private:
    std::shared_ptr<PagesPluginCapability> acquire_current() const
    {
        if (!m_cap) return {};
        std::string error;
        auto pinned = PluginManager::instance().acquire_capability(m_cap->identity(), error);
        if (pinned.get() != m_cap.get()) return {};
        return std::dynamic_pointer_cast<PagesPluginCapability>(std::move(pinned));
    }
    void send(const std::string& message)
    {
        const auto cap = acquire_current();
        if (cap)
            post_to_page(nlohmann::json::accept(message) ? message : nlohmann::json(message).dump());
    }
    std::shared_ptr<PagesPluginCapability>         m_cap;
    std::shared_ptr<std::atomic<LocalPluginPage*>> m_lifetime;
};

// Keep the public get_icon callback and fixed-upstream SVG/PNG resource-name
// semantics while using Snapmaker's existing notebook buttons. A broken icon
// must not prevent the page and its message bridge from being usable.
std::string page_icon(const std::shared_ptr<PluginCapabilityInterface>& capability, Notebook* parent)
{
    try {
        std::string error;
        const auto pinned = PluginManager::instance().acquire_capability(capability->identity(), error);
        if (pinned.get() != capability.get()) return {};
        const auto page = std::dynamic_pointer_cast<PagesPluginCapability>(pinned);
        if (!page) return {};
        const auto icon = page->get_icon();
        if (icon.empty()) return {};
        boost::filesystem::path path(icon);
        const auto extension = path.extension().string();
        if (extension == ".svg" || extension == ".png") path.replace_extension();
        const auto name = path.string();
        if (create_scaled_bitmap(name, parent, 20).IsOk()) return name;
    } catch (const std::exception& ex) {
        BOOST_LOG_TRIVIAL(warning) << "Plugin page icon: " << ex.what();
    } catch (...) {
        BOOST_LOG_TRIVIAL(warning) << "Plugin page icon failed";
    }
    return {};
}

struct Pages
{
    wxWeakRef<Notebook>                                      notebook;
    std::map<PluginCapabilityId, wxWeakRef<LocalPluginPage>> pages;
    std::shared_ptr<bool>                                    alive = std::make_shared<bool>(true);
    void                                                     refresh()
    {
        if (!*alive || !notebook)
            return;
        auto& manager = PluginManager::instance();
        for (auto it = pages.begin(); it != pages.end();) {
            const auto capability = manager.get_plugin_capability(it->first);
            // A rapid unload/reload reuses the id, but never the Python instance.
            if (!capability || !it->second || !it->second->owns(capability.get())) {
                if (it->second) {
                    auto* page = it->second.get();
                    page->detach();
                    const auto index = notebook->FindPage(page);
                    if (index != wxNOT_FOUND) {
                        if (notebook->GetSelection() == index)
                            notebook->SetSelection(0);
                        notebook->DeletePage(index);
                    }
                }
                it = pages.erase(it);
            } else
                ++it;
        }
        for (const auto& cap : manager.get_plugin_capabilities("", PluginCapabilityType::Pages)) {
            const auto id = cap->identity();
            if (pages.count(id))
                continue;
            LocalPluginPage* page = nullptr;
            try {
                page = new LocalPluginPage(notebook.get(), std::dynamic_pointer_cast<PagesPluginCapability>(cap));
                // Snapmaker's icon-aware InsertPage(false) does not hide the
                // new wxPanel. Keep it from covering the current tab before
                // selection has sized and shown the plugin page.
                page->Hide();
                const auto icon = page_icon(cap, notebook.get());
                if (!notebook->AddPage(page, wxString::FromUTF8(cap->name()), icon, icon, false))
                    throw std::runtime_error("Cannot attach plugin page to the main notebook");
                pages.emplace(id, page);
            } catch (const std::exception& ex) {
                if (page) {
                    page->detach();
                    page->Destroy();
                }
                manager.set_plugin_error(id.plugin_key, std::string("Plugin page: ") + ex.what());
            }
        }
        notebook->Layout();
    }
    void shutdown()
    {
        *alive = false;
        for (auto& entry : pages) {
            if (!entry.second)
                continue;
            auto* page = entry.second.get();
            page->detach();
            if (notebook) {
                const auto index = notebook->FindPage(page);
                if (index != wxNOT_FOUND) {
                    if (notebook->GetSelection() == index)
                        notebook->SetSelection(0);
                    notebook->DeletePage(index);
                }
            }
        }
        pages.clear();
        notebook = nullptr;
    }
};
std::unique_ptr<Pages> pages;
} // namespace
void initialize_local_plugin_pages(Notebook* notebook)
{
    shutdown_local_plugin_pages();
    if (!notebook)
        return;
    pages           = std::make_unique<Pages>();
    pages->notebook = notebook;
    const std::weak_ptr<bool> alive(pages->alive);
    auto                      refresh = [alive](const auto&) {
        const auto token = alive.lock();
        if (token && *token && pages)
            pages->refresh();
    };
    auto& manager = PluginManager::instance();
    manager.subscribe_on_load_callback(refresh);
    manager.subscribe_on_unload_callback(refresh);
    manager.subscribe_on_capability_load_callback(refresh);
    manager.subscribe_on_capability_unload_callback(refresh);
    pages->refresh();
}
bool select_local_plugin_page(const PluginCapabilityId& id)
{
    if (!pages || !pages->notebook)
        return false;
    const auto it = pages->pages.find(id);
    if (it == pages->pages.end() || !it->second)
        return false;
    const auto index = pages->notebook->FindPage(it->second.get());
    if (index == wxNOT_FOUND)
        return false;
    pages->notebook->SetSelection(index);
    return pages->notebook->GetSelection() == index;
}
void shutdown_local_plugin_pages()
{
    if (pages)
        pages->shutdown();
    pages.reset();
}
} // namespace Slic3r
