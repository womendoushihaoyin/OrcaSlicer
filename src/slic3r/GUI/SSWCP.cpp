// Implementation of web communication protocol for Slicer Studio
#include "SSWCP.hpp"
#include "GUI_App.hpp"
#include "MainFrame.hpp"
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

// Base SSWCP_Instance implementation
void SSWCP_Instance::process() {
    if (m_cmd == "test") {
        sync_test();
    } else if (m_cmd == "test_async"){
        async_test();
    }
}

// Mark instance as invalid
void SSWCP_Instance::set_Instance_illegal()
{
    m_illegal_mtx.lock();
    m_illegal = true;
    m_illegal_mtx.unlock();
}

// Check if instance is invalid
bool SSWCP_Instance::is_Instance_illegal() {
    m_illegal_mtx.lock();
    bool res = m_illegal;
    m_illegal_mtx.unlock();
    
    return res;
}

// Get associated webview
wxWebView* SSWCP_Instance::get_web_view() const {
    return m_webview;
}

// Send response to JavaScript
void SSWCP_Instance::send_to_js() {
    try {
        if (is_Instance_illegal()) {
            return;
        }
        json response, payload;
        response["header"] = m_header;

        payload["code"] = m_status;
        payload["msg"]  = m_msg;
        payload["data"] = m_res_data;

        response["payload"] = payload;

        std::string str_res = "window.postMessage(JSON.stringify(" + response.dump() + "), '*');";

        if (m_webview && m_webview->GetRefData()) {
            auto self = shared_from_this();
            wxGetApp().CallAfter([self, str_res]() {
                try {
                    WebView::RunScript(self->m_webview, str_res);
                } catch (std::exception& e) {}
            });
        }
    } catch (std::exception& e) {}
}

// Clean up instance
void SSWCP_Instance::finish_job() {
    SSWCP::delete_target(this);
}

