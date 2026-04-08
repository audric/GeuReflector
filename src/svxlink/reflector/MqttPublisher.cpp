#include "MqttPublisher.h"

#include <iostream>
#include <sstream>
#include <mosquitto.h>
#include <AsyncConfig.h>

using namespace std;


static void on_connect_cb(struct mosquitto*, void*, int rc)
{
  if (rc == 0)
  {
    cout << "MQTT: Connected to broker" << endl;
  }
  else
  {
    cerr << "*** WARNING: MQTT connection failed: "
         << mosquitto_connack_string(rc) << endl;
  }
}


static void on_disconnect_cb(struct mosquitto*, void*, int rc)
{
  if (rc != 0)
  {
    cerr << "*** WARNING: MQTT disconnected unexpectedly (rc=" << rc
         << "), will reconnect" << endl;
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
    cerr << "*** ERROR: MQTT/HOST not configured" << endl;
    return false;
  }

  string port_str;
  if (m_cfg.getValue("MQTT", "PORT", port_str))
  {
    m_port = static_cast<uint16_t>(stoi(port_str));
  }
  else
  {
    cerr << "*** ERROR: MQTT/PORT not configured" << endl;
    return false;
  }

  if (!m_cfg.getValue("MQTT", "USERNAME", m_username) || m_username.empty())
  {
    cerr << "*** ERROR: MQTT/USERNAME not configured" << endl;
    return false;
  }

  if (!m_cfg.getValue("MQTT", "PASSWORD", m_password) || m_password.empty())
  {
    cerr << "*** ERROR: MQTT/PASSWORD not configured" << endl;
    return false;
  }

  if (!m_cfg.getValue("MQTT", "TOPIC_PREFIX", m_topic_prefix) ||
      m_topic_prefix.empty())
  {
    cerr << "*** ERROR: MQTT/TOPIC_PREFIX not configured" << endl;
    return false;
  }

  // Strip trailing slash from topic prefix
  while (!m_topic_prefix.empty() && m_topic_prefix.back() == '/')
  {
    m_topic_prefix.pop_back();
  }

  // Read optional TLS config
  string tls_str;
  if (m_cfg.getValue("MQTT", "TLS_ENABLED", tls_str) && tls_str == "1")
  {
    m_tls_enabled = true;
    if (!m_cfg.getValue("MQTT", "TLS_CA_CERT", m_tls_ca_cert) ||
        m_tls_ca_cert.empty())
    {
      cerr << "*** ERROR: MQTT/TLS_CA_CERT required when TLS_ENABLED=1" << endl;
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
    cerr << "*** ERROR: mosquitto_new() failed" << endl;
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
      cerr << "*** ERROR: MQTT TLS configuration failed: "
           << mosquitto_strerror(rc) << endl;
      return false;
    }
  }

  // Auto-reconnect with 1s initial, 30s max, no exponential backoff
  mosquitto_reconnect_delay_set(m_mosq, 1, 30, false);

  // Start background thread for network I/O
  int rc = mosquitto_loop_start(m_mosq);
  if (rc != MOSQ_ERR_SUCCESS)
  {
    cerr << "*** ERROR: mosquitto_loop_start() failed: "
         << mosquitto_strerror(rc) << endl;
    return false;
  }

  // Initiate async connection (non-blocking, handled by background thread)
  rc = mosquitto_connect_async(m_mosq, m_host.c_str(), m_port, 60);
  if (rc != MOSQ_ERR_SUCCESS)
  {
    cerr << "*** WARNING: MQTT initial connect failed: "
         << mosquitto_strerror(rc)
         << " -- will retry in background" << endl;
  }

  cout << "MQTT: Publisher initialized, broker=" << m_host << ":" << m_port
       << " topic_prefix=" << m_topic_prefix << endl;

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
    cout << "MQTT: Publisher shut down" << endl;
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
  mosquitto_publish(m_mosq, nullptr, topic.c_str(),
                    static_cast<int>(payload.size()), payload.c_str(),
                    0, retain);
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
  string topic = "client/" + callsign + "/disconnected";
  publish(topic, Json::writeString(wb, payload));
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


void MqttPublisher::publishFullStatus(const Json::Value& status)
{
  Json::StreamWriterBuilder wb;
  wb["indentation"] = "";
  publish("status", Json::writeString(wb, status), true);
}
