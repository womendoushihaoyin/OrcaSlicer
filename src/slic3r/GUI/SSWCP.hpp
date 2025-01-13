#ifndef SSWCP_HPP
#define SSWCP_HPP

#include <iostream>
#include <mutex>
#include <stack>

#include <wx/webview.h>
#include <unordered_set>
#include <unordered_map>
#include "nlohmann/json.hpp"
#include "Bonjour.hpp"


using namespace nlohmann;

namespace Slic3r { namespace GUI {

class SSWCP_Instance
{
public:
    enum INSTANCE_TYPE {
        COMMON,
        MACHINE_FIND,
        MACHINE_CONNECT,
        MACHINE_OPTION,
    };

public:
    SSWCP_Instance(std::string cmd, const json& header, const json& data, std::string event_id, wxWebView* webview)
        : m_cmd(cmd), m_header(header), m_webview(webview), m_event_id(event_id), m_param_data(data)
    {}

    virtual ~SSWCP_Instance() {}

    virtual void process();

    virtual void send_to_js();

    virtual void finish_job();

    virtual INSTANCE_TYPE getType() { return m_type; }

    virtual bool is_stop() { return false; }
    virtual void set_stop(bool stop) {}


    virtual void set_Instance_illegal();
    virtual bool is_Instance_illegal();

    wxWebView* get_web_view() const;

private:
    void sync_test();
    void async_test();

public:
    std::string m_cmd           = "";
    json        m_header;
    wxWebView* m_webview     = nullptr;
    std::string m_event_id = "";
    json        m_param_data;

    json m_res_data;
    int  m_status = 200;
    std::string m_msg  = "OK";

protected:
    INSTANCE_TYPE m_type = COMMON;

private:
    bool m_illegal = false;
    std::mutex m_illegal_mtx;
};

class SSWCP_MachineConnect_Instance : public SSWCP_Instance
{
public:
    SSWCP_MachineConnect_Instance(std::string cmd, const json& header, const json& data, std::string event_id, wxWebView* webview)
        : SSWCP_Instance(cmd, header, data, event_id, webview)
    {
        m_type = MACHINE_CONNECT;
    }

    ~SSWCP_MachineConnect_Instance()
    {
        if (m_work_thread.joinable())
            m_work_thread.detach();
    }

    void process() override;

private:
    void sw_test_connect();

    void sw_connect();

    void sw_disconnect();

private:
    std::thread m_work_thread;
};

class SSWCP_MachineFind_Instance : public SSWCP_Instance
{
public:
    SSWCP_MachineFind_Instance(std::string cmd, const json& header, const json& data, std::string event_id, wxWebView* webview)
        : SSWCP_Instance(cmd, header, data, event_id, webview)
    {
        m_type = MACHINE_FIND;
    }

    void process() override;

    bool is_stop() { return m_stop; }
    void set_stop(bool stop);

private:
    void sw_GetMachineFindSupportInfo();
    void sw_StartMachineFind();
    void sw_StopMachineFind();

private:
    void add_machine_to_list(const json& machine_info);
    void onOneEngineEnd();

private:
    std::mutex m_machine_list_mtx;
    std::mutex m_stop_mtx;

private:
    std::unordered_map<std::string, json> m_machine_list;

private:
    int                                   m_engine_end_count = 0;
    std::vector<std::shared_ptr<Bonjour>> m_engines;
    bool                                  m_stop = false;
};

class SSWCP_MachineOption_Instance : public SSWCP_Instance
{
public:
    SSWCP_MachineOption_Instance(std::string cmd, const json& header, const json& data, std::string event_id, wxWebView* webview)
        : SSWCP_Instance(cmd, header, data, event_id, webview)
    {
        m_type = MACHINE_OPTION;
    }

    ~SSWCP_MachineOption_Instance()
    {
        if (m_work_thread.joinable())
            m_work_thread.detach();
    }

    void process() override;

private:
    void sw_SendGCodes();

    void sw_GetMachineState();

    void sw_SubscribeMachineState();

private:
    std::thread m_work_thread;
};

class SSWCP
{
public:
    static void handle_web_message(std::string message, wxWebView* webview);

    static std::shared_ptr<SSWCP_Instance> create_sswcp_instance(
        std::string cmd, const json& header, const json& data, std::string event_id, wxWebView* webview);

    static void delete_target(SSWCP_Instance* target);

    static void stop_machine_find();

    static void on_webview_delete(wxWebView* webview);
    
private:

    static std::unordered_set<std::string>                                      m_machine_find_cmd_list;
    static std::unordered_set<std::string>                                      m_machine_option_cmd_list;
    static std::unordered_set<std::string>                                      m_machine_connect_cmd_list;
    static std::unordered_map<SSWCP_Instance*, std::shared_ptr<SSWCP_Instance>> m_instance_list;
}; 



}};

#endif
