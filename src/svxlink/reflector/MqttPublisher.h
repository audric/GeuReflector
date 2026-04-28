#ifndef MQTT_PUBLISHER_INCLUDED
#define MQTT_PUBLISHER_INCLUDED

#include <string>
#include <cstdint>
#include <ctime>
#include <vector>
#include <json/json.h>
#include "ReflectorMsg.h"

namespace Async { class Config; }

#ifdef WITH_MQTT

struct mosquitto;

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

    // RX status update (per-client, all receivers)
    void onRxUpdate(const std::string& callsign, const Json::Value& rx_json);

    // Periodic full status
    void publishFullStatus(const Json::Value& status);

    // Node-list snapshots — published retained
    void publishLocalNodes(
        const std::vector<MsgPeerNodeList::NodeEntry>& nodes);
    void publishPeerNodes(const std::string& peer_id,
        const std::vector<MsgPeerNodeList::NodeEntry>& nodes);

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

    int                 m_last_pub_err_code   = 0;
    time_t              m_last_pub_err_logged = 0;
    uint64_t            m_pub_err_suppressed  = 0;

    void publish(const std::string& topic_suffix, const std::string& payload,
                 bool retain = false);

    MqttPublisher(const MqttPublisher&);
    MqttPublisher& operator=(const MqttPublisher&);

};  /* class MqttPublisher */

#else /* !WITH_MQTT */

// Build-time no-op stub: lets the rest of the codebase compile and link
// when libmosquitto is unavailable. initialize() returns false so
// Reflector::initialize logs the existing "MQTT publisher failed to
// initialize, continuing without MQTT" warning and proceeds with
// m_mqtt = nullptr; every other call site is already null-guarded.
class MqttPublisher
{
  public:
    MqttPublisher(Async::Config&) {}
    bool initialize(void) { return false; }
    void shutdown(void) {}
    void onTalkerStart(uint32_t, const std::string&, bool) {}
    void onTalkerStop(uint32_t, const std::string&, bool) {}
    void onClientConnected(const std::string&, uint32_t,
                           const std::string&) {}
    void onClientDisconnected(const std::string&) {}
    void onTrunkUp(const std::string&, const std::string&,
                   const std::string&, uint16_t) {}
    void onTrunkDown(const std::string&, const std::string&) {}
    void onRxUpdate(const std::string&, const Json::Value&) {}
    void publishFullStatus(const Json::Value&) {}
    void publishLocalNodes(
        const std::vector<MsgPeerNodeList::NodeEntry>&) {}
    void publishPeerNodes(const std::string&,
        const std::vector<MsgPeerNodeList::NodeEntry>&) {}
};

#endif /* WITH_MQTT */

#endif /* MQTT_PUBLISHER_INCLUDED */
