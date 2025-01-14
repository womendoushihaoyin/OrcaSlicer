#ifndef slic3r_Moonraker_hpp_
#define slic3r_Moonraker_hpp_

#include <string>
#include <wx/string.h>
#include <boost/optional.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>

#include "PrintHost.hpp"
#include "libslic3r/PrintConfig.hpp"

class MqttClient;

namespace Slic3r {

class DynamicPrintConfig;
class Http;

class Moonraker : public PrintHost
{
public:
    Moonraker(DynamicPrintConfig *config);
    ~Moonraker() override = default;

    const char* get_name() const override;

    virtual bool test(wxString &curl_msg) const override;
    wxString get_test_ok_msg () const override;
    wxString get_test_failed_msg (wxString &msg) const override;
    bool upload(PrintHostUpload upload_data, ProgressFn prorgess_fn, ErrorFn error_fn, InfoFn info_fn) const override;
    bool                       send_gcodes(const std::vector<std::string>& codes, std::string& extraInfo) override;
    bool                       get_machine_info(const std::vector<std::pair<std::string, std::vector<std::string>>>& targets, nlohmann::json& response) override;
    bool has_auto_discovery() const override { return true; }
    bool can_test() const override { return true; }
    PrintHostPostUploadActions get_post_upload_actions() const override { return PrintHostPostUploadAction::StartPrint; }
    std::string get_host() const override { return m_host; }
    const std::string& get_apikey() const { return m_apikey; }
    const std::string& get_cafile() const { return m_cafile; }

    virtual bool connect(wxString& msg, const nlohmann::json& params) override;
    virtual bool disconnect(wxString& msg, const nlohmann::json& params) override { return true; }
    virtual void async_get_machine_info(const std::vector<std::pair<std::string, std::vector<std::string>>>& targets, std::function<void(const nlohmann::json& response)>) override  {}
    virtual void async_subscribe_machine_info(std::function<void(const nlohmann::json&)>) override {}
    virtual void async_get_machine_objects(std::function<void(const nlohmann::json& response)>)override {}
    virtual void async_set_machine_subscribe_filter(const std::vector<std::pair<std::string, std::vector<std::string>>>& targets,
                                                    std::function<void(const nlohmann::json& response)>                  callback) override {}
    virtual void async_unsubscribe_machine_info(std::function<void(const nlohmann::json&)>) override {}
    virtual void async_send_gcodes(const std::vector<std::string>& scripts, std::function<void(const nlohmann::json&)>) override{}

protected:
#ifdef WIN32
    virtual bool upload_inner_with_resolved_ip(PrintHostUpload upload_data, ProgressFn prorgess_fn, ErrorFn error_fn, InfoFn info_fn, const boost::asio::ip::address& resolved_addr) const;
#endif
    virtual bool validate_version_text(const boost::optional<std::string> &version_text) const;
    virtual bool upload_inner_with_host(PrintHostUpload upload_data, ProgressFn prorgess_fn, ErrorFn error_fn, InfoFn info_fn) const;

    std::string m_host;
    std::string m_apikey;
    std::string m_cafile;
    bool        m_ssl_revoke_best_effort;

    virtual void set_auth(Http &http) const;
    std::string make_url(const std::string &path) const;

private:
#ifdef WIN32
    bool test_with_resolved_ip(wxString& curl_msg) const;
#endif
};



class Moonraker_Mqtt : public Moonraker
{
public:
    Moonraker_Mqtt(DynamicPrintConfig* config);

    virtual bool connect(wxString& msg, const nlohmann::json& params) override;

    virtual bool disconnect(wxString& msg, const nlohmann::json& params) override;

    virtual void async_get_machine_info(const std::vector<std::pair<std::string, std::vector<std::string>>>& targets, std::function<void(const nlohmann::json& response)> callback) override;

    virtual void async_subscribe_machine_info(std::function<void(const nlohmann::json&)>) override;

    virtual void async_get_machine_objects(std::function<void(const nlohmann::json& response)> callback) override;

    virtual void async_set_machine_subscribe_filter(const std::vector<std::pair<std::string, std::vector<std::string>>>& targets,
                                                    std::function<void(const nlohmann::json& response)>                  callback) override;

    virtual void async_unsubscribe_machine_info(std::function<void(const nlohmann::json&)>) override;

    virtual void async_send_gcodes(const std::vector<std::string>& scripts, std::function<void(const nlohmann::json&)>) override;

public:
    void on_mqtt_message_arrived(const std::string& topic, const std::string& payload);

private:
    bool send_to_request(const std::string& method, const nlohmann::json& params, bool need_response = false, std::function<void(const nlohmann::json& response)> callback = nullptr);

    bool add_response_target(const std::string& id, std::function<void(const nlohmann::json& response)> callback);

    std::function<void(const nlohmann::json& response)> get_request_callback(const std::string& id);

    void delete_response_target(const std::string& id);

    void on_response_arrived(const std::string& payload);

    void on_status_arrived(const std::string& payload);

    void on_notification_arrived(const std::string& payload);


private:
    static MqttClient* m_mqtt_client;

    static std::unordered_map<std::string, std::function<void(const nlohmann::json&)>> m_request_cb_map;
    static std::timed_mutex                                                                     m_cb_map_mtx;
    static std::function<void(const nlohmann::json&)>                                  m_status_cb;


    static std::string m_response_topic;
    static std::string m_status_topic;
    static std::string m_notification_topic;
    static std::string m_request_topic;

    static std::string m_sn;

private:
    boost::uuids::random_generator m_generator; // 用于生成唯一id
};

}

#endif