// Asynchronous test implementation
void SSWCP_Instance::async_test() {
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

// Synchronous test implementation
void SSWCP_Instance::sync_test() {
    m_res_data = m_param_data;
    send_to_js();
    finish_job();
}

// Handle timeout event
void SSWCP_Instance::on_timeout() {
    m_status = -2;
    m_msg = "timeout";
    send_to_js();
    finish_job();
}

// SSWCP_MachineFind_Instance implementation

// Process machine find commands
void SSWCP_MachineFind_Instance::process() {
    if (m_event_id != "") {
        json header;
        send_to_js();

        m_header.clear();
        m_header["event_id"] = m_event_id;
    }

    if (m_cmd == "sw_StartMachineFind") {
        sw_StartMachineFind();
    } else if (m_cmd == "sw_GetMachineFindSupportInfo") {
        sw_GetMachineFindSupportInfo();
    } else if (m_cmd == "sw_StopMachineFind") {
        sw_StopMachineFind();
    }
}

// Set stop flag for machine discovery
void SSWCP_MachineFind_Instance::set_stop(bool stop)
{
    m_stop_mtx.lock();
    m_stop = stop;
    m_stop_mtx.unlock();
}

// Get machine discovery support info
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

// Start machine discovery
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
                //mdns_service_names.push_back("prusalink");
                //mdns_service_names.push_back("rdlink");
                //mdns_service_names.push_back("raop");

                m_engines.clear();
                for (size_t i = 0; i < mdns_service_names.size(); ++i) {
                    m_engines.push_back(nullptr);
                }

                Bonjour::TxtKeys txt_keys   = {"sn", "version", "machine_type"};
                std::string      unique_key = "sn";

                for (size_t i = 0; i < m_engines.size(); ++i) {
                    auto self = std::dynamic_pointer_cast<SSWCP_MachineFind_Instance>(shared_from_this());
                    if(!self){
                        continue;
                    }
                    m_engines[i] = Bonjour(mdns_service_names[i])
                                     .set_txt_keys(std::move(txt_keys))
                                     .set_retries(3)
                                     .set_timeout(last_time >= 0.0 ? last_time/1000 : 20)
                                     .on_reply([self, unique_key](BonjourReply&& reply) {
                                         if(self->is_stop()){
                                            return;
                                         }
                                         json machine_data;
                                         machine_data["name"] = reply.service_name;
                                         // machine_data["hostname"] = reply.hostname;
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

                                         // 模拟一下
                                         machine_data["cover"] = LOCALHOST_URL + std::to_string(PAGE_HTTP_PORT) +
                                                                 "/profiles/Snapmaker/Snapmaker A350 Dual BKit_cover.png";

                                         if (reply.txt_data.count("machine_type")) {
                                             std::string machine_type      = reply.txt_data["machine_type"];
                                             size_t      vendor_pos    = machine_type.find_first_of(" ");
                                             if (vendor_pos != std::string::npos) {
                                                 std::string vendor = machine_type.substr(0, vendor_pos);
                                                 std::string machine_cover = LOCALHOST_URL + std::to_string(PAGE_HTTP_PORT) + "/profiles/" +
                                                                             vendor + machine_type + "_cover.png";

                                                 machine_data["cover"] = machine_cover;
                                             }
                                             

                                         }
                                         json machine_object;
                                         if (machine_data.count("unique_value")) {
                                             self->add_machine_to_list(machine_object);
                                             machine_object[reply.txt_data[unique_key]] = machine_data;
                                         } else {
                                             machine_object[reply.ip.to_string()] = machine_data;
                                         }
                                         self->add_machine_to_list(machine_object);
                                         
                                     })
                                     .on_complete([self]() {
                                         wxGetApp().CallAfter([self]() {
                                             self->onOneEngineEnd();
                                         });
                                     })
                                     .lookup();
                }

            } else {
                // 支持其他机器发现协议
            }
        }

    } catch (std::exception& e) {}
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
            // m_res_data.push_back(machine_info);
            m_res_data[key] = value;
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
    if (m_event_id != "") {
        json header;
        send_to_js();

        m_header.clear();
        m_header["event_id"] = m_event_id;
    }
    if (m_cmd == "sw_SendGCodes") {
        sw_SendGCodes();
    }
    else if (m_cmd == "sw_GetMachineState") {
        sw_GetMachineState();
    } else if (m_cmd == "sw_SubscribeMachineState") {
        sw_SubscribeMachineState();
    } else if (m_cmd == "sw_GetMachineObjects") {
        sw_GetMachineObjects();
    } else if (m_cmd == "sw_SetSubscribeFilter") {
        sw_SetMachineSubscribeFilter();
    } else if (m_cmd == "sw_StopMachineStateSubscription") {
        sw_UnSubscribeMachineState();
    }
}

void SSWCP_MachineOption_Instance::sw_UnSubscribeMachineState() {
    try {
        std::shared_ptr<PrintHost> host(PrintHost::get_print_host(&wxGetApp().preset_bundle->printers.get_edited_preset().config));

        if (!host) {
            m_status = 1;
            m_msg    = "failure";
            send_to_js();
            finish_job();
        }

        auto self  = shared_from_this();
        host->async_unsubscribe_machine_info([self](const json& response) {
            if (response.is_null()) {
                self->m_status = -1;
                self->m_msg    = "failure";
                self->send_to_js();
            }else if(response.count("error")){
                if("error" == response["error"].get<std::string>()){
                    self->m_status = -2;
                    self->m_msg    = "timeout";
                    self->send_to_js();
                }
            } 
            else {
                self->m_res_data = response;
                self->send_to_js();
            }
            self->finish_job();
        });

        SSWCP::stop_subscribe_machine();


    } catch (std::exception& e) {}
}

void SSWCP_MachineOption_Instance::sw_SubscribeMachineState() {
    try {
        std::shared_ptr<PrintHost> host(PrintHost::get_print_host(&wxGetApp().preset_bundle->printers.get_edited_preset().config));

        if (!host) {
            m_status = 1;
            m_msg    = "failure";
            send_to_js();
            finish_job();
            return;
        }

        auto self = shared_from_this();
        host->async_subscribe_machine_info([self](const json& response) {
            if (response.is_null()) {
                self->m_status = -1;
                self->m_msg    = "failure";
                self->send_to_js();
            }else if(response.count("error")){
                if("error" == response["error"].get<std::string>()){
                    self->m_status = -2;
                    self->m_msg    = "timeout";
                    self->send_to_js();
                }
            } 
            else {
                self->m_res_data = response;
                self->send_to_js();
            }
        });

    } catch (std::exception& e) {}
}

