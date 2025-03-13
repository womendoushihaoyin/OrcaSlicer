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

#include "slic3r/GUI/WebPresetDialog.hpp"

#include "slic3r/GUI/SMPhysicalPrinterDialog.hpp"

namespace pt = boost::property_tree;

using namespace nlohmann;

namespace Slic3r { namespace GUI {

// Base SSWCP_Instance implementation
void SSWCP_Instance::process() {
    if (m_cmd == "test") {
        sync_test();
    } else if (m_cmd == "test_async"){
        async_test();
    } else if (m_cmd == "sw_test_mqtt_moonraker") {
        test_mqtt_request();
    } else {
        this->m_status = -1;
        this->m_msg    = "failure";
        this->send_to_js();
        this->finish_job();
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

void SSWCP_Instance::test_mqtt_request() {
    try {
        std::shared_ptr<PrintHost> host = nullptr;
        wxGetApp().get_connect_host(host);

        if (!host) {
            m_status = -1;
            m_msg    = "failure";
            send_to_js();
            finish_job();
            return;
        }

        auto self = shared_from_this();
        host->test_async_wcp_mqtt_moonraker(m_param_data, [self](const json& response) {
            SSWCP_Instance::on_mqtt_msg_arrived(self,response);
        });
        // host->async_get_printer_info([self](const json& response) { SSWCP_Instance::on_mqtt_msg_arrived(self, response); });
    } catch (std::exception& e) {}
}

void SSWCP_Instance::on_mqtt_msg_arrived(std::shared_ptr<SSWCP_Instance> obj, const json& response) {
    if (!obj) {
        return;
    }
    if (response.is_null()) {
        obj->m_status = -1;
        obj->m_msg    = "failure";
        obj->send_to_js();
    } else if (response.count("error")) {
        if (response["error"].is_string() && "timeout" == response["error"].get<std::string>()) {
            obj->on_timeout();
        } else {
            obj->m_res_data = response;
            obj->send_to_js();
        }
    } else {
        obj->m_res_data = response;
        obj->send_to_js();
    }
    obj->finish_job();
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
    } else {
        this->m_status = -1;
        this->m_msg    = "failure";
        this->send_to_js();
        this->finish_job();
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
                                        /* machine_data["cover"] = LOCALHOST_URL + std::to_string(PAGE_HTTP_PORT) +
                                                                 "/profiles/Snapmaker/Snapmaker A350 Dual BKit_cover.png";*/

                                         if (reply.txt_data.count("machine_type")) {
                                             std::string machine_type      = reply.txt_data["machine_type"];

                                             auto machine_ip_type = MachineIPType::getInstance();
                                             if (machine_ip_type) {
                                                 machine_ip_type->add_instance(reply.ip.to_string(), machine_type);
                                             }

                                             size_t      vendor_pos    = machine_type.find_first_of(" ");
                                             if (vendor_pos != std::string::npos) {
                                                 std::string vendor = machine_type.substr(0, vendor_pos);
                                                 std::string machine_cover = LOCALHOST_URL + std::to_string(PAGE_HTTP_PORT) + "/profiles/" +
                                                                             vendor + "/" + machine_type + "_cover.png";

                                                 machine_data["cover"] = machine_cover;
                                             }
                                         } else {
                                             // test
                                             auto machine_ip_type = MachineIPType::getInstance();
                                             if (machine_ip_type) {
                                                 machine_ip_type->add_instance(reply.ip.to_string(), "lava");
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
            m_machine_list_mtx.unlock();

            m_res_data[key] = value;
            need_send = true;
        } else {
            m_machine_list_mtx.unlock();
        }
        

        m_res_data[key]["connected"] = false;

        DeviceInfo info;
        if (wxGetApp().app_config->get_device_info(ip, info) && info.connected) {
            m_res_data[key]["connected"] = true;
        }

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
    } else if (m_cmd == "sw_GetPrinterInfo") {
        sw_GetPrintInfo();
    } else if (m_cmd == "sw_GetMachineSystemInfo") {
        sw_GetSystemInfo();
    } else if (m_cmd == "sw_MachinePrintStart") {
        sw_MachinePrintStart();
    } else if (m_cmd == "sw_MachinePrintPause") {
        sw_MachinePrintPause();
    } else if (m_cmd == "sw_MachinePrintResume") {
        sw_MachinePrintResume();
    } else if (m_cmd == "sw_MachinePrintCancel") {
        sw_MachinePrintCancel();
    } else if (m_cmd == "sw_MachineFilesRoots") {
        sw_MachineFilesRoots();
    } else if (m_cmd == "sw_MachineFilesMetadata") {
        sw_MachineFilesMetadata();
    } else if (m_cmd == "sw_MachineFilesThumbnails") {
        sw_MachineFilesThumbnails();
    } else if (m_cmd == "sw_MachineFilesGetDirectory") {
        sw_MachineFilesGetDirectory();
    } else if (m_cmd == "sw_CameraStartMonitor") {
        sw_CameraStartMonitor();
    } else if (m_cmd == "sw_CameraStopMonitor") {
        sw_CameraStartMonitor();
    } 
    
    else {
        this->m_status = -1;
        this->m_msg    = "failure";
        this->send_to_js();
        this->finish_job();
    }
}

void SSWCP_MachineOption_Instance::sw_UnSubscribeMachineState() {
    try {
        std::shared_ptr<PrintHost> host = nullptr;
        wxGetApp().get_connect_host(host);

        if (!host) {
            m_status = 1;
            m_msg    = "failure";
            send_to_js();
            finish_job();
        }

        auto self  = shared_from_this();
        host->async_unsubscribe_machine_info([self](const json& response) {
            SSWCP_Instance::on_mqtt_msg_arrived(self,response);
        });

        SSWCP::stop_subscribe_machine();


    } catch (std::exception& e) {}
}

void SSWCP_MachineOption_Instance::sw_SubscribeMachineState() {
    try {
        std::shared_ptr<PrintHost> host = nullptr;
        wxGetApp().get_connect_host(host);

        if (!host) {
            m_status = 1;
            m_msg    = "failure";
            send_to_js();
            finish_job();
            return;
        }

        auto self = shared_from_this();
        host->async_subscribe_machine_info([self](const json& response) {
            SSWCP_Instance::on_mqtt_msg_arrived(self,response);
        });

    } catch (std::exception& e) {}
}

void SSWCP_MachineOption_Instance::sw_GetPrintInfo() {
    try {
        std::shared_ptr<PrintHost> host = nullptr;
        wxGetApp().get_connect_host(host);

        if (!host) {
            m_status = -1;
            m_msg    = "failure";
            send_to_js();
            finish_job();
            return;
        }

        auto self = shared_from_this();
        host->async_get_printer_info([self](const json& response) {
            SSWCP_Instance::on_mqtt_msg_arrived(self,response);
        });
    }
    catch (std::exception& e) {

    }
}

void SSWCP_MachineOption_Instance::sw_GetMachineState() {
    try {
        if (m_param_data.count("objects")) {
            std::shared_ptr<PrintHost> host = nullptr;
            wxGetApp().get_connect_host(host);
            std::vector<std::pair<std::string, std::vector<std::string>>> targets;

            json items = m_param_data["objects"];
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
                SSWCP_Instance::on_mqtt_msg_arrived(self,response);
            });
        } else {
            m_status = -1;
            m_msg    = "failure";
            send_to_js();
            finish_job();
        }

    } catch (std::exception& e) {}
}

void SSWCP_MachineOption_Instance::sw_SendGCodes() {
    try {
        if (m_param_data.count("script")) {
            std::shared_ptr<PrintHost> host = nullptr;
            wxGetApp().get_connect_host(host);
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
                SSWCP_Instance::on_mqtt_msg_arrived(self,response);
            });
        }
        
    } catch (const std::exception&) {}
}

