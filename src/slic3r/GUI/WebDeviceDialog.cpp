#include "WebDeviceDialog.hpp"
#include "I18N.hpp"
#include "GUI_App.hpp"
#include "MainFrame.hpp"
#include "SSWCP.hpp"
#include <wx/sizer.h>
#include <slic3r/GUI/Widgets/WebView.hpp>

namespace Slic3r { namespace GUI {

BEGIN_EVENT_TABLE(WebDeviceDialog, wxDialog)
    EVT_CLOSE(WebDeviceDialog::OnClose)
END_EVENT_TABLE()

WebDeviceDialog::WebDeviceDialog()
    : wxDialog((wxWindow*)(wxGetApp().mainframe), wxID_ANY, _L("Add Device"))
{
    SetBackgroundColour(*wxWHITE);

    // Create the webview

    // 语言判断
    wxString strlang = wxGetApp().current_language_code_safe();
    wxString target_url = m_device_url;
    if (strlang != "") {
        if (target_url.find("?") != std::string::npos) {
            target_url += "&lang=" + strlang;
        } else {
            target_url += "?lang=" + strlang;
        }
    }
        

    m_browser = WebView::CreateWebView(this, target_url);
    if (m_browser == nullptr) {
        wxLogError("Could not init m_browser");
        return;
    }
    m_browser->Hide();

    // Connect the webview events
    Bind(wxEVT_WEBVIEW_NAVIGATING, &WebDeviceDialog::OnNavigationRequest, this, m_browser->GetId());
    Bind(wxEVT_WEBVIEW_NAVIGATED, &WebDeviceDialog::OnNavigationComplete, this, m_browser->GetId());
    Bind(wxEVT_WEBVIEW_LOADED, &WebDeviceDialog::OnDocumentLoaded, this, m_browser->GetId());
    Bind(wxEVT_WEBVIEW_ERROR, &WebDeviceDialog::OnError, this, m_browser->GetId());
    Bind(wxEVT_WEBVIEW_SCRIPT_MESSAGE_RECEIVED, &WebDeviceDialog::OnScriptMessage, this, m_browser->GetId());

    // Set dialog size
    SetSize(FromDIP(wxSize(800, 600)));

    // Create sizer and add webview
    wxBoxSizer* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(m_browser, wxSizerFlags().Expand().Proportion(1));
    SetSizer(sizer);

    // Center dialog
    CenterOnParent();

    wxGetApp().UpdateDlgDarkUI(this);
}

WebDeviceDialog::~WebDeviceDialog()
{
    SSWCP::on_webview_delete(m_browser);
}

void WebDeviceDialog::reload()
{
    load_url(m_device_url);
}

void WebDeviceDialog::load_url(wxString &url)
{
    m_browser->LoadURL(url);
    m_browser->Show();
    Layout();
}

bool WebDeviceDialog::run()
{
    this->load_url(m_device_url);
    if (this->ShowModal() == wxID_OK) {
        return true;
    }
    return false;
}

void WebDeviceDialog::RunScript(const wxString &javascript)
{
    m_javascript = javascript;
    if (!m_browser) return;
    WebView::RunScript(m_browser, javascript);
}

void WebDeviceDialog::OnNavigationRequest(wxWebViewEvent &evt)
{
    evt.Skip();
}

void WebDeviceDialog::OnNavigationComplete(wxWebViewEvent &evt)
{
    m_browser->Show();
    Layout();
}

void WebDeviceDialog::OnDocumentLoaded(wxWebViewEvent &evt)
{
    evt.Skip();
}

void WebDeviceDialog::OnError(wxWebViewEvent &evt)
{
    wxLogError("Web View Error: %s", evt.GetString());
}

void WebDeviceDialog::OnScriptMessage(wxWebViewEvent &evt)
{
    BOOST_LOG_TRIVIAL(trace) << __FUNCTION__ << ": " << evt.GetString().ToUTF8().data();

    if (wxGetApp().get_mode() == comDevelop)
        wxLogMessage("Script message received; value = %s, handler = %s", evt.GetString(), evt.GetMessageHandler());

    // test
    SSWCP::handle_web_message(evt.GetString().ToUTF8().data(), m_browser);

}

void WebDeviceDialog::OnClose(wxCloseEvent& evt)
{
    evt.Skip();
}

}} // namespace Slic3r::GUI 