void SSWCP_MachineOption_Instance::sw_GetMachineState() {
    try {
        if (m_param_data.count("targets")) {
            std::shared_ptr<PrintHost> host(PrintHost::get_print_host(&wxGetApp().preset_bundle->printers.get_edited_preset().config));
            std::vector<std::pair<std::string, std::vector<std::string>>> targets;

            json items = m_param_data["targets"];
            for (auto& [key, value] : items.items()) {
                if (value.is_null()) {
                    targets.push_back({key, {}});
                } else {
                    std::vector<std::string> items;
                    if (value.is_array()) {
                        for (size_t i = 0; i < value.size(); ++i) {
                            items.push_back(value[i].get<std::string>());
                        }
                    } else {
                        items.push_back(value.get<std::string>());
                    }
                    targets.push_back({key, items});
                }
            }

            if (!host) {
                m_status = -1;
                m_msg    = "failure";
                send_to_js();
                finish_job();
                return;
            }

            auto self = shared_from_this();
            host->async_get_machine_info(targets, [self](const json& response) {
                if (response.is_null()) {
                    self->m_status = -1;
                    self->m_msg    = "failure";
                    self->send_to_js();
                }else if(response.count("error")){
                    if("error" == response["error"].get<std::string>()){
                        self->m_status = -2;
                        self->m_msg    = "timeout";
                        self->send_to_js();
                    }
                } 
                else {
                    self->m_res_data = response;
                    self->send_to_js();
                }
                self->finish_job();
            });
        } else {
            finish_job();
        }

    } catch (std::exception& e) {}
}

void SSWCP_MachineOption_Instance::sw_SendGCodes() {
    try {
        if (m_param_data.count("script")) {
            std::shared_ptr<PrintHost> host(PrintHost::get_print_host(&wxGetApp().preset_bundle->printers.get_edited_preset().config));
            std::vector<std::string>   str_codes;
        

            if (m_param_data["script"].is_array()) {
                json codes = m_param_data["script"];
                for (size_t i = 0; i < codes.size(); ++i) {
                    str_codes.push_back(codes[i].get<std::string>());
                }
            } else if (m_param_data["script"].is_string()) {
                str_codes.push_back(m_param_data["script"].get<std::string>());
            }

            if (!host) {
                m_status = -1;
                m_msg    = "failure";
                send_to_js();
                finish_job();
                return;
            }

            auto self = shared_from_this();
            host->async_send_gcodes(str_codes, [self](const json& response) {
                if (response.is_null()) {
                    self->m_status = -1;
                    self->m_msg    = "failure";
                    self->send_to_js();
                }else if(response.count("error")){
                    if("error" == response["error"].get<std::string>()){
                        self->m_status = -2;
                        self->m_msg    = "timeout";
                        self->send_to_js();
                    }
                } 
                else {
                    self->m_res_data = response;
                    self->send_to_js();
                }
                self->finish_job();
            });
        }
        
    } catch (const std::exception&) {}
}