void SSWCP_MachineOption_Instance::sw_MachinePrintStart() {
    try {
        if (m_param_data.count("filename")) {
            std::shared_ptr<PrintHost> host = nullptr;
            wxGetApp().get_connect_host(host);

            std::string filename = m_param_data["filename"].get<std::string>();

            if (!host) {
                m_status = -1;
                m_msg    = "failure";
                send_to_js();
                finish_job();
                return;
            }

            auto self = shared_from_this();
            host->async_start_print_job(filename, [self](const json& response) { SSWCP_Instance::on_mqtt_msg_arrived(self, response); });
        }
    }
    catch(std::exception& e){}
}

void SSWCP_MachineOption_Instance::sw_MachinePrintPause()
{
    try {
        std::shared_ptr<PrintHost> host = nullptr;
        wxGetApp().get_connect_host(host);

        if (!host) {
            m_status = -1;
            m_msg    = "failure";
            send_to_js();
            finish_job();
            return;
        }

        auto self = shared_from_this();
        host->async_pause_print_job([self](const json& response) { SSWCP_Instance::on_mqtt_msg_arrived(self, response); });
    } catch (std::exception& e) {}
}

void SSWCP_MachineOption_Instance::sw_MachinePrintResume()
{
    try {
        std::shared_ptr<PrintHost> host = nullptr;
        wxGetApp().get_connect_host(host);

        if (!host) {
            m_status = -1;
            m_msg    = "failure";
            send_to_js();
            finish_job();
            return;
        }

        auto self = shared_from_this();
        host->async_resume_print_job([self](const json& response) { SSWCP_Instance::on_mqtt_msg_arrived(self, response); });
    } catch (std::exception& e) {}
}

