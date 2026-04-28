#include "MqttPublisher.h"

#include <iostream>
#include <sstream>
#include <mosquitto.h>
#include <AsyncConfig.h>
#include <Log.h>

using namespace std;


static void on_connect_cb(struct mosquitto*, void*, int rc)
{
  if (rc == 0)
  {
    geulog::info("mqtt", "MQTT: Connected to broker");
  }
  else
  {
    geulog::warn("mqtt", "MQTT connection failed: ",
                 mosquitto_connack_string(rc));
  }
}


static void on_disconnect_cb(struct mosquitto*, void*, int rc)
{
  if (rc != 0)
  {
    geulog::warn("mqtt", "MQTT disconnected unexpectedly (rc=", rc,
                 "), will reconnect");
  }
}


MqttPublisher::MqttPublisher(Async::Config& cfg)
  : m_cfg(cfg)
{
}


MqttPublisher::~MqttPublisher(void)
{
  shutdown();
}


bool MqttPublisher::initialize(void)
{
  // Read required config values
  if (!m_cfg.getValue("MQTT", "HOST", m_host) || m_host.empty())
  {
    geulog::error("mqtt", "MQTT/HOST not configured");
    return false;
  }

  string port_str;
  if (m_cfg.getValue("MQTT", "PORT", port_str))
  {
    m_port = static_cast<uint16_t>(stoi(port_str));
  }
  else
  {
    geulog::error("mqtt", "MQTT/PORT not configured");
    return false;
  }

  if (!m_cfg.getValue("MQTT", "USERNAME", m_username) || m_username.empty())
  {
    geulog::error("mqtt", "MQTT/USERNAME not configured");
    return false;
  }

  if (!m_cfg.getValue("MQTT", "PASSWORD", m_password) || m_password.empty())
  {
    geulog::error("mqtt", "MQTT/PASSWORD not configured");
    return false;
  }

  if (!m_cfg.getValue("MQTT", "TOPIC_PREFIX", m_topic_prefix) ||
      m_topic_prefix.empty())
  {
    geulog::error("mqtt", "MQTT/TOPIC_PREFIX not configured");
    return false;
  }

  // Strip trailing slash from topic prefix
  while (!m_topic_prefix.empty() && m_topic_prefix.back() == '/')
  {
    m_topic_prefix.pop_back();
  }

  // Optional MQTT_NAME — appended as a path component so each reflector
  // publishes under a unique sub-tree (TOPIC_PREFIX/MQTT_NAME/...).
  string mqtt_name;
  if (m_cfg.getValue("MQTT", "MQTT_NAME", mqtt_name) && !mqtt_name.empty())
  {
    while (!mqtt_name.empty() && mqtt_name.back()  == '/') mqtt_name.pop_back();
    while (!mqtt_name.empty() && mqtt_name.front() == '/') mqtt_name.erase(0, 1);
    if (!mqtt_name.empty()) m_topic_prefix += "/" + mqtt_name;
  }

  // Read optional TLS config
  string tls_str;
  if (m_cfg.getValue("MQTT", "TLS_ENABLED", tls_str) && tls_str == "1")
  {
    m_tls_enabled = true;
    if (!m_cfg.getValue("MQTT", "TLS_CA_CERT", m_tls_ca_cert) ||
        m_tls_ca_cert.empty())
    {
      geulog::error("mqtt", "MQTT/TLS_CA_CERT required when TLS_ENABLED=1");
      return false;
    }
    m_cfg.getValue("MQTT", "TLS_CLIENT_CERT", m_tls_client_cert);
    m_cfg.getValue("MQTT", "TLS_CLIENT_KEY", m_tls_client_key);
  }

  // Initialize libmosquitto
  mosquitto_lib_init();

  m_mosq = mosquitto_new(nullptr, true, nullptr);
  if (m_mosq == nullptr)
  {
    geulog::error("mqtt", "mosquitto_new() failed");
    return false;
  }

  mosquitto_connect_callback_set(m_mosq, on_connect_cb);
  mosquitto_disconnect_callback_set(m_mosq, on_disconnect_cb);

  // Set credentials
  mosquitto_username_pw_set(m_mosq, m_username.c_str(), m_password.c_str());

  // Configure TLS if enabled
  if (m_tls_enabled)
  {
    const char* client_cert =
        m_tls_client_cert.empty() ? nullptr : m_tls_client_cert.c_str();
    const char* client_key =
        m_tls_client_key.empty() ? nullptr : m_tls_client_key.c_str();
    int rc = mosquitto_tls_set(m_mosq, m_tls_ca_cert.c_str(), nullptr,
                                client_cert, client_key, nullptr);
    if (rc != MOSQ_ERR_SUCCESS)
    {
      geulog::error("mqtt", "MQTT TLS configuration failed: ",
                    mosquitto_strerror(rc));
      return false;
    }
  }

  // Auto-reconnect with 1s initial, 30s max, no exponential backoff
  mosquitto_reconnect_delay_set(m_mosq, 1, 30, false);

  // Start background thread for network I/O
  int rc = mosquitto_loop_start(m_mosq);
  if (rc != MOSQ_ERR_SUCCESS)
  {
    geulog::error("mqtt", "mosquitto_loop_start() failed: ",
                  mosquitto_strerror(rc));
    return false;
  }

  // Initiate async connection (non-blocking, handled by background thread)
  rc = mosquitto_connect_async(m_mosq, m_host.c_str(), m_port, 60);
  if (rc != MOSQ_ERR_SUCCESS)
  {
    geulog::warn("mqtt", "MQTT initial connect failed: ",
                 mosquitto_strerror(rc), " -- will retry in background");
  }

  geulog::info("mqtt", "MQTT: Publisher initialized, broker=", m_host, ":",
               m_port, " topic_prefix=", m_topic_prefix);

  return true;
}