void SSWCP_MachineOption_Instance::sw_SetMachineSubscribeFilter()
{
    try {
        if (m_param_data.count("targets")) {
            std::shared_ptr<PrintHost> host(PrintHost::get_print_host(&wxGetApp().preset_bundle->printers.get_edited_preset().config));
            std::vector<std::pair<std::string, std::vector<std::string>>> targets;

            json items = m_param_data["targets"];
            for (auto& [key, value] : items.items()) {
                if (value.is_null()) {
                    targets.push_back({key, {}});
                } else {
                    std::vector<std::string> items;
                    if (value.is_array()) {
                        for (size_t i = 0; i < value.size(); ++i) {
                            items.push_back(value[i].get<std::string>());
                        }
                    } else {
                        items.push_back(value.get<std::string>());
                    }
                    targets.push_back({key, items});
                }
            }

            if (!host) {
                // 错误处理
                m_status = -1;
                m_msg    = "failure";
                send_to_js();
                finish_job();
            } else {
                auto self = shared_from_this();
                host->async_set_machine_subscribe_filter(targets, [self](const json& response) {
                    if (response.is_null()) {
                        self->m_status = -1;
                        self->m_msg    = "failure";
                        self->send_to_js();
                    }else if(response.count("error")){
                        if("error" == response["error"].get<std::string>()){
                            self->m_status = -2;
                            self->m_msg    = "timeout";
                            self->send_to_js();
                        }
                    } 
                    else {
                        self->m_res_data = response;
                        self->send_to_js();
                    }
                    self->finish_job();
                });
            }
        } else {
            finish_job();
        }

    } catch (std::exception& e) {}
}
void SSWCP_MachineOption_Instance::sw_GetMachineObjects()
{
    try {
        std::shared_ptr<PrintHost> host(PrintHost::get_print_host(&wxGetApp().preset_bundle->printers.get_edited_preset().config));
            
        if (!host) {
            m_status = -1;
            m_msg = "failure";
            send_to_js();
            finish_job();
            return;
        }

        auto self = shared_from_this();
        host->async_get_machine_objects([self](const json& response) {
            if (response.is_null()) {
                self->m_status = -1;
                self->m_msg    = "failure";
                self->send_to_js();
            }else if(response.count("error")){
                if("error" == response["error"].get<std::string>()){
                    self->m_status = -2;
                    self->m_msg    = "timeout";
                    self->send_to_js();
                }
            } 
            else {
                self->m_res_data = response;
                self->send_to_js();
            }
            self->finish_job();
        });

    } catch (std::exception& e) {}
}

// SSWCP_MachineConnect_Instance
void SSWCP_MachineConnect_Instance::process() {
    if (m_cmd == "sw_Test_connect") {
        sw_test_connect();
    } else if (m_cmd == "sw_Connect") {
        sw_connect();
    } else if (m_cmd == "sw_Disconnect") {
        sw_disconnect();
    }
}

void SSWCP_MachineConnect_Instance::sw_test_connect() {
    try {
        if (m_param_data.count("ip")) {
            std::string protocol = "moonraker";
            
            if (m_param_data.count("protcol")) {
                protocol = m_param_data["protocol"].get<std::string>();
            }
                
            std::string ip       = m_param_data["ip"].get<std::string>();
            int         port     = -1;

            if (m_param_data.count("port") && m_param_data["port"].is_number_integer()) {
                port = m_param_data["port"].get<int>();
            }

            auto p_config = &(wxGetApp().preset_bundle->printers.get_edited_preset().config);


            PrintHostType type = PrintHostType::htMoonRaker;
            // todo : 增加输入与type的映射
            
            p_config->option<ConfigOptionEnum<PrintHostType>>("host_type")->value = type;
            
            p_config->set("print_host", ip + (port == -1 ? "" : std::to_string(port)));

            std::shared_ptr<PrintHost> host(PrintHost::get_print_host(&wxGetApp().preset_bundle->printers.get_edited_preset().config));

            if (!host) {
                // 错误处理
                finish_job();
            } else {
                m_work_thread = std::thread([this, host] {
                    wxString msg;
                    bool        res = host->test(msg);
                    if (res) {
                        send_to_js();
                    } else {
                        // 错误处理
                        m_status = 1;
                        m_msg    = msg.c_str();
                        send_to_js();
                    }

                    finish_job();
                });
            }
        } else {
            // 错误处理
            finish_job();
        }
    } catch (const std::exception&) {}
}