void SSWCP_MachineOption_Instance::sw_MachinePrintCancel()
{
    try {
        std::shared_ptr<PrintHost> host = nullptr;
        wxGetApp().get_connect_host(host);

        if (!host) {
            m_status = -1;
            m_msg    = "failure";
            send_to_js();
            finish_job();
            return;
        }

        auto self = shared_from_this();
        host->async_cancel_print_job([self](const json& response) { SSWCP_Instance::on_mqtt_msg_arrived(self, response); });
    } catch (std::exception& e) {}
}

void SSWCP_MachineOption_Instance::sw_GetSystemInfo()
{
    try {
        std::shared_ptr<PrintHost> host = nullptr;
        wxGetApp().get_connect_host(host);

        if (!host) {
            m_status = -1;
            m_msg    = "failure";
            send_to_js();
            finish_job();
            return;
        }

        auto self = shared_from_this();
        host->async_get_system_info([self](const json& response) { 
            SSWCP_Instance::on_mqtt_msg_arrived(self, response); 
        });
    }
    catch(std::exception& e){}
}

void SSWCP_MachineOption_Instance::sw_SetMachineSubscribeFilter()
{
    try {
        if (m_param_data.count("objects")) {
            std::shared_ptr<PrintHost> host = nullptr;
            wxGetApp().get_connect_host(host);
            std::vector<std::pair<std::string, std::vector<std::string>>> targets;

            json items = m_param_data["objects"];
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
                    SSWCP_Instance::on_mqtt_msg_arrived(self, response);
                });
            }
        } else {
            m_status = -1;
            m_msg    = "failure";
            send_to_js();
            finish_job();
        }

    } catch (std::exception& e) {}
}
void SSWCP_MachineOption_Instance::sw_GetMachineObjects()
{
    try {
        std::shared_ptr<PrintHost> host = nullptr;
        wxGetApp().get_connect_host(host);
            
        if (!host) {
            m_status = -1;
            m_msg = "failure";
            send_to_js();
            finish_job();
            return;
        }

        auto self = shared_from_this();
        host->async_get_machine_objects([self](const json& response) {
            SSWCP_Instance::on_mqtt_msg_arrived(self,response);
        });

    } catch (std::exception& e) {}
}

void SSWCP_MachineOption_Instance::sw_MachineFilesRoots()
{
    try {
        std::shared_ptr<PrintHost> host = nullptr;
        wxGetApp().get_connect_host(host);
            
        if (!host) {
            m_status = -1;
            m_msg = "failure";
            send_to_js();
            finish_job();
            return;
        }

        auto self = shared_from_this();
        host->async_machine_files_roots([self](const json& response) {
            SSWCP_Instance::on_mqtt_msg_arrived(self,response);
        });
    } catch (std::exception& e) {}
}

