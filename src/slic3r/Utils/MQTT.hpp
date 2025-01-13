#ifndef MQTT_H
#define MQTT_H

#include <functional>
#include <string>
#include <map>
#include <mqtt/async_client.h>

#define CONNECT_RETRY_TIME 3
#define SUBSCRIBE_RETRY_TIME 3

// 定义 action_listener 类
class action_listener : public virtual mqtt::iaction_listener
{
private:
    std::string name_;

public:
    action_listener(const std::string& name) : name_(name) {}

    void on_failure(const mqtt::token& tok) override
    {
        std::cout << name_ << " failure";
        if (tok.get_message_id() != 0)
            std::cout << " for token: [" << tok.get_message_id() << "]" << std::endl;
        std::cout << std::endl;
    }

    void on_success(const mqtt::token& tok) override
    {
        std::cout << name_ << " success";
        if (tok.get_message_id() != 0)
            std::cout << " for token: [" << tok.get_message_id() << "]" << std::endl;
        auto top = tok.get_topics();
        if (top && !top->empty())
            std::cout << "\ttoken topic: '" << (*top)[0] << "', ..." << std::endl;
        std::cout << std::endl;
    }
};

class MqttClient : public virtual mqtt::callback, public virtual mqtt::iaction_listener
{
public:
    // 构造函数，初始化MQTT客户端
    MqttClient(const std::string& server_address, const std::string& client_id, bool clean_session);

    // 连接到MQTT服务器
    bool Connect();

    // 断开与MQTT服务器的连接
    bool Disconnect();

    // 订阅某个话题
    bool Subscribe(const std::string& topic, int qos);

    // 解除对某个话题的订阅
    bool Unsubscribe(const std::string& topic);

    // 发布消息到某个话题
    bool Publish(const std::string& topic, const std::string& payload, int qos);

    // 设置消息到达的回调
    void SetMessageCallback(std::function<void(const std::string& topic, const std::string& payload)> callback);

    // 实现mqtt::callback接口的方法
    void connection_lost(const std::string& cause) override;
    void message_arrived(mqtt::const_message_ptr msg) override;
    void delivery_complete(mqtt::delivery_token_ptr token) override;

    // 实现mqtt::iaction_listener接口的方法
    void on_failure(const mqtt::token& tok) override;
    void on_success(const mqtt::token& tok) override;

    // 检查客户端是否已连接
    bool CheckConnected();

private:
    std::string                                                               server_address_;
    std::string                                                               client_id_;
    mqtt::async_client                                                        client_;
    std::function<void(const std::string& topic, const std::string& payload)> message_callback_;
    mqtt::connect_options                                                     connOpts_;
    bool                                                                      connected_ = false;
    std::map<std::string, int>                                                topics_to_resubscribe_;
    action_listener                                                           subListener_;
    int                                                                       connect_retry_time_;
    int                                                                       subscribe_retry_time_;

    
};

#endif // MQTT_H