void SSWCP_MachineConnect_Instance::sw_connect() {
    try {
        if (m_param_data.count("ip")) {
            std::string ip = m_param_data["ip"].get<std::string>();

            int port = -1;
            if (m_param_data.count("port") && m_param_data["port"].is_number_integer()) {
                port = m_param_data["port"].get<int>();
            }

            std::string protocol = "moonraker";
            if (m_param_data.count("protocol") && m_param_data["protocol"].is_string()) {
                protocol = m_param_data["protocol"].get<std::string>();
            }

            auto p_config = &(wxGetApp().preset_bundle->printers.get_edited_preset().config);

            PrintHostType type = PrintHostType::htMoonRaker_mqtt;
            // todo : 增加输入与type的映射

            p_config->option<ConfigOptionEnum<PrintHostType>>("host_type")->value = type;

            p_config->set("print_host", ip + (port == -1 ? "" : ":" + std::to_string(port)));

            std::shared_ptr<PrintHost> host(PrintHost::get_print_host(&wxGetApp().preset_bundle->printers.get_edited_preset().config));

            json connect_params;
            if (m_param_data.count("sn") && m_param_data["sn"].is_string()) {
                connect_params["sn"] = m_param_data["sn"].get<std::string>();
            }

            if (!host) {
                // 错误处理
                finish_job();
            } else {
                auto self = shared_from_this();
                m_work_thread = std::thread([self, host, connect_params] {
                    // moonraker_mqtt 如果没有sn码，需要http请求获取sn码

                    //wxString msg = "";
                    //bool     res = host->test(msg);

                    //if (res) {
                    //    wxGetApp().mainframe->load_printer_url("http://"+  host->get_host());
                    //    // wxGetApp().mainframe->load_printer_url(host->, wxString apikey)
                    //} else {
                    //    m_status = 1;
                    //    m_msg    = msg.c_str();
                    //    send_to_js();
                    //}

                    wxString msg = "";
                    json     params;
                    bool res = host->connect(msg, connect_params);

                    std::string ip_port = host->get_host();
                    if (res) {
                        // 后续应改成机器交互页的本地web服务器地址
                        int         pos     = ip_port.find(':');
                        if (pos != std::string::npos) {
                            ip_port = ip_port.substr(0, pos);
                        }
                        wxGetApp().mainframe->load_printer_url("http://" + ip_port);

                        wxGetApp().CallAfter([ip_port](){
                            MessageDialog msg_window(nullptr,
                                                 ip_port + _L(" connected sucessfully !\n"),
                                                 L("Machine Connected"), wxICON_QUESTION | wxOK);
                            msg_window.ShowModal();

                            auto dialog = wxGetApp().get_web_device_dialog();
                            if (dialog) {
                                dialog->EndModal(1);
                            }
                        });
                        
                    } else {
                        wxGetApp().CallAfter([ip_port](){
                            MessageDialog msg_window(nullptr, ip_port + _L(" connected unseccessfully !\n"), L("Failed"),
                                                 wxICON_QUESTION | wxOK);
                            msg_window.ShowModal();
                        });

                        self->m_status = 1;
                        self->m_msg    = msg.c_str();
                    }

                    self->send_to_js();
                    self->finish_job();

                });
            }

        } else {
            // 错误处理
            finish_job();
        }
    } catch (std::exception& e) {}
}

void SSWCP_MachineConnect_Instance::sw_disconnect() {
    auto self = shared_from_this();
    m_work_thread = std::thread([self](){
        std::shared_ptr<PrintHost> host(PrintHost::get_print_host(&wxGetApp().preset_bundle->printers.get_edited_preset().config));

        wxString msg = "";
        bool res = host->disconnect(msg, {});

        if (res) {
            // todo: 交互页面删除
        } else {
            self->m_status = 1;
            self->m_msg    = msg.c_str();
        }

        self->send_to_js();
        self->finish_job();
    });
}


TimeoutMap<SSWCP_Instance*, std::shared_ptr<SSWCP_Instance>> SSWCP::m_instance_list;
constexpr std::chrono::milliseconds SSWCP::DEFAULT_INSTANCE_TIMEOUT;

std::unordered_set<std::string> SSWCP::m_machine_find_cmd_list = {
    "sw_GetMachineFindSupportInfo",
    "sw_StartMachineFind",
    "sw_StopMachineFind",
};

std::unordered_set<std::string> SSWCP::m_machine_option_cmd_list = {
    "sw_SendGCodes",
    "sw_GetMachineState",
    "sw_SubscribeMachineState",
    "sw_GetMachineObjects",
    "sw_SetSubscribeFilter",
    "sw_StopMachineStateSubscription",
};

std::unordered_set<std::string> SSWCP::m_machine_connect_cmd_list = {
    "sw_Test_connect",
    "sw_Connect",
    "sw_Disconnect",
};

