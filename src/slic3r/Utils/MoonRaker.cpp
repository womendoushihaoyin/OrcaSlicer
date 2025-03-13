// Implementation of Moonraker printer host communication
#include "MoonRaker.hpp"
#include "MQTT.hpp"

#include <algorithm>
#include <sstream>
#include <exception>
#include <boost/format.hpp>
#include <boost/log/trivial.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/asio.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/nowide/convert.hpp>
#include <curl/curl.h>

#include <wx/progdlg.h>

#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/format.hpp"
#include "Http.hpp"
#include "libslic3r/AppConfig.hpp"
#include "Bonjour.hpp"
#include "slic3r/GUI/BonjourDialog.hpp"

namespace fs = boost::filesystem;
namespace pt = boost::property_tree;


namespace Slic3r {

namespace {

#ifdef WIN32
// Helper function to extract host and port from URL
std::string get_host_from_url(const std::string& url_in)
{
    std::string url = url_in;
    // Add http:// if there is no scheme
    size_t double_slash = url.find("//");
    if (double_slash == std::string::npos)
        url = "http://" + url;
    std::string out  = url;
    CURLU*      hurl = curl_url();
    if (hurl) {
        // Parse the input URL
        CURLUcode rc = curl_url_set(hurl, CURLUPART_URL, url.c_str(), 0);
        if (rc == CURLUE_OK) {
            // Extract host and port
            char* host;
            rc = curl_url_get(hurl, CURLUPART_HOST, &host, 0);
            if (rc == CURLUE_OK) {
                char* port;
                rc = curl_url_get(hurl, CURLUPART_PORT, &port, 0);
                if (rc == CURLUE_OK && port != nullptr) {
                    out = std::string(host) + ":" + port;
                    curl_free(port);
                } else {
                    out = host;
                    curl_free(host);
                }
            } else
                BOOST_LOG_TRIVIAL(error) << "Moonraker get_host_from_url: failed to get host form URL " << url;
        } else
            BOOST_LOG_TRIVIAL(error) << "Moonraker get_host_from_url: failed to parse URL " << url;
        curl_url_cleanup(hurl);
    } else
        BOOST_LOG_TRIVIAL(error) << "Moonraker get_host_from_url: failed to allocate curl_url";
    return out;
}

// Workaround for Windows 10/11 mDNS resolve issue, where two mDNS resolves in succession fail.
std::string substitute_host(const std::string& orig_addr, std::string sub_addr)
{
    // put ipv6 into [] brackets
    if (sub_addr.find(':') != std::string::npos && sub_addr.at(0) != '[')
        sub_addr = "[" + sub_addr + "]";

#if 0
    //URI = scheme ":"["//"[userinfo "@"] host [":" port]] path["?" query]["#" fragment]
    std::string final_addr = orig_addr;
    //  http
    size_t double_dash = orig_addr.find("//");
    size_t host_start = (double_dash == std::string::npos ? 0 : double_dash + 2);
    // userinfo
    size_t at = orig_addr.find("@");
    host_start = (at != std::string::npos && at > host_start ? at + 1 : host_start);
    // end of host, could be port(:), subpath(/) (could be query(?) or fragment(#)?)
    // or it will be ']' if address is ipv6 )
    size_t potencial_host_end = orig_addr.find_first_of(":/", host_start); 
    // if there are more ':' it must be ipv6
    if (potencial_host_end != std::string::npos && orig_addr[potencial_host_end] == ':' && orig_addr.rfind(':') != potencial_host_end) {
        size_t ipv6_end = orig_addr.find(']', host_start);
        // DK: Uncomment and replace orig_addr.length() if we want to allow subpath after ipv6 without [] parentheses.
        potencial_host_end = (ipv6_end != std::string::npos ? ipv6_end + 1 : orig_addr.length()); //orig_addr.find('/', host_start));
    }
    size_t host_end = (potencial_host_end != std::string::npos ? potencial_host_end : orig_addr.length());
    // now host_start and host_end should mark where to put resolved addr
    // check host_start. if its nonsense, lets just use original addr (or  resolved addr?)
    if (host_start >= orig_addr.length()) {
        return final_addr;
    }
    final_addr.replace(host_start, host_end - host_start, sub_addr);
    return final_addr;
#else
    // Using the new CURL API for handling URL. https://everything.curl.dev/libcurl/url
    // If anything fails, return the input unchanged.
    std::string out  = orig_addr;
    CURLU*      hurl = curl_url();
    if (hurl) {
        // Parse the input URL.
        CURLUcode rc = curl_url_set(hurl, CURLUPART_URL, orig_addr.c_str(), 0);
        if (rc == CURLUE_OK) {
            // Replace the address.
            rc = curl_url_set(hurl, CURLUPART_HOST, sub_addr.c_str(), 0);
            if (rc == CURLUE_OK) {
                // Extract a string fromt the CURL URL handle.
                char* url;
                rc = curl_url_get(hurl, CURLUPART_URL, &url, 0);
                if (rc == CURLUE_OK) {
                    out = url;
                    curl_free(url);
                } else
                    BOOST_LOG_TRIVIAL(error) << "Moonraker substitute_host: failed to extract the URL after substitution";
            } else
                BOOST_LOG_TRIVIAL(error) << "Moonraker substitute_host: failed to substitute host " << sub_addr << " in URL " << orig_addr;
        } else
            BOOST_LOG_TRIVIAL(error) << "Moonraker substitute_host: failed to parse URL " << orig_addr;
        curl_url_cleanup(hurl);
    } else
        BOOST_LOG_TRIVIAL(error) << "Moonraker substitute_host: failed to allocate curl_url";
    return out;
#endif
}
#endif // WIN32

// Helper function to URL encode a string
std::string escape_string(const std::string& unescaped)
{
    std::string ret_val;
    CURL*       curl = curl_easy_init();
    if (curl) {
        char* decoded = curl_easy_escape(curl, unescaped.c_str(), unescaped.size());
        if (decoded) {
            ret_val = std::string(decoded);
            curl_free(decoded);
        }
        curl_easy_cleanup(curl);
    }
    return ret_val;
}

// Helper function to URL encode each element of a filesystem path
std::string escape_path_by_element(const boost::filesystem::path& path)
{
    std::string             ret_val = escape_string(path.filename().string());
    boost::filesystem::path parent(path.parent_path());
    while (!parent.empty() &&
           parent.string() != "/") // "/" check is for case "/file.gcode" was inserted. Then boost takes "/" as parent_path.
    {
        ret_val = escape_string(parent.filename().string()) + "/" + ret_val;
        parent  = parent.parent_path();
    }
    return ret_val;
}
} // namespace

Moonraker::Moonraker(DynamicPrintConfig* config)
    : m_host(config->opt_string("print_host"))
    , m_apikey(config->opt_string("printhost_apikey"))
    , m_cafile(config->opt_string("printhost_cafile"))
    , m_ssl_revoke_best_effort(config->opt_bool("printhost_ssl_ignore_revoke"))
{}

// Return the name of this print host type
const char* Moonraker::get_name() const { return "Moonraker"; }
#ifdef WIN32
bool Moonraker::test_with_resolved_ip(wxString& msg) const
{
    // Since the request is performed synchronously here,
    // it is ok to refer to `msg` from within the closure
    const char* name = get_name();
    bool        res  = true;
    // Msg contains ip string.
    auto url = substitute_host(make_url("api/version"), GUI::into_u8(msg));
    msg.Clear();

    BOOST_LOG_TRIVIAL(info) << boost::format("%1%: Get version at: %2%") % name % url;

    std::string host = get_host_from_url(m_host);
    auto        http = Http::get(url); // std::move(url));
    // "Host" header is necessary here. We have resolved IP address and subsituted it into "url" variable.
    // And when creating Http object above, libcurl automatically includes "Host" header from address it got.
    // Thus "Host" is set to the resolved IP instead of host filled by user. We need to change it back.
    // Not changing the host would work on the most cases (where there is 1 service on 1 hostname) but would break when f.e. reverse proxy
    // is used (issue #9734). Also when allow_ip_resolve = 0, this is not needed, but it should not break anything if it stays.
    // https://www.rfc-editor.org/rfc/rfc7230#section-5.4
    http.header("Host", host);
    set_auth(http);
    http.on_error([&](std::string body, std::string error, unsigned status) {
            BOOST_LOG_TRIVIAL(error) << boost::format("%1%: Error getting version at %2% : %3%, HTTP %4%, body: `%5%`") % name % url %
                                            error % status % body;
            res = false;
            msg = format_error(body, error, status);
        })
        .on_complete([&, this](std::string body, unsigned) {
            BOOST_LOG_TRIVIAL(info) << boost::format("%1%: Got version: %2%") % name % body;

            try {
                std::stringstream ss(body);
                pt::ptree         ptree;
                pt::read_json(ss, ptree);

                if (!ptree.get_optional<std::string>("api")) {
                    res = false;
                    return;
                }

                const auto text = ptree.get_optional<std::string>("text");
                res             = validate_version_text(text);
                if (!res) {
                    msg = GUI::format_wxstr(_L("Mismatched type of print host: %s"), (text ? *text : name));
                }
            } catch (const std::exception&) {
                res = false;
                msg = "Could not parse server response.";
            }
        })
        .ssl_revoke_best_effort(m_ssl_revoke_best_effort)
        .perform_sync();

    return res;
}
#endif // WIN32

bool Moonraker::get_machine_info(const std::vector<std::pair<std::string, std::vector<std::string>>>& targets, json& response) {
    bool res = true;

    auto url = make_url("printer/objects/query");
    auto http = Http::post(std::move(url));

    for (const auto pair : targets) {
        std::string value = "";
        for (size_t i = 0; i < pair.second.size(); ++i) {
            if (i != 0) {
                value += ",";
            }
            value += pair.second[i];
        }
        http.form_add(pair.first, value);
    }

    http.on_error([&](std::string body, std::string error, unsigned status) {
            res = false;
            try{
                response = json::parse(body);
            } catch (std::exception& e) {}
        })
        .on_complete([&](std::string body, unsigned) {
            try {
                response = json::parse(body);
            } catch (std::exception& e) {}
        })
        .perform_sync();

    return res;
}

bool Moonraker::send_gcodes(const std::vector<std::string>& codes, std::string& extraInfo)
{
    bool res = true;
    std::string param = "?script=";
    for (const auto code : codes) {
        param += "\n";
        param += code;
    }
    auto url = make_url("printer/gcode/script");
    auto http = Http::post(std::move(url));

    http.form_add("script", param)
        .on_error([&](std::string body, std::string error, unsigned status) {
            res = false;    
        })
        .on_complete([&](std::string body, unsigned){
            res = true;
        })
        .perform_sync();

    return res;
}

bool Moonraker::test(wxString& msg) const
{
    //// test
    //MqttClient* mqttClient = new MqttClient("mqtt://172.18.0.9:1883", "OrcaClient", false);
    //mqttClient->SetMessageCallback([](const std::string& topic, const std::string& payload) {
    //    int a = 0;
    //    int b = 0;
    //});
    //mqttClient->Connect();
    //mqttClient->Subscribe("9BD4E436F33D0B56/response", 2);
    //mqttClient->Subscribe("9BD4E436F33D0B56/status", 2);
    //mqttClient->Publish("9BD4E436F33D0B56/request", "{\"jsonrpc\": \"2.0\",\"method\": \"server.info\",\"id\": 9546}", 1);
    
    // Since the request is performed synchronously here,
    // it is ok to refer to `msg` from within the closure
    const char* name = get_name();

    bool res = true;
    auto url = make_url("printer/info");

    BOOST_LOG_TRIVIAL(info) << boost::format("%1%: Get version at: %2%") % name % url;
    // Here we do not have to add custom "Host" header - the url contains host filled by user and libCurl will set the header by itself.
    auto http = Http::get(std::move(url));
    set_auth(http);
    http.on_error([&](std::string body, std::string error, unsigned status) {
            BOOST_LOG_TRIVIAL(error) << boost::format("%1%: Error getting version: %2%, HTTP %3%, body: `%4%`") % name % error % status %
                                            body;
            res = false;
            msg = format_error(body, error, status);
        })
        .on_complete([&, this](std::string body, unsigned) {
            BOOST_LOG_TRIVIAL(debug) << boost::format("%1%: Got version: %2%") % name % body;

            //try {
            //    std::stringstream ss(body);
            //    pt::ptree         ptree;
            //    pt::read_json(ss, ptree);

            //    if (!ptree.get_optional<std::string>("api")) {
            //        res = false;
            //        return;
            //    }

            //    const auto text = ptree.get_optional<std::string>("text");
            //    // test
            //    //res             = validate_version_text(text);

            //    res = true;
            //    if (!res) {
            //        msg = GUI::format_wxstr(_L("Mismatched type of print host: %s"), (text ? *text : name));
            //    }
            //} catch (const std::exception&) {
            //    res = false;
            //    msg = "Could not parse server response";
            //}
        })
#ifdef WIN32
        .ssl_revoke_best_effort(m_ssl_revoke_best_effort)
        .on_ip_resolve([&](std::string address) {
            // Workaround for Windows 10/11 mDNS resolve issue, where two mDNS resolves in succession fail.
            // Remember resolved address to be reused at successive REST API call.
            msg = GUI::from_u8(address);
        })
#endif // WIN32
        .perform_sync();

    return res;
}

// Return success message for connection test
wxString Moonraker::get_test_ok_msg() const { return _(L("Connection to Moonraker works correctly.")); }

// Return formatted error message for failed connection test
wxString Moonraker::get_test_failed_msg(wxString& msg) const
{
    return GUI::format_wxstr("%s: %s\n\n%s", _L("Could not connect to Moonraker"), msg,
                             _L("Note: Moonraker version at least 1.1.0 is required."));
}

// Upload a file to the printer
bool Moonraker::upload(PrintHostUpload upload_data, ProgressFn prorgess_fn, ErrorFn error_fn, InfoFn info_fn) const
{
#ifndef WIN32
    return upload_inner_with_host(std::move(upload_data), prorgess_fn, error_fn, info_fn);
#else
    std::string host = get_host_from_url(m_host);

    // decide what to do based on m_host - resolve hostname or upload to ip
    std::vector<boost::asio::ip::address> resolved_addr;
    boost::system::error_code             ec;
    boost::asio::ip::address              host_ip = boost::asio::ip::make_address(host, ec);
    if (!ec) {
        resolved_addr.push_back(host_ip);
    } else if (GUI::get_app_config()->get_bool("allow_ip_resolve") && boost::algorithm::ends_with(host, ".local")) {
        Bonjour("Moonraker")
            .set_hostname(host)
            .set_retries(5) // number of rounds of queries send
            .set_timeout(1) // after each timeout, if there is any answer, the resolving will stop
            .on_resolve([&ra = resolved_addr](const std::vector<BonjourReply>& replies) {
                for (const auto& rpl : replies) {
                    boost::asio::ip::address ip(rpl.ip);
                    ra.emplace_back(ip);
                    BOOST_LOG_TRIVIAL(info) << "Resolved IP address: " << rpl.ip;
                }
            })
            .resolve_sync();
    }

    // Handle different resolution scenarios
    if (resolved_addr.empty()) {
        // no resolved addresses - try system resolving
        BOOST_LOG_TRIVIAL(error) << "PrusaSlicer failed to resolve hostname " << m_host
                                 << " into the IP address. Starting upload with system resolving.";
        return upload_inner_with_host(std::move(upload_data), prorgess_fn, error_fn, info_fn);
    } else if (resolved_addr.size() == 1) {
        // one address resolved - upload there
        return upload_inner_with_resolved_ip(std::move(upload_data), prorgess_fn, error_fn, info_fn, resolved_addr.front());
    } else if (resolved_addr.size() == 2 && resolved_addr[0].is_v4() != resolved_addr[1].is_v4()) {
        // there are just 2 addresses and 1 is ip_v4 and other is ip_v6
        // try sending to both. (Then if both fail, show both error msg after second try)
        wxString error_message;
        if (!upload_inner_with_resolved_ip(
                std::move(upload_data), prorgess_fn,
                [&msg = error_message, resolved_addr](wxString error) { msg = GUI::format_wxstr("%1%: %2%", resolved_addr.front(), error); },
                info_fn, resolved_addr.front()) &&
            !upload_inner_with_resolved_ip(
                std::move(upload_data), prorgess_fn,
                [&msg = error_message, resolved_addr](wxString error) {
                    msg += GUI::format_wxstr("\n%1%: %2%", resolved_addr.back(), error);
                },
                info_fn, resolved_addr.back())) {
            error_fn(error_message);
            return false;
        }
        return true;
    } else {
        // There are multiple addresses - user needs to choose which to use.
        size_t       selected_index = resolved_addr.size();
        IPListDialog dialog(nullptr, boost::nowide::widen(m_host), resolved_addr, selected_index);
        if (dialog.ShowModal() == wxID_OK && selected_index < resolved_addr.size()) {
            return upload_inner_with_resolved_ip(std::move(upload_data), prorgess_fn, error_fn, info_fn, resolved_addr[selected_index]);
        }
    }
    return false;
#endif // WIN32
}

#ifdef WIN32
// Upload file using resolved IP address
bool Moonraker::upload_inner_with_resolved_ip(PrintHostUpload upload_data, ProgressFn prorgess_fn, ErrorFn error_fn, InfoFn info_fn, const boost::asio::ip::address& resolved_addr) const
{
    info_fn(L"resolve", boost::nowide::widen(resolved_addr.to_string()));

    // If test fails, test_msg_or_host_ip contains the error message.
    // Otherwise on Windows it contains the resolved IP address of the host.
    // Test_msg already contains resolved ip and will be cleared on start of test().
    wxString test_msg_or_host_ip = GUI::from_u8(resolved_addr.to_string());
    if (/* !test_with_resolved_ip(test_msg_or_host_ip)*/ false) {
        error_fn(std::move(test_msg_or_host_ip));
        return false;
    }

    const char* name               = get_name();
    const auto  upload_filename    = upload_data.upload_path.filename();
    const auto  upload_parent_path = upload_data.upload_path.parent_path();
    std::string url                = substitute_host(make_url("server/files/upload"), resolved_addr.to_string());
    bool        result             = true;

    info_fn(L"resolve", boost::nowide::widen(url));

    BOOST_LOG_TRIVIAL(info) << boost::format("%1%: Uploading file %2% at %3%, filename: %4%, path: %5%, print: %6%") % name %
                                   upload_data.source_path % url % upload_filename.string() % upload_parent_path.string() %
                                   (upload_data.post_action == PrintHostPostUploadAction::StartPrint ? "true" : "false");

    std::string host = get_host_from_url(m_host);
    auto        http = Http::post(url); // std::move(url));
    // "Host" header is necessary here. We have resolved IP address and subsituted it into "url" variable.
    // And when creating Http object above, libcurl automatically includes "Host" header from address it got.
    // Thus "Host" is set to the resolved IP instead of host filled by user. We need to change it back.
    // Not changing the host would work on the most cases (where there is 1 service on 1 hostname) but would break when f.e. reverse proxy
    // is used (issue #9734). https://www.rfc-editor.org/rfc/rfc7230#section-5.4
    http.header("Host", host);
    set_auth(http);
    http.form_add("print", upload_data.post_action == PrintHostPostUploadAction::StartPrint ? "true" : "false")
        .form_add("path", upload_parent_path.string()) // XXX: slashes on windows ???
        .form_add_file("file", upload_data.source_path.string(), upload_filename.string())

        .on_complete([&](std::string body, unsigned status) {
            BOOST_LOG_TRIVIAL(debug) << boost::format("%1%: File uploaded: HTTP %2%: %3%") % name % status % body;
        })
        .on_error([&](std::string body, std::string error, unsigned status) {
            BOOST_LOG_TRIVIAL(error) << boost::format("%1%: Error uploading file to %2%: %3%, HTTP %4%, body: `%5%`") % name % url % error %
                                            status % body;

            // 尝试8080端口
            url = substitute_host(make_url_8080("server/files/upload"), GUI::into_u8(test_msg_or_host_ip));

            auto http_8080 = Http::post(std::move(url));
            http_8080.header("host", host);
            set_auth(http_8080);

            http_8080.form_add("print", upload_data.post_action == PrintHostPostUploadAction::StartPrint ? "true" : "false")
                .form_add("path", upload_parent_path.string()) // XXX: slashes on windows ???
                .form_add_file("file", upload_data.source_path.string(), upload_filename.string())
                .on_complete([&](std::string body, unsigned status) {
                    BOOST_LOG_TRIVIAL(debug) << boost::format("%1%: File uploaded: HTTP %2%: %3%") % name % status % body;
                })
                .on_error([&](std::string body, std::string error, unsigned status) {
                    BOOST_LOG_TRIVIAL(error) << boost::format("%1%: Error uploading file: %2%, HTTP %3%, body: `%4%`") % name % error %
                                                    status % body;

                    error_fn(format_error(body, error, status));
                    result = false;
                })
                .on_progress([&](Http::Progress progress, bool& cancel) {
                    prorgess_fn(std::move(progress), cancel);
                    if (cancel) {
                        // Upload was canceled
                        BOOST_LOG_TRIVIAL(info) << name << ": Upload canceled";
                        result = false;
                    }
                })
#ifdef WIN32
                .ssl_revoke_best_effort(m_ssl_revoke_best_effort)
#endif
                .perform_sync();
        })
        .on_progress([&](Http::Progress progress, bool& cancel) {
            prorgess_fn(std::move(progress), cancel);
            if (cancel) {
                // Upload was canceled
                BOOST_LOG_TRIVIAL(info) << name << ": Upload canceled";
                result = false;
            }
        })
        .ssl_revoke_best_effort(m_ssl_revoke_best_effort)
        .perform_sync();

    return result;
}
#endif // WIN32

// Upload file using hostname
bool Moonraker::upload_inner_with_host(PrintHostUpload upload_data, ProgressFn prorgess_fn, ErrorFn error_fn, InfoFn info_fn) const
{
    const char* name = get_name();

    const auto upload_filename    = upload_data.upload_path.filename();
    const auto upload_parent_path = upload_data.upload_path.parent_path();

    // If test fails, test_msg_or_host_ip contains the error message.
    // Otherwise on Windows it contains the resolved IP address of the host.
    wxString test_msg_or_host_ip;
    
    /*
    if (!test(test_msg_or_host_ip)) {
        error_fn(std::move(test_msg_or_host_ip));
        return false;
    }
     */

    std::string url;
    bool        res = true;

#ifdef WIN32
    // Workaround for Windows 10/11 mDNS resolve issue, where two mDNS resolves in succession fail.
    if (m_host.find("https://") == 0 || test_msg_or_host_ip.empty() || !GUI::get_app_config()->get_bool("allow_ip_resolve"))
#endif // _WIN32
    {
        // If https is entered we assume signed ceritificate is being used
        // IP resolving will not happen - it could resolve into address not being specified in cert
        url = make_url("server/files/upload");
    }
#ifdef WIN32
    else {
        // Workaround for Windows 10/11 mDNS resolve issue, where two mDNS resolves in succession fail.
        // Curl uses easy_getinfo to get ip address of last successful transaction.
        // If it got the address use it instead of the stored in "host" variable.
        // This new address returns in "test_msg_or_host_ip" variable.
        // Solves troubles of uploades failing with name address.
        // in original address (m_host) replace host for resolved ip
        info_fn(L"resolve", test_msg_or_host_ip);
        url = substitute_host(make_url("server/files/upload"), GUI::into_u8(test_msg_or_host_ip));
        BOOST_LOG_TRIVIAL(info) << "Upload address after ip resolve: " << url;
    }
#endif // _WIN32

    BOOST_LOG_TRIVIAL(info) << boost::format("%1%: Uploading file %2% at %3%, filename: %4%, path: %5%, print: %6%") % name %
                                   upload_data.source_path % url % upload_filename.string() % upload_parent_path.string() %
                                   (upload_data.post_action == PrintHostPostUploadAction::StartPrint ? "true" : "false");

    auto http = Http::post(std::move(url));
#ifdef WIN32
    // "Host" header is necessary here. In the workaround above (two mDNS..) we have got IP address from test connection and subsituted it
    // into "url" variable. And when creating Http object above, libcurl automatically includes "Host" header from address it got. Thus "Host"
    // is set to the resolved IP instead of host filled by user. We need to change it back. Not changing the host would work on the most cases
    // (where there is 1 service on 1 hostname) but would break when f.e. reverse proxy is used (issue #9734). Also when allow_ip_resolve =
    // 0, this is not needed, but it should not break anything if it stays. https://www.rfc-editor.org/rfc/rfc7230#section-5.4
    std::string host = get_host_from_url(m_host);
    http.header("Host", host);
#endif // _WIN32
    set_auth(http);
    http.form_add("print", upload_data.post_action == PrintHostPostUploadAction::StartPrint ? "true" : "false")
        .form_add("path", upload_parent_path.string()) // XXX: slashes on windows ???
        .form_add_file("file", upload_data.source_path.string(), upload_filename.string())
        .on_complete([&](std::string body, unsigned status) {
            BOOST_LOG_TRIVIAL(debug) << boost::format("%1%: File uploaded: HTTP %2%: %3%") % name % status % body;
        })
        .on_error([&](std::string body, std::string error, unsigned status) {
            BOOST_LOG_TRIVIAL(error) << boost::format("%1%: Error uploading file: %2%, HTTP %3%, body: `%4%`") % name % error % status %
                                            body;

            // 尝试8080端口
            url = make_url_8080("server/files/upload");
            
            auto http_8080 = Http::post(std::move(url));
            set_auth(http_8080);

            http_8080.form_add("print", upload_data.post_action == PrintHostPostUploadAction::StartPrint ? "true" : "false")
                .form_add("path", upload_parent_path.string()) // XXX: slashes on windows ???
                .form_add_file("file", upload_data.source_path.string(), upload_filename.string())
                .on_complete([&](std::string body, unsigned status) {
                    BOOST_LOG_TRIVIAL(debug) << boost::format("%1%: File uploaded: HTTP %2%: %3%") % name % status % body;
                })
                .on_error([&](std::string body, std::string error, unsigned status) {
                    BOOST_LOG_TRIVIAL(error) << boost::format("%1%: Error uploading file: %2%, HTTP %3%, body: `%4%`") % name % error %
                                                    status % body;

                    error_fn(format_error(body, error, status));
                    res = false;
                })
                .on_progress([&](Http::Progress progress, bool& cancel) {
                    prorgess_fn(std::move(progress), cancel);
                    if (cancel) {
                        // Upload was canceled
                        BOOST_LOG_TRIVIAL(info) << name << ": Upload canceled";
                        res = false;
                    }
                })
#ifdef WIN32
                .ssl_revoke_best_effort(m_ssl_revoke_best_effort)
#endif
                .perform_sync();
        })
        .on_progress([&](Http::Progress progress, bool& cancel) {
            prorgess_fn(std::move(progress), cancel);
            if (cancel) {
                // Upload was canceled
                BOOST_LOG_TRIVIAL(info) << name << ": Upload canceled";
                res = false;
            }
        })
#ifdef WIN32
        .ssl_revoke_best_effort(m_ssl_revoke_best_effort)
#endif
        .perform_sync();

    return res;
}

// Validate version text to confirm printer type
bool Moonraker::validate_version_text(const boost::optional<std::string>& version_text) const
{
    return version_text ? boost::starts_with(*version_text, "Moonraker") : true;
}

// Set authentication headers for HTTP requests
void Moonraker::set_auth(Http& http) const
{
    http.header("X-Api-Key", m_apikey);

    if (!m_cafile.empty()) {
        http.ca_file(m_cafile);
    }
}

std::string Moonraker::make_url_8080(const std::string& path) const
{
    size_t pos = m_host.find(":");
    std::string target_host = m_host;
    if (pos != std::string::npos && pos != std::string("http:").length() - 1 && std::string("https:").length() - 1) {
        target_host = target_host.substr(0, pos);
    }
    if (target_host.find("http://") == 0 || target_host.find("https://") == 0) {
        if (target_host.back() == '/') {
            target_host = target_host.substr(0, target_host.length() - 1);
            return (boost::format("%1%:8080/%2%") % target_host % path).str();
        } else {
            return (boost::format("%1%:8080/%2%") % target_host % path).str();
        }
    } else {
        return (boost::format("http://%1%:8080/%2%") % target_host % path).str();
    }
}

// Construct full URL for API endpoint
std::string Moonraker::make_url(const std::string& path) const
{
    if (m_host.find("http://") == 0 || m_host.find("https://") == 0) {
        if (m_host.back() == '/') {
            return (boost::format("%1%%2%") % m_host % path).str();
        } else {
            return (boost::format("%1%/%2%") % m_host % path).str();
        }
    } else {
        if (m_host.find(":1883") != std::string::npos) {
            std::string http_host = m_host.substr(0, m_host.find(":1883"));
            return (boost::format("http://%1%/%2%") % http_host % path).str();
        }
        return (boost::format("http://%1%/%2%") % m_host % path).str();
    }
}

// Basic connect implementation
bool Moonraker::connect(wxString& msg, const nlohmann::json& params) {
    return test(msg);
}








// Moonraker_mqtt

std::shared_ptr<MqttClient> Moonraker_Mqtt::m_mqtt_client = nullptr;
TimeoutMap<std::string, Moonraker_Mqtt::RequestCallback> Moonraker_Mqtt::m_request_cb_map;
std::function<void(const nlohmann::json&)> Moonraker_Mqtt::m_status_cb = nullptr;
std::string Moonraker_Mqtt::m_response_topic = "/response";
std::string Moonraker_Mqtt::m_status_topic = "/status";
std::string Moonraker_Mqtt::m_notification_topic = "/notification";
std::string Moonraker_Mqtt::m_request_topic = "/request";
std::string Moonraker_Mqtt::m_sn = "";
std::mutex Moonraker_Mqtt::m_sn_mtx;


Moonraker_Mqtt::Moonraker_Mqtt(DynamicPrintConfig* config, bool change_engine) : Moonraker(config) {
    std::string host_info = config->option<ConfigOptionString>("print_host")->value;

    if (change_engine) {
        // 生成随机UUID
        boost::uuids::random_generator gen;
        boost::uuids::uuid             uuid       = gen();
        std::string                    random_str = boost::uuids::to_string(uuid);

        // 截取UUID的前8位
        random_str = random_str.substr(0, 8);

        m_mqtt_client.reset(new MqttClient("mqtt://" + host_info, "orca_" + random_str, true));
    }
    
}

// Connect to MQTT broker
bool Moonraker_Mqtt::connect(wxString& msg, const nlohmann::json& params) {
    if (m_mqtt_client->CheckConnected()) {
        disconnect(msg, params);
    }
    m_sn_mtx.lock();
    m_sn = "";
    m_sn_mtx.unlock();


    bool is_connect = m_mqtt_client->Connect();
    if (!is_connect)
        return false;

    std::string mainLayer = "";
    if (params.count("sn")) {
        mainLayer = params["sn"].get<std::string>();
        if (mainLayer != "") {
            m_sn_mtx.lock();
            m_sn = mainLayer;
            m_sn_mtx.unlock();
        } else {
            mainLayer = "+";
        }
    } else {
        mainLayer = "+";
    }

    bool notification_subscribed = m_mqtt_client->Subscribe(mainLayer + m_notification_topic, 2);
    bool response_subscribed = m_mqtt_client->Subscribe(mainLayer + m_response_topic, 2);
    m_mqtt_client->SetMessageCallback([this](const std::string& topic, const std::string& payload) {
        this->on_mqtt_message_arrived(topic, payload);
    });

    /*if (!wait_for_sn()) {
        return false;
    }*/

    return is_connect && notification_subscribed && response_subscribed;
}

// Disconnect from MQTT broker
bool Moonraker_Mqtt::disconnect(wxString& msg, const nlohmann::json& params) {
    return m_mqtt_client->Disconnect();
    m_sn_mtx.lock();
    m_sn = "";
    m_sn_mtx.unlock();
}

// Subscribe to printer status updates
void Moonraker_Mqtt::async_subscribe_machine_info(std::function<void(const nlohmann::json&)> callback)
{
    std::string main_layer = "+";
    
    m_sn_mtx.lock();
    main_layer = m_sn;
    m_sn_mtx.unlock();

    bool res = m_mqtt_client->Subscribe(main_layer + m_status_topic, 2);

    if (!res) {
        if (m_status_cb) {
            callback(json::value_t::null);
        }
        return;
    }

    m_status_cb = callback;
    callback(json::object());
}

// start print job
void Moonraker_Mqtt::async_start_print_job(const std::string& filename, std::function<void(const nlohmann::json&)> cb)
{
    std::string method = "printer.print.start";
    json        params = json::object();
    params["filename"] = filename;

    if (!send_to_request(method, params, true, cb,
                         [cb]() {
                             json res;
                             res["error"] = "timeout";
                             cb(res);
                         }) &&
        cb) {
        cb(json::value_t::null);
    }
}

void Moonraker_Mqtt::async_pause_print_job(std::function<void(const nlohmann::json&)> cb) {
    std::string method =  "printer.print.pause";
    json        params  = json::object();

    if (!send_to_request(method, params, true, cb,
                         [cb]() {
                             json res;
                             res["error"] = "timeout";
                             cb(res);
                         }) &&
        cb) {
        cb(json::value_t::null);
    }
}

void Moonraker_Mqtt::async_resume_print_job(std::function<void(const nlohmann::json&)> cb) {
    std::string method = "printer.print.resume";
    json        params = json::object();

    if (!send_to_request(method, params, true, cb,
                         [cb]() {
                             json res;
                             res["error"] = "timeout";
                             cb(res);
                         }) &&
        cb) {
        cb(json::value_t::null);
    }
}

void Moonraker_Mqtt::test_async_wcp_mqtt_moonraker(const nlohmann::json& mqtt_request_params, std::function<void(const nlohmann::json&)> cb) {
    std::string id = "";

    if (!mqtt_request_params.count("id")) {
        cb(json::value_t::null);
        return;
    }

    if (mqtt_request_params["id"].is_string()) {
        id = mqtt_request_params["id"].get<std::string>();
    } else if (mqtt_request_params["id"].is_number()) {
        id = std::to_string(mqtt_request_params["id"].get<int>());
    }

    if (id == "") {
        cb(json::value_t::null);
        return;
    }

    if (!add_response_target(id, cb, [cb]() {
            json res;
            res["error"] = "timeout";
            cb(res);
        })){
        cb(json::value_t::null);
    }

    if (m_mqtt_client) {
        std::string main_layer = "+";

        if (wait_for_sn()) {
            m_sn_mtx.lock();
            main_layer = m_sn;
            m_sn_mtx.unlock();

            if (main_layer == "+" || main_layer == "") {
                delete_response_target(id);
                cb(json::value_t::null);
                return;
            }

            bool res = m_mqtt_client->Publish(main_layer + m_request_topic, mqtt_request_params.dump(), 2);
            if (!res) {
                delete_response_target(id);
            }
            return;
        }
    }
}

void Moonraker_Mqtt::async_cancel_print_job(std::function<void(const nlohmann::json&)> cb)
{
    std::string method = "printer.print.cancel";
    json        params = json::object();

    if (!send_to_request(method, params, true, cb,
                         [cb]() {
                             json res;
                             res["error"] = "timeout";
                             cb(res);
                         }) &&
        cb) {
        cb(json::value_t::null);
    }
}

// Get printer info
void Moonraker_Mqtt::async_get_printer_info(std::function<void(const nlohmann::json& response)> callback) {
    std::string method = "printer.info";
    json        params = json::object();

    if (!send_to_request(method, params, true, callback,
                         [callback]() {
                             json res;
                             res["error"] = "timeout";
                             callback(res);
                         }) &&
        callback) {
        callback(json::value_t::null);
    }
}

// Send G-code commands to printer
void Moonraker_Mqtt::async_send_gcodes(const std::vector<std::string>& scripts, std::function<void(const nlohmann::json&)> callback)
{
    std::string method = "printer.gcode.script";

    std::string str_scripts = "";
    for (size_t i = 0; i < scripts.size(); ++i) {
        if (i != 0) {
            str_scripts += "\n";
        }
        str_scripts += scripts[i];
    }

    json params;
    params["script"] = str_scripts;

    if (!send_to_request(method, params, true, callback, [callback](){
        json res;
        res["error"] = "timeout";
        callback(res);
    }) && callback) {
        callback(json::value_t::null);
    }
}

// Unsubscribe from printer status updates
void Moonraker_Mqtt::async_unsubscribe_machine_info(std::function<void(const nlohmann::json&)> callback)
{
    std::string main_layer = "+";
    
    m_sn_mtx.lock();
    main_layer = m_sn;
    m_sn_mtx.unlock();

    bool res = m_mqtt_client->Unsubscribe(main_layer + m_status_topic);

    if (!res) {
        if (callback) {
            callback(json::value_t::null);
        }
        return;
    }

    m_status_cb = nullptr;
    callback(json::object());
}

// Set filters for printer status subscription
void Moonraker_Mqtt::async_set_machine_subscribe_filter(
    const std::vector<std::pair<std::string, std::vector<std::string>>>& targets,
    std::function<void(const nlohmann::json& response)> callback)
{
    std::string method = "printer.objects.subscribe";

    json params;
    params["objects"] = json::object();

    for (size_t i = 0; i < targets.size(); ++i) {
        if (targets[i].second.size() == 0) {
            params["objects"][targets[i].first] = json::value_t::null;
        } else {
            params["objects"][targets[i].first] = json::array();

            for (const auto& key : targets[i].second) {
                params["objects"][targets[i].first].push_back(key);
            }
        }
    }

    if (!send_to_request(method, params, true, callback, [callback](){
        json res;
        res["error"] = "timeout";
        callback(res);
    }) && callback) {
        callback(json::value_t::null);
    }
}

void Moonraker_Mqtt::async_machine_files_roots(std::function<void(const nlohmann::json& response)> callback) {
    std::string method = "server.files.roots";

    json params = json::object();

    if (!send_to_request(method, params, true, callback,
                         [callback]() {
                             json res;
                             res["error"] = "timeout";
                             callback(res);
                         }) &&
        callback) {
        callback(json::value_t::null);
    }
}

void Moonraker_Mqtt::async_machine_files_metadata(const std::string& filename, std::function<void(const nlohmann::json& response)> callback)
{
    std::string method = "server.files.metadata";

    json params;
    params["filename"] = filename;

    if (!send_to_request(method, params, true, callback,
                         [callback]() {
                             json res;
                             res["error"] = "timeout";
                             callback(res);
                         }) &&
        callback) {
        callback(json::value_t::null);
    }
}

void Moonraker_Mqtt::async_machine_files_thumbnails(const std::string& filename, std::function<void(const nlohmann::json& response)> callback) {
    std::string method = "server.files.thumbnails";

    json params;
    params["filename"] = filename;

    if (!send_to_request(method, params, true, callback,
                         [callback]() {
                             json res;
                             res["error"] = "timeout";
                             callback(res);
                         }) &&
        callback) {
        callback(json::value_t::null);
    }
}

void Moonraker_Mqtt::async_machine_files_directory(const std::string& path, bool extend, std::function<void(const nlohmann::json& response)> callback) {
    std::string method = "server.files.get_directory";

    json params;
    params["path"] = path;
    params["extended"] = extend;

    if (!send_to_request(method, params, true, callback,
                         [callback]() {
                             json res;
                             res["error"] = "timeout";
                             callback(res);
                         }) &&
        callback) {
        callback(json::value_t::null);
    }
}

void Moonraker_Mqtt::async_camera_start(const std::string& domain, std::function<void(const nlohmann::json& response)> callback) {
    std::string method = "server.camera.start_monitor";

    json params;
    params["domain"]     = domain;

    if (!send_to_request(method, params, true, callback,
                         [callback]() {
                             json res;
                             res["error"] = "timeout";
                             callback(res);
                         }) &&
        callback) {
        callback(json::value_t::null);
    }
}

void Moonraker_Mqtt::async_canmera_stop(const std::string& domain, std::function<void(const nlohmann::json& response)> callback) {
    std::string method = "server.camera.stop_monitor";

    json params;
    params["domain"] = domain;

    if (!send_to_request(method, params, true, callback,
                         [callback]() {
                             json res;
                             res["error"] = "timeout";
                             callback(res);
                         }) &&
        callback) {
        callback(json::value_t::null);
    }
}

// Query printer information
void Moonraker_Mqtt::async_get_machine_info(
    const std::vector<std::pair<std::string, std::vector<std::string>>>& targets,
    std::function<void(const nlohmann::json& response)> callback)
{
    std::string method = "printer.objects.query";

    json params;
    params["objects"] = json::object();

    for (size_t i = 0; i < targets.size(); ++i) {
        if (targets[i].second.size() == 0) {
            params["objects"][targets[i].first] = json::value_t::null;
        } else {
            params["objects"][targets[i].first] = json::array();

            for (const auto& key : targets[i].second) {
                params["objects"][targets[i].first].push_back(key);
            }
        }
    }

    if (!send_to_request(method, params, true, callback, [callback](){
        json res;
        res["error"] = "timeout";
        callback(res);
    }) && callback) {
        callback(json::value_t::null);
    }
}

// Get system info of the machine
void Moonraker_Mqtt::async_get_system_info(std::function<void(const nlohmann::json& response)> callback)
{
    std::string method = "machine.system_info";
    json params = json::object();

    if (!send_to_request(method, params, true, callback, [callback](){
        json res;
        res["error"] = "timeout";
        callback(res);
    }) && callback) {
        callback(json::value_t::null);
    }
}

// Get list of available printer objects
void Moonraker_Mqtt::async_get_machine_objects(std::function<void(const nlohmann::json& response)> callback)
{
    std::string method = "printer.objects.list";
    json params = json::object();

    if (!send_to_request(method, params, true, callback, [callback](){
        json res;
        res["error"] = "timeout";
        callback(res);
    }) && callback) {
        callback(json::value_t::null);
    }
}

// Send request to printer via MQTT
bool Moonraker_Mqtt::send_to_request(
    const std::string& method,
    const json& params,
    bool need_response,
    std::function<void(const nlohmann::json& response)> callback,
    std::function<void()> timeout_callback)
{
    json body;
    body["jsonrpc"] = "2.0";
    body["method"] = method;
    body["params"] = params;

    boost::uuids::uuid uuid = m_generator();
    std::string str_uuid = boost::uuids::to_string(uuid);

    if (need_response) {
        if (!add_response_target(str_uuid, callback, timeout_callback)) {
            return false;
        }
        body["id"] = str_uuid;
    }

    if (m_mqtt_client) {
        std::string main_layer = "+";
    
        m_sn_mtx.lock();
        main_layer = m_sn;
        m_sn_mtx.unlock();

        if (main_layer == "+" || main_layer == "") {
            delete_response_target(str_uuid);
            return false;
        }

        bool res = m_mqtt_client->Publish(main_layer + m_request_topic, body.dump(), 2);
        if (!res) {
            delete_response_target(str_uuid);
        }
        return res;

        
    }
    return false;
}

// Register callback for response to a request
bool Moonraker_Mqtt::add_response_target(
    const std::string& id,
    std::function<void(const nlohmann::json&)> callback,
    std::function<void()> timeout_callback,
    std::chrono::milliseconds timeout)
{
    return m_request_cb_map.add(
        id,
        RequestCallback(std::move(callback), std::move(timeout_callback)),
        timeout
    );
}

// Remove registered callback
void Moonraker_Mqtt::delete_response_target(const std::string& id) {
    m_request_cb_map.remove(id);
}

bool Moonraker_Mqtt::check_sn_arrived() {
    return wait_for_sn();
}

// Get and remove callback for a request
std::function<void(const json&)> Moonraker_Mqtt::get_request_callback(const std::string& id)
{
    auto request_cb = m_request_cb_map.get_and_remove(id);
    return request_cb ? request_cb->success_cb : nullptr;
}

// Handle incoming MQTT messages
void Moonraker_Mqtt::on_mqtt_message_arrived(const std::string& topic, const std::string& payload)
{
    try {
        if (topic.find(m_response_topic) != std::string::npos) {
            on_response_arrived(payload);
        } else if (topic.find(m_status_topic) != std::string::npos) {
            on_status_arrived(payload);
        } else if (topic.find(m_notification_topic) != std::string::npos) {
            
            size_t pos = topic.find("/notification");

            if (pos != std::string::npos) {
                std::string sn = topic.substr(0, pos);

                m_sn_mtx.lock();
                m_sn = sn;
                m_sn_mtx.unlock();
            }
            

            on_notification_arrived(payload);
        }
        else {
            return;
        }
    } catch (std::exception& e) {}
}

// Handle response messages
void Moonraker_Mqtt::on_response_arrived(const std::string& payload)
{
    try {
        json body = json::parse(payload);

        if (!body.count("id")) {
            return;
        }

        std::string id = "";
        if (body["id"].is_number()) {
            id = std::to_string(body["id"].get<int>());
        } else if (body["id"].is_string()) {
            id = body["id"].get<std::string>();
        }

        auto cb = get_request_callback(id);
        delete_response_target(id);

        if (!cb) {
            return;
        }

        json res;

        if (id.find("mqtt") != std::string::npos) {
            cb(body);
        } else {
            if (!body.count("result")) {
                if (body.count("error")) {
                    json error   = body["error"];
                    res["error"] = error;
                }
            } else {
                res["data"] = body["result"];
                // test: 如果有method，则进行传递
            }
            res["method"] = "";
            if (body.count("method")) {
                res["method"] = body["method"];
            }
            cb(res);
        }
        

    } catch (std::exception& e) {}
}

// Handle status update messages
void Moonraker_Mqtt::on_status_arrived(const std::string& payload)
{
    try {
        json body = json::parse(payload);

        json data;
        if (body.count("params")) {
            data["data"] = body["params"];
        } else if (body.count("result") && body["result"].count("status")) {
            data["data"] = body["result"]["status"];
        } else {
            return;
        }

        // test: 如果有method，则进行传递
        data["method"] = "";
        if (body.count("method")){
            data["method"] = body["method"];
        }

        if (!m_status_cb) {
            return;
        }

        m_status_cb(data);

    } catch (std::exception& e) {}
}

// Handle notification messages
void Moonraker_Mqtt::on_notification_arrived(const std::string& payload)
{
    try {
    } catch (std::exception& e) {}
}

// 添加一个辅助函数来等待SN
bool Moonraker_Mqtt::wait_for_sn(int timeout_seconds)
{
    using namespace std::chrono;
    auto start = steady_clock::now();

    while (true) {
        {
            std::lock_guard<std::mutex> lock(m_sn_mtx);
            if (!m_sn.empty()) {
                return true;
            }
        }

        auto now = steady_clock::now();
        if (duration_cast<seconds>(now - start).count() >= timeout_seconds) {
            return false;
        }

        std::this_thread::sleep_for(milliseconds(100));
    }
}

void Moonraker_Mqtt::set_connection_lost(std::function<void()> callback) {
    m_mqtt_client->SetConnectionFailureCallback(callback);
}

} // namespace Slic3r
