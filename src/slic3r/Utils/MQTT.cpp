#include "MQTT.hpp"
#include <iostream>
#include <thread>

// 构造函数，初始化MQTT客户端
MqttClient::MqttClient(const std::string& server_address, const std::string& client_id, bool clean_session)
    : server_address_(server_address), client_id_(client_id), client_(server_address_, client_id_), connOpts_(), subListener_("Subscription"), connected_(false)
{
    // 设置连接选项
    connOpts_.set_clean_session(clean_session);
    connOpts_.set_keep_alive_interval(20);

    // 设置回调
    client_.set_callback(*this);
}

// 连接到MQTT服务器
bool MqttClient::Connect()
{
    try {
        std::cout << "Connecting to the MQTT server..." << std::flush;
        mqtt::token_ptr conntok = client_.connect(connOpts_, nullptr, *this);
        conntok->wait();
        connected_ = true;
        std::cout << "Connected" << std::endl;
        return true;
    } catch (const mqtt::exception& exc) {
        connected_ = false;
        std::cerr << "\nERROR: Unable to connect to MQTT server: '" << server_address_ << "'\n" << exc.what() << std::endl;
        return false;
    }
}

// 断开与MQTT服务器的连接
bool MqttClient::Disconnect()
{
    if (!connected_) {
        std::cerr << "Already disconnected" << std::endl;
        return false;
    }

    try {
        std::cout << "\nDisconnecting from the MQTT server..." << std::flush;
        mqtt::token_ptr disctok = client_.disconnect();
        disctok->wait();
        connected_ = false;
        std::cout << "OK" << std::endl;
        return true;
    } catch (const mqtt::exception& exc) {
        std::cerr << "Error on disconnect: " << exc.what() << std::endl;
        return false;
    }
}

// 检查客户端是否已连接
bool MqttClient::CheckConnected()
{
    if (!connected_) {
        std::cerr << "Error: Client is not connected to the MQTT server" << std::endl;
        return false;
    }
    return true;
}

// 订阅某个话题
bool MqttClient::Subscribe(const std::string& topic, int qos)
{
    if (!CheckConnected()) {
        return false;
    }

    try {
        std::cout << "Subscribing to topic '" << topic << "' with QoS " << qos << std::endl;
        mqtt::token_ptr subtok = client_.subscribe(topic, qos, nullptr, subListener_);
        subtok->wait();
        return true;
    } catch (const mqtt::exception& exc) {
        std::cerr << "Error on subscribe: " << exc.what() << std::endl;
        return false;
    }
}

// 解除对某个话题的订阅
bool MqttClient::Unsubscribe(const std::string& topic)
{
    if (!CheckConnected()) {
        return false;
    }

    try {
        std::cout << "Unsubscribing from topic '" << topic << "'" << std::endl;
        mqtt::token_ptr unsubtok = client_.unsubscribe(topic);
        unsubtok->wait();
        return true;
    } catch (const mqtt::exception& exc) {
        std::cerr << "Error on unsubscribe: " << exc.what() << std::endl;
        return false;
    }
}

// 发布消息到某个话题
bool MqttClient::Publish(const std::string& topic, const std::string& payload, int qos)
{
    if (!CheckConnected()) {
        return false;
    }

    mqtt::message_ptr pubmsg = mqtt::make_message(topic, payload);
    pubmsg->set_qos(qos);

    try {
        std::cout << "Publishing message '" << payload << "' to topic '" << topic << "' with QoS " << qos << std::endl;
        mqtt::token_ptr pubtok = client_.publish(pubmsg);
        pubtok->wait();
        return true;
    } catch (const mqtt::exception& exc) {
        std::cerr << "Error on publish: " << exc.what() << std::endl;
        return false;
    }
}

// 设置消息到达的回调
void MqttClient::SetMessageCallback(std::function<void(const std::string& topic, const std::string& payload)> callback)
{
    message_callback_ = callback;
}

// 实现mqtt::callback接口的方法
void MqttClient::connection_lost(const std::string& cause)
{
    connected_ = false;
    std::cout << "\nConnection lost" << std::endl;
    if (!cause.empty())
        std::cout << "\tcause: " << cause << std::endl;

    Connect();
}

void MqttClient::message_arrived(mqtt::const_message_ptr msg)
{
    if (message_callback_) {
        message_callback_(msg->get_topic(), msg->to_string());
    }
}

void MqttClient::delivery_complete(mqtt::delivery_token_ptr token)
{
    std::cout << "Delivery complete for token: " << (token ? token->get_message_id() : -1) << std::endl;
}

// 实现mqtt::iaction_listener接口的方法
void MqttClient::on_failure(const mqtt::token& tok)
{
    std::cout << "Failure in token: " << tok.get_message_id() << std::endl;
    if (tok.get_reason_code() != 0)
        std::cout << "\treason_code: " << tok.get_reason_code() << std::endl;

    // connected_ = false;
}

void MqttClient::on_success(const mqtt::token& tok)
{
    std::cout << "Success in token: " << tok.get_message_id() << std::endl;
    auto top = tok.get_topics();
    if (top && !top->empty())
        std::cout << "\ttoken topic: '" << (*top)[0] << "', ..." << std::endl;
    std::cout << std::endl;
}