std::shared_ptr<SSWCP_Instance> SSWCP::create_sswcp_instance(std::string cmd, const json& header, const json& data, std::string event_id, wxWebView* webview)
{
    std::shared_ptr<SSWCP_Instance> instance;
    
    if (m_machine_find_cmd_list.find(cmd) != m_machine_find_cmd_list.end()) {
        instance = std::make_shared<SSWCP_MachineFind_Instance>(cmd, header, data, event_id, webview);
    } else if (m_machine_connect_cmd_list.find(cmd) != m_machine_connect_cmd_list.end()) {
        instance = std::make_shared<SSWCP_MachineConnect_Instance>(cmd, header, data, event_id, webview);
    } else if (m_machine_option_cmd_list.find(cmd) != m_machine_option_cmd_list.end()) {
        instance = std::make_shared<SSWCP_MachineOption_Instance>(cmd, header, data, event_id, webview);
    } else {
        instance = std::make_shared<SSWCP_Instance>(cmd, header, data, event_id, webview);
    }
    
    return instance;
}

// Handle incoming web messages
void SSWCP::handle_web_message(std::string message, wxWebView* webview) {
    try {
        if (!webview) {
            return;
        }

        json j_message = json::parse(message);

        if (j_message.empty() || !j_message.count("header") || !j_message.count("payload") || !j_message["payload"].count("cmd")) {
            return;
        }

        json header = j_message["header"];
        json payload = j_message["payload"];

        std::string cmd = "";
        std::string event_id = "";
        json params;

        if (payload.count("cmd")) {
            cmd = payload["cmd"].get<std::string>();
        }
        if (payload.count("params")) {
            params = payload["params"];
        }

        if (payload.count("event_id") && !payload["event_id"].is_null()) {
            event_id = payload["event_id"].get<std::string>();
        }

        std::shared_ptr<SSWCP_Instance> instance = create_sswcp_instance(cmd, header, params, event_id, webview);
        if (instance) {
            if (event_id != "") {
                m_instance_list.add_infinite(instance.get(), instance);
            } else {
                m_instance_list.add(instance.get(), instance, DEFAULT_INSTANCE_TIMEOUT);
            }
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

// Delete instance from list
void SSWCP::delete_target(SSWCP_Instance* target) {
    wxGetApp().CallAfter([target]() {
        m_instance_list.remove(target);
    });
}

// Stop all machine subscriptions
void SSWCP::stop_subscribe_machine()
{
    wxGetApp().CallAfter([]() {
        std::vector<SSWCP_Instance*> instances_to_stop;
        
        auto snapshot = m_instance_list.get_snapshot();

        // Get all subscription instances to stop
        for (const auto& instance : snapshot) {  
            if (instance.second->getType() == SSWCP_MachineFind_Instance::MACHINE_OPTION && instance.second->m_cmd == "sw_SubscribeMachineState") {
                instances_to_stop.push_back(instance.first);
            }
        }
        
        // Stop each instance
        for (auto* instance : instances_to_stop) {
            auto instance_ptr = m_instance_list.get(instance);
            if (instance_ptr) {
                (*instance_ptr)->finish_job();
            }
        }
    });
}

// Stop all machine discovery instances
void SSWCP::stop_machine_find() {
    wxGetApp().CallAfter([]() {
        std::vector<SSWCP_Instance*> instances_to_stop;
        
        auto snapshot = m_instance_list.get_snapshot();

        // Get all discovery instances to stop
        for (const auto& instance : snapshot) {  
            if (instance.second->getType() == SSWCP_MachineFind_Instance::MACHINE_FIND) {
                instances_to_stop.push_back(instance.first);
            }
        }
        
        // Set stop flag for each instance
        for (auto* instance : instances_to_stop) {
            auto instance_ptr = m_instance_list.get(instance);
            if (instance_ptr) {
                (*instance_ptr)->set_stop(true);
            }
        }
    });
}

// Handle webview deletion
void SSWCP::on_webview_delete(wxWebView* view)
{
    // Mark all instances associated with this webview as invalid
    std::vector<SSWCP_Instance*> instances_to_invalidate;
    
    // Get all instances using this webview
    for (const auto& instance : m_instance_list) {
        if (instance.second->value->get_web_view() == view) {
            instances_to_invalidate.push_back(instance.first);
        }
    }
    
    // Mark each instance as invalid
    for (auto* instance : instances_to_invalidate) {
        auto instance_ptr = m_instance_list.get(instance);
        if (instance_ptr) {
            (*instance_ptr)->set_Instance_illegal();
        }
    }
}

}}; // namespace Slic3r::GUI