void MqttPublisher::shutdown(void)
{
  if (m_mosq != nullptr)
  {
    mosquitto_disconnect(m_mosq);
    mosquitto_loop_stop(m_mosq, false);
    mosquitto_destroy(m_mosq);
    m_mosq = nullptr;
    mosquitto_lib_cleanup();
    geulog::info("mqtt", "MQTT: Publisher shut down");
  }
}


void MqttPublisher::publish(const std::string& topic_suffix,
                            const std::string& payload, bool retain)
{
  if (m_mosq == nullptr)
  {
    return;
  }
  string topic = m_topic_prefix + "/" + topic_suffix;
  int rc = mosquitto_publish(m_mosq, nullptr, topic.c_str(),
                             static_cast<int>(payload.size()), payload.c_str(),
                             0, retain);
  if (rc == MOSQ_ERR_SUCCESS)
  {
    return;
  }

  // Rate-limit: log first failure, log on error-code change, otherwise
  // suppress for 60s. Include suppressed count when we do log.
  const time_t now = time(nullptr);
  const bool first        = (m_last_pub_err_logged == 0);
  const bool code_changed = (rc != m_last_pub_err_code);
  const bool cooldown_ok  = (now - m_last_pub_err_logged >= 60);

  if (first || code_changed || cooldown_ok)
  {
    if (m_pub_err_suppressed > 0)
    {
      geulog::warn("mqtt", "MQTT publish failed: topic=", topic,
                   " rc=", rc, " (", mosquitto_strerror(rc), ")",
                   " [suppressed ", m_pub_err_suppressed, " prior]");
    }
    else
    {
      geulog::warn("mqtt", "MQTT publish failed: topic=", topic,
                   " rc=", rc, " (", mosquitto_strerror(rc), ")");
    }
    m_last_pub_err_code   = rc;
    m_last_pub_err_logged = now;
    m_pub_err_suppressed  = 0;
  }
  else
  {
    ++m_pub_err_suppressed;
  }
}


void MqttPublisher::onTalkerStart(uint32_t tg, const std::string& callsign,
                                  bool is_trunk)
{
  Json::Value payload;
  payload["callsign"] = callsign;
  payload["source"] = is_trunk ? "trunk" : "local";
  Json::StreamWriterBuilder wb;
  wb["indentation"] = "";
  string topic = "talker/" + to_string(tg) + "/start";
  publish(topic, Json::writeString(wb, payload));
}


void MqttPublisher::onTalkerStop(uint32_t tg, const std::string& callsign,
                                 bool is_trunk)
{
  Json::Value payload;
  payload["callsign"] = callsign;
  payload["source"] = is_trunk ? "trunk" : "local";
  Json::StreamWriterBuilder wb;
  wb["indentation"] = "";
  string topic = "talker/" + to_string(tg) + "/stop";
  publish(topic, Json::writeString(wb, payload));
}


void MqttPublisher::onPeerTalkerStart(const std::string& peer_id,
                                      uint32_t tg,
                                      const std::string& callsign)
{
  Json::Value payload;
  payload["callsign"] = callsign;
  payload["tg"] = static_cast<Json::UInt>(tg);
  Json::StreamWriterBuilder wb;
  wb["indentation"] = "";
  string topic = "peer/" + peer_id + "/talker/" + to_string(tg) + "/start";
  publish(topic, Json::writeString(wb, payload));
}


void MqttPublisher::onPeerTalkerStop(const std::string& peer_id,
                                     uint32_t tg,
                                     const std::string& callsign)
{
  Json::Value payload;
  payload["callsign"] = callsign;
  payload["tg"] = static_cast<Json::UInt>(tg);
  Json::StreamWriterBuilder wb;
  wb["indentation"] = "";
  string topic = "peer/" + peer_id + "/talker/" + to_string(tg) + "/stop";
  publish(topic, Json::writeString(wb, payload));
}