void SSWCP_MachineOption_Instance::sw_MachineFilesMetadata() {
    try {
        if (m_param_data.count("filename")) {
            std::shared_ptr<PrintHost> host = nullptr;
            wxGetApp().get_connect_host(host);

            if (!host) {
                m_status = -1;
                m_msg    = "failure";
                send_to_js();
                finish_job();
                return;
            }

            std::string filename = m_param_data["filename"].get<std::string>();

            auto self = shared_from_this();
            host->async_machine_files_metadata(filename, [self](const json& response) { SSWCP_Instance::on_mqtt_msg_arrived(self, response); });
        } else {
            m_status = -1;
            m_msg    = "failure";
            send_to_js();
            finish_job();
        }
        
    } catch (std::exception& e) {}
}

void SSWCP_MachineOption_Instance::sw_MachineFilesThumbnails()
{
    try {
        if (m_param_data.count("filename")) {
            std::shared_ptr<PrintHost> host = nullptr;
            wxGetApp().get_connect_host(host);

            if (!host) {
                m_status = -1;
                m_msg    = "failure";
                send_to_js();
                finish_job();
                return;
            }

            std::string filename = m_param_data["filename"].get<std::string>();

            auto self = shared_from_this();
            host->async_machine_files_thumbnails(filename,
                                               [self](const json& response) { SSWCP_Instance::on_mqtt_msg_arrived(self, response); });
        } else {
            m_status = -1;
            m_msg    = "failure";
            send_to_js();
            finish_job();
        }

    } catch (std::exception& e) {}
}

void SSWCP_MachineOption_Instance::sw_MachineFilesGetDirectory()
{
    try {
        if (m_param_data.count("path") && m_param_data.count("extended")) {
            std::shared_ptr<PrintHost> host = nullptr;
            wxGetApp().get_connect_host(host);

            if (!host) {
                m_status = -1;
                m_msg    = "failure";
                send_to_js();
                finish_job();
                return;
            }

            std::string path = m_param_data["path"].get<std::string>();
            bool        extend = m_param_data["extended"].get<bool>();

            auto self = shared_from_this();
            host->async_machine_files_directory(path, extend, [self](const json& response) { SSWCP_Instance::on_mqtt_msg_arrived(self, response); });
        } else {
            m_status = -1;
            m_msg    = "failure";
            send_to_js();
            finish_job();
        }

    } catch (std::exception& e) {}
}

void SSWCP_MachineOption_Instance::sw_CameraStartMonitor() {
    try {
        if (m_param_data.count("domain")) {
            std::shared_ptr<PrintHost> host = nullptr;
            wxGetApp().get_connect_host(host);

            if (!host) {
                m_status = -1;
                m_msg    = "failure";
                send_to_js();
                finish_job();
                return;
            }

            std::string domain = m_param_data["domain"].get<std::string>();

            auto self = shared_from_this();
            host->async_camera_start(domain, [self](const json& response) { SSWCP_Instance::on_mqtt_msg_arrived(self, response); });
        } else {
            m_status = -1;
            m_msg    = "failure";
            send_to_js();
            finish_job();
        }

    } catch (std::exception& e) {}
}

void SSWCP_MachineOption_Instance::sw_CameraStopMonitor() {
    try {
        if (m_param_data.count("domain")) {
            std::shared_ptr<PrintHost> host = nullptr;
            wxGetApp().get_connect_host(host);

            if (!host) {
                m_status = -1;
                m_msg    = "failure";
                send_to_js();
                finish_job();
                return;
            }

            std::string domain = m_param_data["domain"].get<std::string>();

            auto self = shared_from_this();
            host->async_canmera_stop(domain, [self](const json& response) { SSWCP_Instance::on_mqtt_msg_arrived(self, response); });
        } else {
            m_status = -1;
            m_msg    = "failure";
            send_to_js();
            finish_job();
        }

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
    } else if (m_cmd == "sw_GetConnectedMachine") {
        sw_get_connect_machine();
    } else if (m_cmd == "sw_ConnectOtherMachine"){
        sw_connect_other_device();
    } else {
        this->m_status = -1;
        this->m_msg    = "failure";
        this->send_to_js();
        this->finish_job();
    }
}

