#ifndef MQTT_H
#define MQTT_H

#include <functional>
#include <string>
#include <map>
#include <mqtt/async_client.h>
#include <boost/log/trivial.hpp>

// Number of retries for connection and subscription attempts
#define CONNECT_RETRY_TIME 3
#define SUBSCRIBE_RETRY_TIME 3

// Action listener class to handle MQTT operation callbacks
class action_listener : public virtual mqtt::iaction_listener
{
private:
    std::string name_;  // Name identifier for the listener

public:
    // Constructor with a name for identification
    action_listener(const std::string& name) : name_(name) {}

    // Called when an MQTT operation fails
    void on_failure(const mqtt::token& tok) override
    {
        BOOST_LOG_TRIVIAL(error) << name_ << " operation failed";
        if (tok.get_message_id() != 0) {
            BOOST_LOG_TRIVIAL(error) << "Token: [" << tok.get_message_id() << "]";
        }
    }

    // Called when an MQTT operation succeeds
    void on_success(const mqtt::token& tok) override
    {
        BOOST_LOG_TRIVIAL(debug) << name_ << " operation successful";
        if (tok.get_message_id() != 0) {
            BOOST_LOG_TRIVIAL(debug) << "Token: [" << tok.get_message_id() << "]";
        }
        auto top = tok.get_topics();
        if (top && !top->empty()) {
            BOOST_LOG_TRIVIAL(debug) << "Token topic: '" << (*top)[0] << "'";
        }
    }
};

// Main MQTT client class implementing both callback and action listener interfaces
class MqttClient : public virtual mqtt::callback, public virtual mqtt::iaction_listener
{
public:
    // Constructor initializes the MQTT client with server details
    MqttClient(const std::string& server_address, const std::string& client_id, bool clean_session);

    // Connect to the MQTT broker
    bool Connect();

    // Disconnect from the MQTT broker
    bool Disconnect();

    // Subscribe to a specific topic with given QoS
    bool Subscribe(const std::string& topic, int qos);

    // Unsubscribe from a specific topic
    bool Unsubscribe(const std::string& topic);

    // Publish a message to a specific topic with given QoS
    bool Publish(const std::string& topic, const std::string& payload, int qos);

    // Set callback for handling incoming messages
    void SetMessageCallback(std::function<void(const std::string& topic, const std::string& payload)> callback);

    // Callback interface implementations
    void connection_lost(const std::string& cause) override;
    void message_arrived(mqtt::const_message_ptr msg) override;
    void delivery_complete(mqtt::delivery_token_ptr token) override;

    // Action listener interface implementations
    void on_failure(const mqtt::token& tok) override;
    void on_success(const mqtt::token& tok) override;

    // Check if client is currently connected
    bool CheckConnected();

private:
    std::string server_address_;     // MQTT broker address
    std::string client_id_;          // Unique client identifier
    mqtt::async_client client_;      // Async MQTT client instance
    std::function<void(const std::string& topic, const std::string& payload)> message_callback_;  // Message handler
    mqtt::connect_options connOpts_; // Connection options
    bool connected_;                 // Connection status flag
    std::map<std::string, int> topics_to_resubscribe_;  // Topics to resubscribe after reconnection
    action_listener subListener_;    // Subscription listener
    int connect_retry_time_;         // Connection retry counter
    int subscribe_retry_time_;       // Subscription retry counter
};

#endif // MQTT_H