void MqttPublisher::onClientConnected(const std::string& callsign,
                                      uint32_t tg, const std::string& ip)
{
  Json::Value payload;
  payload["tg"] = tg;
  payload["ip"] = ip;
  Json::StreamWriterBuilder wb;
  wb["indentation"] = "";
  string topic = "client/" + callsign + "/connected";
  publish(topic, Json::writeString(wb, payload));
}


void MqttPublisher::onClientDisconnected(const std::string& callsign)
{
  Json::StreamWriterBuilder wb;
  wb["indentation"] = "";
  Json::Value payload(Json::objectValue);
  publish("client/" + callsign + "/disconnected",
          Json::writeString(wb, payload));
  // Clear retained per-client topics so a long-disconnected callsign
  // does not linger in the broker store.
  publish("client/" + callsign + "/rx", "", true);
  publish("client/" + callsign + "/status", "", true);
}


void MqttPublisher::onTrunkUp(const std::string& section,
                              const std::string& direction,
                              const std::string& host, uint16_t port)
{
  Json::Value payload;
  payload["host"] = host;
  payload["port"] = port;
  Json::StreamWriterBuilder wb;
  wb["indentation"] = "";
  string topic = "trunk/" + section + "/" + direction + "/up";
  publish(topic, Json::writeString(wb, payload));
}


void MqttPublisher::onTrunkDown(const std::string& section,
                                const std::string& direction)
{
  Json::StreamWriterBuilder wb;
  wb["indentation"] = "";
  Json::Value payload(Json::objectValue);
  string topic = "trunk/" + section + "/" + direction + "/down";
  publish(topic, Json::writeString(wb, payload));
}


void MqttPublisher::onRxUpdate(const std::string& callsign,
                               const Json::Value& rx_json)
{
  Json::StreamWriterBuilder wb;
  wb["indentation"] = "";
  string topic = "client/" + callsign + "/rx";
  publish(topic, Json::writeString(wb, rx_json), true);  // retained
}


void MqttPublisher::publishFullStatus(const Json::Value& status)
{
  Json::StreamWriterBuilder wb;
  wb["indentation"] = "";
  publish("status", Json::writeString(wb, status), true);
}


static Json::Value nodeListToJson(
    const std::vector<MsgPeerNodeList::NodeEntry>& nodes)
{
  Json::Value arr(Json::arrayValue);
  for (const auto& n : nodes)
  {
    // Prefer the rich per-client status blob; fall back to the flat
    // wire fields if the source didn't ship one (older fork builds).
    Json::Value e = n.status.isObject() ? n.status
                                        : Json::Value(Json::objectValue);
    e["callsign"] = n.callsign;
    e["tg"]       = n.tg;
    if (n.lat != 0.0f || n.lon != 0.0f)
    {
      e["lat"] = n.lat;
      e["lon"] = n.lon;
    }
    if (!n.qth_name.empty()) e["qth_name"] = n.qth_name;
    arr.append(e);
  }
  Json::Value root;
  root["nodes"]     = arr;
  root["timestamp"] = (Json::Int64)time(nullptr);
  return root;
}


void MqttPublisher::publishLocalNodes(
    const std::vector<MsgPeerNodeList::NodeEntry>& nodes)
{
  Json::StreamWriterBuilder wb;
  wb["indentation"] = "";
  publish("nodes/local",
          Json::writeString(wb, nodeListToJson(nodes)), true);
}


void MqttPublisher::publishPeerNodes(const std::string& peer_id,
    const std::vector<MsgPeerNodeList::NodeEntry>& nodes)
{
  Json::StreamWriterBuilder wb;
  wb["indentation"] = "";
  publish("nodes/" + peer_id,
          Json::writeString(wb, nodeListToJson(nodes)), true);
}


void MqttPublisher::onClientStatus(const std::string& callsign,
                                   const Json::Value& status_json)
{
  Json::StreamWriterBuilder wb;
  wb["indentation"] = "";
  publish("client/" + callsign + "/status",
          Json::writeString(wb, status_json), true);  // retained
}


void MqttPublisher::publishPeerClientEvent(const std::string& peer_id,
                                           const std::string& callsign,
                                           const std::string& event,
                                           const Json::Value& payload,
                                           bool retained)
{
  Json::StreamWriterBuilder wb;
  wb["indentation"] = "";
  std::string topic =
      "peer/" + peer_id + "/client/" + callsign + "/" + event;
  publish(topic, Json::writeString(wb, payload), retained);
}


void MqttPublisher::clearPeerClientRetained(const std::string& peer_id,
                                            const std::string& callsign)
{
  std::string base = "peer/" + peer_id + "/client/" + callsign + "/";
  publish(base + "rx", "", true);
  publish(base + "status", "", true);
}
