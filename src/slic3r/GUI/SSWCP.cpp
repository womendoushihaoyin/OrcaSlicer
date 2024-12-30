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

#include <slic3r/GUI/Widgets/WebView.hpp>

#include "MoonRaker.hpp"

namespace pt = boost::property_tree;

using namespace nlohmann;

namespace Slic3r { namespace GUI {

// SSWCP_Instance
void SSWCP_Instance::process() {
    if (m_cmd == "test") {
        sync_test();
    } else if (m_cmd == "test_async"){
        async_test();
    }
}


void SSWCP_Instance::send_to_js() {

    json response, payload;
    response["header"]     = m_header;
    
    if (m_event_id != "") {
        json header;
        header["event_id"] = m_event_id;
        response["header"] = header;
    }

    payload["status"]     = m_status;
    payload["msg"]        = m_msg;
    payload["data"]       = m_res_data;

    response["payload"] = payload;

    std::string str_res = "window.postMessage(" + response.dump() + ");";

    if (m_webview) {
        wxGetApp().CallAfter([this, str_res]() {
            try {
                WebView::RunScript(this->m_webview, str_res);
            } 
            catch (std::exception& e) {
                
            }
        });
    }
}

void SSWCP_Instance::finish_job() {
    SSWCP::delete_target(this);
}

void SSWCP_Instance::async_test() {
    // 业务逻辑

    auto http = Http::get("http://172.18.1.69/");
    http.on_error([&](std::string body, std::string error, unsigned status) {

    })
    .on_complete([=](std::string body, unsigned) {
        json data;
        data["str"] = body;
        this->m_res_data = data;
        this->send_to_js();    
        finish_job();
    })
    .perform();
}

void SSWCP_Instance::sync_test() {
    // 业务逻辑
    m_res_data = m_param_data;

    // send_to_js
    send_to_js();

    finish_job();
}

// SSWCP_MachineFind_Instance

void SSWCP_MachineFind_Instance::process() {
    if (m_cmd == "sw_StartMachineFind") {
        sw_StartMachineFind();
    } else if (m_cmd == "sw_GetMachineFindSupportInfo") {
        sw_GetMachineFindSupportInfo();
    } else if (m_cmd == "sw_StopMachineFind") {
        sw_StopMachineFind();
    }
}

void SSWCP_MachineFind_Instance::set_stop(bool stop)
{
    m_stop_mtx.lock();
    m_stop = stop;
    m_stop_mtx.unlock();
}

void SSWCP_MachineFind_Instance::sw_GetMachineFindSupportInfo()
{
    // 2.0.0 只支持 mdns - snapmaker
    json protocols = json::array();
    protocols.push_back("mdns");

    json mdns_service_names = json::array();
    mdns_service_names.push_back("snapmaker");

    json res;
    res["protocols"] = protocols;
    res["mdns_service_names"] = mdns_service_names;

    m_res_data = res;

    send_to_js();
}

void SSWCP_MachineFind_Instance::sw_StartMachineFind()
{
    try {

        std::vector<string> protocols;

        float last_time = -1;
        if (m_param_data.count("last_time") && m_param_data.is_number()) {
            last_time = m_param_data["last_time"].get<float>();
        }

        // 目前只支持通过mdns协议搜索snapmaker,prusalink，之后可以再扩充
        protocols.push_back("mdns");

        for (size_t i = 0; i < protocols.size(); ++i) {
            if (protocols[i] == "mdns") {
                std::vector<std::string> mdns_service_names;

                mdns_service_names.push_back("snapmaker");
                mdns_service_names.push_back("prusalink");
                mdns_service_names.push_back("rdlink");
                mdns_service_names.push_back("raop");

                m_engines.clear();
                for (size_t i = 0; i < mdns_service_names.size(); ++i) {
                    m_engines.push_back(nullptr);
                }

                Bonjour::TxtKeys txt_keys   = {"sn", "version"};
                std::string      unique_key = "sn";

                for (size_t i = 0; i < m_engines.size(); ++i) {
                    m_engines[i] = Bonjour(mdns_service_names[i])
                                     .set_txt_keys(std::move(txt_keys))
                                     .set_retries(3)
                                     .set_timeout(last_time >= 0.0 ? last_time/1000 : 20)
                                     .on_reply([this, unique_key](BonjourReply&& reply) {
                                         if(is_stop()){
                                            return;
                                         }
                                         json machine_data;
                                         machine_data["name"] = reply.service_name;
                                         machine_data["hostname"] = reply.hostname;
                                         machine_data["ip"]       = reply.ip.to_string();
                                         machine_data["port"]     = reply.port;
                                         if (reply.txt_data.count("protocol")) {
                                             machine_data["protocol"] = reply.txt_data["protocol"];
                                         }
                                         if (reply.txt_data.count("version")) {
                                             machine_data["pro_version"] = reply.txt_data["version"];
                                         }
                                         if (reply.txt_data.count(unique_key)) {
                                             machine_data["unique_key"]   = unique_key;
                                             machine_data["unique_value"] = reply.txt_data[unique_key];
                                             machine_data[unique_key]     = machine_data["unique_value"];
                                         }

                                         json machine_object;
                                         if (machine_data.count("unique_value")) {
                                             machine_object[reply.txt_data[unique_key]] = machine_data;
                                         } else {
                                             machine_object[reply.ip.to_string()] = machine_data;
                                         }
                                         this->add_machine_to_list(machine_object);
                                         
                                     })
                                     .on_complete([this]() {
                                         wxGetApp().CallAfter([this]() {
                                             this->onOneEngineEnd();
                                         });
                                     })
                                     .lookup();
                }

            } else {
                // 支持其他机器发现协议
            }
        }

    } 
    catch (std::exception& e) {}
}

void SSWCP_MachineFind_Instance::sw_StopMachineFind()
{
    try {
        SSWCP::stop_machine_find();
        send_to_js();
    }
    catch (std::exception& e) {

    }
}



void SSWCP_MachineFind_Instance::add_machine_to_list(const json& machine_info)
{
    for (const auto& [key, value] : machine_info.items()) {
        std::string ip        = value["ip"].get<std::string>();
        bool        need_send = false;
        m_machine_list_mtx.lock();
        if (!m_machine_list.count(ip)) {
            m_machine_list[ip] = machine_info;
            m_res_data.push_back(machine_info);
            need_send = true;
        }
        m_machine_list_mtx.unlock();

        if (need_send) {
            send_to_js();
        }
    }
    
    
}

void SSWCP_MachineFind_Instance::onOneEngineEnd()
{
    ++m_engine_end_count;
    if (m_engine_end_count == m_engines.size()) {
        this->finish_job();
    }
}


// SSWCP_MachineOption_Instance
void SSWCP_MachineOption_Instance::process()
{
    if (m_cmd == "sw_SendGCodes") {
        sw_SendGCodes();
    }
}


void SSWCP_MachineOption_Instance::sw_SendGCodes() {
    try {
        if (m_param_data.count("codes")) {
            std::shared_ptr<PrintHost> host(PrintHost::get_print_host(&wxGetApp().preset_bundle->printers.get_edited_preset().config));
            std::vector<std::string>   str_codes;

            if (m_param_data["codes"].is_array()) {
                json codes = m_param_data["codes"];
                for (size_t i = 0; i < codes.size(); ++i) {
                    str_codes.push_back(codes[i].get<std::string>());
                }
            } else if (m_param_data["codes"].is_string()) {
                str_codes.push_back(m_param_data["codes"].get<std::string>());
            }


            if (!host) {
                // 错误处理
                finish_job();
            } else {
                m_work_thread = 
                    std::thread([this, str_codes, host]() {
                        std::string extraInfo = "";
                        bool res = host->send_gcodes(str_codes, extraInfo);
                        if (res) {
                            send_to_js();
                        } else {
                            // 错误处理
                        }
                    });
            }
        } else {
            // 错误处理
            finish_job();
        }
    } catch (const std::exception&) {}
}


std::unordered_map<SSWCP_Instance*, std::shared_ptr<SSWCP_Instance>> SSWCP::m_instance_list;


std::unordered_set<std::string> SSWCP::m_machine_find_cmd_list = {
    "sw_GetMachineFindSupportInfo",
    "sw_StartMachineFind",
    "sw_StopMachineFind",
};

std::unordered_set<std::string> SSWCP::m_machine_option_cmd_list = {
    "sw_SendGCodes",
};

std::shared_ptr<SSWCP_Instance> SSWCP::create_sswcp_instance(
    std::string cmd, const json& header, const json& data, std::string callback_name, wxWebView* webview)
{
    if (m_machine_find_cmd_list.count(cmd)) {
        return std::shared_ptr<SSWCP_Instance>(new SSWCP_MachineFind_Instance(cmd, header, data, callback_name, webview));
    }
    else if (m_machine_option_cmd_list.count(cmd)) {
        return std::shared_ptr<SSWCP_Instance>(new SSWCP_MachineOption_Instance(cmd, header, data, callback_name, webview));
    }
    else {
        return std::shared_ptr<SSWCP_Instance>(new SSWCP_Instance(cmd, header, data, callback_name, webview));
    }
}

void SSWCP::handle_web_message(std::string message, wxWebView* webview) {
    try {
        if (!webview) {
            // todo: 返回错误处理
            return;
        }

        json j_message = json::parse(message);

        if (j_message.empty() || !j_message.count("header") || !j_message.count("payload") || !j_message["payload"].count("cmd")) {
            return;
        }

        json        header        = j_message["header"];
        json        payload       = j_message["payload"];

        std::string cmd = "";
        json        params;
        std::string callback_name = "";

        if (payload.count("cmd")) {
            cmd = payload["cmd"].get<std::string>();
        }
        if (payload.count("params")) {
            params = payload["params"];
        }

        if (payload.count("callback_name")) {
            callback_name = payload["callback_name"].get<std::string>();
        }

        std::shared_ptr<SSWCP_Instance> instance = create_sswcp_instance(cmd, header, params, callback_name, webview);
        if (instance) {
            m_instance_list[instance.get()] = instance;
            instance->process();
        }
        //if (!m_func_map.count(cmd)) {
        //    // todo:返回不支持处理
        //}

        //m_func_map[cmd](sequenceId, data, callback_name, webview);

    }
    catch (std::exception& e) {
        
    }
}


void SSWCP::delete_target(SSWCP_Instance* target) {
    wxGetApp().CallAfter([target]() {
        if (m_instance_list.count(target)) {
            m_instance_list.erase(target);
        }
    });
}


void SSWCP::stop_machine_find() {
    wxGetApp().CallAfter([]() {
        for (auto& instance : m_instance_list) {
            if (instance.second->getType() == SSWCP_MachineFind_Instance::MACHINE_FIND) {
                instance.second->set_stop(true);
            }
        }
    });
}


}}; // namespace Slic3r::GUI


