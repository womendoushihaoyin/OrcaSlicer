#include "SSWCP.hpp"
#include "GUI_App.hpp"
#include "nlohmann/json.hpp"
#include <algorithm>
#include <iterator>
#include <exception>
#include <cstdlib>
#include <regex>
#include <thread>
#include <string_view>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/format.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/convert.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>

namespace pt = boost::property_tree;

using namespace nlohmann;

namespace Slic3r { namespace GUI {

std::unordered_map<std::string, std::function<void(int, const json&, std::string, wxWebView*)>> SSWCP::m_func_map = {
    {"test", SSWCP::sync_test},
    {"test_async", SSWCP::async_test},
};

void SSWCP::handle_web_message(std::string message, wxWebView* webview) {
    try {
        if (!webview) {
            // todo: 返回错误处理
        }

        json j_message = json::parse(message);

        if (j_message.empty() || j_message.count("sequenceId") || j_message.count("cmd") || j_message.count("data") || j_message.count("callback_name")) {
            // todo:返回错误处理
        }

        int         sequenceId    = j_message["sequenceId"].get<int>();
        std::string cmd        = j_message["cmd"].get<std::string>();
        json        data       = j_message["data"];
        std::string callback_name = j_message["callback_name"].get<std::string>();

        if (!m_func_map.count(cmd)) {
            // todo:返回不支持处理
        }

        m_func_map[cmd](sequenceId, data, callback_name, webview);

    }
    catch (std::exception& e) {

    }
}


void SSWCP::sync_test(int sequenceId, const json& data, std::string callback_name, wxWebView* webview) {
    // 业务逻辑



    // send_to_js
    send_to_js(webview, sequenceId, callback_name, data);
}

void SSWCP::async_test(int sequenceId, const json& data, std::string callback_name, wxWebView* webview) {
    // 业务逻辑 

    auto http = Http::get("http://172.18.1.69/");
    http.on_error([&](std::string body, std::string error, unsigned status) {
            
    })
    .on_complete([=](std::string body, unsigned) {
        json data;
        data["str"] = body;

        SSWCP::send_to_js(webview, sequenceId, callback_name, data);
    })
    .perform();
}


void SSWCP::send_to_js(wxWebView* webview, int sequenceId, std::string callback_name, const json& data, int status, std::string err)
{
    if (callback_name == "") {
        return;
    }

    json response;
    response["sequenceId"] = sequenceId;
    response["status"]     = status;
    response["err"]        = err;
    response["data"]       = data;

    std::string str_res = callback_name + "(" + response.dump() + ")";

    if (webview) {
        wxGetApp().CallAfter([webview, str_res]() {
            try {
                webview->RunScript(str_res);
            }
            catch(std::exception& e){}
        }); 
    }
}


}}; // namespace Slic3r::GUI