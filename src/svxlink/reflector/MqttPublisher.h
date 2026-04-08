#ifndef MQTT_PUBLISHER_INCLUDED
#define MQTT_PUBLISHER_INCLUDED

#include <string>
#include <cstdint>
#include <json/json.h>

struct mosquitto;

namespace Async { class Config; }

class MqttPublisher
{
  public:
    MqttPublisher(Async::Config& cfg);
    ~MqttPublisher(void);

    bool initialize(void);
    void shutdown(void);

    // Talker events (local clients and trunk peers)
    void onTalkerStart(uint32_t tg, const std::string& callsign, bool is_trunk);
    void onTalkerStop(uint32_t tg, const std::string& callsign, bool is_trunk);

    // Client events
    void onClientConnected(const std::string& callsign, uint32_t tg,
                           const std::string& ip);
    void onClientDisconnected(const std::string& callsign);

    // Trunk link events
    void onTrunkUp(const std::string& section, const std::string& direction,
                   const std::string& host, uint16_t port);
    void onTrunkDown(const std::string& section, const std::string& direction);

    // Periodic full status
    void publishFullStatus(const Json::Value& status);

  private:
    Async::Config&      m_cfg;
    struct mosquitto*   m_mosq = nullptr;
    std::string         m_host;
    uint16_t            m_port = 1883;
    std::string         m_username;
    std::string         m_password;
    std::string         m_topic_prefix;
    bool                m_tls_enabled = false;
    std::string         m_tls_ca_cert;
    std::string         m_tls_client_cert;
    std::string         m_tls_client_key;

    void publish(const std::string& topic_suffix, const std::string& payload,
                 bool retain = false);

    MqttPublisher(const MqttPublisher&);
    MqttPublisher& operator=(const MqttPublisher&);

};  /* class MqttPublisher */

#endif /* MQTT_PUBLISHER_INCLUDED */
