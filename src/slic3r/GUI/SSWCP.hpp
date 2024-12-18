#ifndef SSWCP_HPP
#define SSWCP_HPP

#include <iostream>
#include <mutex>
#include <stack>

#include <wx/webview.h>

#include "nlohmann/json.hpp"

using namespace nlohmann;

namespace Slic3r { namespace GUI {

class SSWCP
{
public:
    static void handle_web_message(std::string message, wxWebView* webview);

    static void send_to_js(wxWebView* webview, int sequenceId, std::string callback_name, const json& data = {} , int status = 0, std::string err = "");

protected:
    static void sync_test(int sequenceId, const json& data, std::string callback_name, wxWebView* webview);

    static void async_test(int sequenceId, const json& data, std::string callback_name, wxWebView* webview);

private:

    static std::unordered_map<std::string, std::function<void(int, const json&, std::string, wxWebView*)>> m_func_map;

};

}};

#endif