void SSWCP_MachineConnect_Instance::sw_connect_other_device() {
    try {
        auto self = shared_from_this();
        wxGetApp().CallAfter([self](){
            
            auto config = &wxGetApp().preset_bundle->printers.get_edited_preset().config;
            config->set("print_host", "");
            config->set("printhost_apikey", "");

            auto dialog = SMPhysicalPrinterDialog(wxGetApp().mainframe->plater()->GetParent()); //  todo
            int res = dialog.ShowModal();

            dialog.EndModal(1);

            if (dialog.m_connected) {
                auto device_dialog = wxGetApp().get_web_device_dialog();
                if (device_dialog) {
                    device_dialog->EndModal(1);
                }
            } else {
                self->m_status = -1;
                self->m_msg    = "failed!";
                self->send_to_js();
                self->finish_job();
            }
            
            
        });
    }
    catch (std::exception& e) {}
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
        // 兼容第三方机器连接
        if (m_param_data.count("from_homepage") && m_param_data["from_homepage"].get<bool>() && m_param_data.count("sn") &&
            m_param_data["sn"].get<std::string>() == "-1") {

            auto self = shared_from_this();

            wxGetApp().CallAfter([self]() {
                // 从首页触发的请求，没有sn的一律认为是第三方机器

                std::string dev_id = self->m_param_data.count("dev_id") ? self->m_param_data["dev_id"].get<std::string>() : "";
                if (dev_id == "") {
                    // 失败
                    self->m_status = -1;
                    self->m_msg    = "failed";
                    self->finish_job();
                    return;
                }

                auto cfg = &wxGetApp().preset_bundle->printers.get_edited_preset().config;
                auto devices = wxGetApp().app_config->get_devices();
                for (const auto& device : devices) {
                    if (device.dev_id == dev_id) {
                        cfg->option<ConfigOptionEnum<PrintHostType>>("host_type")->value = PrintHostType(device.protocol);
                        cfg->set("print_host", device.ip);
                        cfg->set("printhost_apikey", device.api_key);
                        break;
                    }
                }

                SMPhysicalPrinterDialog dialog = SMPhysicalPrinterDialog(wxGetApp().mainframe->plater()->GetParent());
                dialog.ShowModal();

                if (!dialog.m_connected) {
                    self->m_status = -1;
                    self->m_msg    = "failed!";
                    self->send_to_js();
                    self->finish_job();
                } 
            });
           
        } else {
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

                auto config = wxGetApp().preset_bundle->printers.get_edited_preset().config;

                PrintHostType type = PrintHostType::htMoonRaker_mqtt;
                // todo : 增加输入与type的映射

                config.option<ConfigOptionEnum<PrintHostType>>("host_type")->value = type;

                config.set("print_host", ip + (port == -1 ? "" : ":" + std::to_string(port)));

                std::shared_ptr<PrintHost> host(PrintHost::get_print_host(&config));
                wxGetApp().set_connect_host(host);
                wxGetApp().set_host_config(config);

                json connect_params;
                if (m_param_data.count("sn") && m_param_data["sn"].is_string()) {
                    connect_params["sn"] = m_param_data["sn"].get<std::string>();
                }

                if (!host) {
                    // 错误处理
                    finish_job();
                } else {
                    auto self     = shared_from_this();
                    m_work_thread = std::thread([self, host, connect_params] {
                        wxString msg = "";
                        json     params;
                        bool     res = host->connect(msg, connect_params);

                        std::string ip_port = host->get_host();
                        if (res) {
                            int         pos = ip_port.find(':');
                            std::string ip  = ip_port;
                            if (pos != std::string::npos) {
                                ip = ip_port.substr(0, pos);
                            }

                            // 更新其他设备连接状态为断开
                            auto devices = wxGetApp().app_config->get_devices();
                            for (size_t i = 0; i < devices.size(); ++i) {
                                if (devices[i].connected) {
                                    devices[i].connected = false;
                                    wxGetApp().app_config->save_device_info(devices[i]);
                                    break;
                                }
                            }

                            wxGetApp().CallAfter([ip, connect_params, self]() {
                                // 查询机器的机型和喷嘴信息
                                std::string              machine_type = "";
                                std::vector<std::string> nozzle_diameters;

                                std::shared_ptr<PrintHost> host = nullptr;
                                wxGetApp().get_connect_host(host);

                                if (!host->check_sn_arrived()) {
                                    wxGetApp().CallAfter([ip]() {
                                        MessageDialog msg_window(nullptr, ip + _L(" connected unseccessfully !\n"), L("Failed"),
                                                                 wxICON_QUESTION | wxOK);
                                        msg_window.ShowModal();
                                    });

                                    self->m_status = 1;
                                    self->m_msg    = "sn is not exist";

                                    self->send_to_js();
                                    self->finish_job();
                                    return;
                                }

                                if (SSWCP::query_machine_info(host, machine_type, nozzle_diameters) && machine_type != "") {
                                    // 查询成功
                                    if (nozzle_diameters.empty()) {
                                        MessageDialog msg_window(nullptr,
                                                                 ip + _L(" The target machine model has been detected as ") + machine_type +
                                                                     "\n" + _L("Please bind the nozzle information\n"),
                                                                 L("Nozzle Bind"), wxICON_QUESTION | wxOK);
                                        msg_window.ShowModal();

                                        auto dialog          = WebPresetDialog(&wxGetApp());
                                        dialog.m_bind_nozzle = true;
                                        dialog.m_device_id   = ip;
                                        dialog.run();
                                    }

                                    DeviceInfo info;
                                    info.ip           = ip;
                                    info.dev_id       = ip;
                                    info.dev_name     = ip;
                                    info.connected    = true;
                                    info.model_name   = machine_type;
                                    info.nozzle_sizes = nozzle_diameters;
                                    info.protocol     = int(PrintHostType::htMoonRaker_mqtt);
                                    if (connect_params.count("sn") && connect_params["sn"].is_string()) {
                                        info.sn = connect_params["sn"].get<std::string>();
                                    }

                                    size_t vendor_pos = machine_type.find_first_of(" ");
                                    if (vendor_pos != std::string::npos) {
                                        std::string vendor        = machine_type.substr(0, vendor_pos);
                                        std::string machine_cover = LOCALHOST_URL + std::to_string(PAGE_HTTP_PORT) + "/profiles/" + vendor +
                                                                    "/" + machine_type + "_cover.png";
                                        info.img = machine_cover;
                                    }

                                    // 设置预设名称（使用第一个喷嘴尺寸）
                                    if (!nozzle_diameters.empty()) {
                                        info.preset_name = machine_type + " (" + nozzle_diameters[0] + " nozzle)";
                                    } else {
                                        info.preset_name = machine_type + " (0.4 nozzle)"; // 默认值
                                    }

                                    wxGetApp().app_config->save_device_info(info);
                                } else {
                                    // 是否为连接过的设备
                                    DeviceInfo query_info;
                                    if (wxGetApp().app_config->get_device_info(ip, query_info)) {
                                        query_info.connected = true;
                                        wxGetApp().app_config->save_device_info(query_info);
                                    } else {
                                        auto machine_ip_type = MachineIPType::getInstance();
                                        if (machine_ip_type) {
                                            std::string machine_type = "";
                                            if (machine_ip_type->get_machine_type(ip, machine_type)) {
                                                // 已经发现过的机型信息
                                                //// test
                                                // if (machine_type == "lava") {
                                                //     machine_type = "Snapmaker A250 BKit";
                                                // }

                                                DeviceInfo info;
                                                info.ip         = ip;
                                                info.dev_id     = ip;
                                                info.dev_name   = ip;
                                                info.connected  = true;
                                                info.model_name = machine_type;
                                                info.protocol   = int(PrintHostType::htMoonRaker_mqtt);
                                                if (connect_params.count("sn") && connect_params["sn"].is_string()) {
                                                    info.sn = connect_params["sn"].get<std::string>();
                                                }

                                                size_t vendor_pos = machine_type.find_first_of(" ");
                                                if (vendor_pos != std::string::npos) {
                                                    std::string vendor        = machine_type.substr(0, vendor_pos);
                                                    std::string machine_cover = LOCALHOST_URL + std::to_string(PAGE_HTTP_PORT) +
                                                                                "/profiles/" + vendor + "/" + machine_type + "_cover.png";
                                                    info.img = machine_cover;
                                                }

                                                wxGetApp().app_config->save_device_info(info);
                                                // todo 绑定喷嘴

                                                MessageDialog msg_window(nullptr,
                                                                         ip + _L(" The target machine model has been detected as ") +
                                                                             machine_type + "\n" +
                                                                             _L("Please bind the nozzle information\n"),
                                                                         L("Nozzle Bind"), wxICON_QUESTION | wxOK);
                                                msg_window.ShowModal();
                                                auto dialog          = WebPresetDialog(&wxGetApp());
                                                dialog.m_bind_nozzle = true;
                                                dialog.m_device_id   = ip;
                                                dialog.run();

                                                if (info.nozzle_sizes.empty())
                                                    info.nozzle_sizes.push_back("0.4");

                                                info.preset_name = machine_type + " (" + info.nozzle_sizes[0] + " nozzle)";

                                                wxGetApp().app_config->save_device_info(info);
                                            } else {
                                                DeviceInfo info;
                                                info.ip        = ip;
                                                info.dev_id    = ip;
                                                info.dev_name  = ip;
                                                info.connected = true;
                                                info.protocol  = int(PrintHostType::htMoonRaker_mqtt);
                                                if (connect_params.count("sn") && connect_params["sn"].is_string()) {
                                                    info.sn = connect_params["sn"].get<std::string>();
                                                }
                                                wxGetApp().app_config->save_device_info(info);
                                                MessageDialog msg_window(
                                                    nullptr,
                                                    ip + _L(" The target machine model has not been detected. Please bind manually. "),
                                                    L("Machine Bind"), wxICON_QUESTION | wxOK);
                                                msg_window.ShowModal();
                                                auto dialog        = WebPresetDialog(&wxGetApp());
                                                dialog.m_device_id = ip;
                                                dialog.run();
                                            }
                                        }
                                    }
                                }

                                // 更新首页设备卡片
                                auto devices = wxGetApp().app_config->get_devices();

                                json param;
                                param["command"]       = "local_devices_arrived";
                                param["sequece_id"]    = "10001";
                                param["data"]          = devices;
                                std::string logout_cmd = param.dump();
                                wxString    strJS      = wxString::Format("window.postMessage(%s)", logout_cmd);
                                GUI::wxGetApp().run_script(strJS);

                                MessageDialog msg_window(nullptr, ip + _L(" connected sucessfully !\n"), L("Machine Connected"),
                                                         wxICON_QUESTION | wxOK);
                                msg_window.ShowModal();

                                auto dialog = wxGetApp().get_web_device_dialog();
                                if (dialog) {
                                    dialog->EndModal(1);
                                }

                                wxGetApp().app_config->set("use_new_connect", "true");
                                wxGetApp().mainframe->plater()->sidebar().update_all_preset_comboboxes();
                                wxGetApp().mainframe->m_print_enable = true;
                                wxGetApp().mainframe->update_slice_print_status(MainFrame::eEventPlateUpdate);
                                // wxGetApp().mainframe->load_printer_url("http://" + ip);  //到时全部加载本地交互页面
                                wxGetApp().mainframe->load_printer_url(
                                    "http://localhost:13619/web/flutter_web/index.html?path=device_control"); // 到时全部加载本地交互页面
                            });

                        } else {
                            wxGetApp().CallAfter([ip_port]() {
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
        }
        
    } catch (std::exception& e) {}
}

void SSWCP_MachineConnect_Instance::sw_get_connect_machine() {
    try {
        auto devices = wxGetApp().app_config->get_devices();
        for (const auto& device : devices) {
            if (device.connected) {
                m_res_data = device;
                break;
            }
        }

        send_to_js();
        finish_job();
    }
    catch (std::exception& e) {

    }
}

void SSWCP_MachineConnect_Instance::sw_disconnect() {
    auto self = shared_from_this();
    m_work_thread = std::thread([self](){
        std::shared_ptr<PrintHost> host = nullptr;
        wxGetApp().get_connect_host(host);
        wxString msg = "";

        bool res = false;
        if (host == nullptr) {
            res = true;
        } else {
            res = host->disconnect(msg, {});
        }
        
        if (res) {
            wxGetApp().CallAfter([]() {
                wxGetApp().app_config->set("use_new_connect", "false");
                auto p_config = &(wxGetApp().preset_bundle->printers.get_edited_preset().config);
                p_config->set("print_host", "");

                auto devices = wxGetApp().app_config->get_devices();
                for (size_t i = 0; i < devices.size(); ++i) {
                    if (devices[i].connected) {
                        devices[i].connected = false;
                        wxGetApp().app_config->save_device_info(devices[i]);
                        break;
                    }
                }

                json param;
                param["command"]       = "local_devices_arrived";
                param["sequece_id"]    = "10001";
                param["data"]          = devices;
                std::string logout_cmd = param.dump();
                wxString    strJS      = wxString::Format("window.postMessage(%s)", logout_cmd);
                GUI::wxGetApp().run_script(strJS);

                MessageDialog msg_window(nullptr, _L(" Disconnected sucessfully !\n"), L("Machine Disconnected"), wxICON_QUESTION | wxOK);
                msg_window.ShowModal();

                wxGetApp().mainframe->plater()->sidebar().update_all_preset_comboboxes();
                wxGetApp().set_connect_host(nullptr);
            });

        } else {
            wxGetApp().CallAfter([]() {
                MessageDialog msg_window(nullptr, _L(" Disconnect Failed !\n"), L("Machine Disconnected"), wxICON_QUESTION | wxOK);
                msg_window.ShowModal(); 
            });
            

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
    "sw_GetPrinterInfo",
    "sw_MachinePrintStart",
    "sw_MachinePrintPause",
    "sw_MachinePrintResume",
    "sw_MachinePrintCancel",
    "sw_GetMachineSystemInfo",
    "sw_MachineFilesRoots",
    "sw_MachineFilesMetadata",
    "sw_MachineFilesThumbnails",
    "sw_MachineFilesGetDirectory",
    "sw_CameraStartMonitor",
    "sw_CameraStopMonitor",
};

std::unordered_set<std::string> SSWCP::m_machine_connect_cmd_list = {
    "sw_Test_connect",
    "sw_Connect",
    "sw_Disconnect",
    "sw_GetConnectedMachine",
    "sw_ConnectOtherMachine",
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

// query the info of the machine
bool SSWCP::query_machine_info(std::shared_ptr<PrintHost>& host, std::string& out_model, std::vector<std::string>& out_nozzle_diameters, int timeout_second)
{
    if (!host) return false;

    // 创建同步等待的条件变量和互斥锁
    std::condition_variable cv;
    std::shared_ptr<std::mutex> mutex(new std::mutex);
    std::weak_ptr<std::mutex>   cb_mutex = mutex;
    bool received = false;
    bool timeout = false;
    json system_info;

    // 发送查询请求
    host->async_get_system_info(
        [&, cb_mutex](const json& response) {
            if (cb_mutex.expired()) {
                return;
            }
            std::lock_guard<std::mutex> lock(*mutex);
            if (!response.is_null() && !response.count("error")) {
                system_info = response;
            }
            received = true;
            cv.notify_one();
        }
    );

    // 等待响应
    {
        std::unique_lock<std::mutex> lock(*mutex);
        auto predicate = [&received]() { return received; };
        timeout = !cv.wait_for(lock, std::chrono::seconds(timeout_second), predicate);
    }

    if (!timeout && !system_info.is_null()) {
        // 成功获取到信息
        if (system_info.count("data")) {
            system_info = system_info["data"];
        }
        if (system_info.contains("system_info")) {
            auto& system_data = system_info["system_info"];
            
            if(system_data.contains("product_info")){
                auto& product_info = system_data["product_info"];

                // 获取机型
                if(product_info.contains("machine_type")){
                    out_model = product_info["machine_type"].get<std::string>();
                }

                // 获取喷嘴信息
                if(product_info.contains("nozzle_diameter")){
                    if(product_info["nozzle_diameter"].is_array()){
                        for(const auto& nozzle : product_info["nozzle_diameter"]){
                            // todo 不一定是string
                            out_nozzle_diameters.push_back(nozzle.get<std::string>());
                        }
                    }
                    else {
                        // 如果是单个值
                        out_nozzle_diameters.push_back(
                        product_info["nozzle_diameter"].get<std::string>());
                    }
                }
            }

            return true;
        }
    }
    
    return false;
}

MachineIPType* MachineIPType::getInstance()
{
    static MachineIPType mipt_instance;
    return &mipt_instance;
}


}}; // namespace Slic3r::GUI


