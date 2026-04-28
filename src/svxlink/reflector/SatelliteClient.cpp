#include <cmath>
#include <iostream>
#include <sstream>
#include <random>

#include <AsyncTcpConnection.h>

#include <Log.h>

#include "SatelliteClient.h"
#include "ReflectorMsg.h"
#include "Reflector.h"
#include "TGHandler.h"
#include "ReflectorClient.h"

using namespace std;
using namespace Async;
using namespace sigc;

namespace {

std::string sanitizeIdent(const std::string& in, size_t max_len)
{
  std::string out;
  out.reserve(std::min(in.size(), max_len));
  for (char c : in)
  {
    unsigned char u = static_cast<unsigned char>(c);
    if (u < 0x20 || u == 0x7f) continue;
    if (c == ':') continue;
    out += c;
    if (out.size() >= max_len) break;
  }
  return out;
}

std::string sanitizeText(const std::string& in, size_t max_bytes)
{
  std::string out;
  out.reserve(std::min(in.size(), max_bytes));
  for (char c : in)
  {
    unsigned char u = static_cast<unsigned char>(c);
    if (u < 0x20 || u == 0x7f) continue;
    out += c;
    if (out.size() >= max_bytes) break;
  }
  return out;
}

void sanitizeJsonStrings(Json::Value& v, unsigned depth = 0)
{
  static constexpr unsigned MAX_DEPTH       = 8;
  static constexpr Json::ArrayIndex MAX_LEN = 256;
  static constexpr size_t MAX_STR_BYTES     = 1024;
  if (depth >= MAX_DEPTH) { v = Json::Value(); return; }
  if (v.isString())
  {
    v = sanitizeText(v.asString(), MAX_STR_BYTES);
  }
  else if (v.isArray())
  {
    if (v.size() > MAX_LEN) v.resize(MAX_LEN);
    for (Json::ArrayIndex i = 0; i < v.size(); ++i)
    {
      sanitizeJsonStrings(v[i], depth + 1);
    }
  }
  else if (v.isObject())
  {
    auto names = v.getMemberNames();
    if (names.size() > MAX_LEN)
    {
      for (size_t i = MAX_LEN; i < names.size(); ++i)
      {
        v.removeMember(names[i]);
      }
      names.resize(MAX_LEN);
    }
    for (const auto& k : names)
    {
      sanitizeJsonStrings(v[k], depth + 1);
    }
  }
}

}  // namespace


SatelliteClient::SatelliteClient(Reflector* reflector, Async::Config& cfg)
  : m_reflector(reflector), m_cfg(cfg),
    m_parent_port(5303), m_priority(0), m_hello_received(false),
    m_heartbeat_timer(1000, Timer::TYPE_PERIODIC, false),
    m_hb_tx_cnt(0), m_hb_rx_cnt(0)
{
  std::random_device rd;
  std::mt19937 rng(rd());
  std::uniform_int_distribution<uint32_t> dist;
  m_priority = dist(rng);

  m_con.connected.connect(mem_fun(*this, &SatelliteClient::onConnected));
  m_con.disconnected.connect(mem_fun(*this, &SatelliteClient::onDisconnected));
  m_con.frameReceived.connect(mem_fun(*this, &SatelliteClient::onFrameReceived));
  m_con.setMaxFrameSize(ReflectorMsg::MAX_POSTAUTH_FRAME_SIZE);

  m_heartbeat_timer.expired.connect(
      mem_fun(*this, &SatelliteClient::heartbeatTick));
} /* SatelliteClient::SatelliteClient */


SatelliteClient::~SatelliteClient(void)
{
  TGHandler::instance()->clearAllTrunkTalkers();
} /* SatelliteClient::~SatelliteClient */


bool SatelliteClient::initialize(void)
{
  if (!m_cfg.getValue("GLOBAL", "SATELLITE_OF", m_parent_host) ||
      m_parent_host.empty())
  {
    geulog::error("satellite", "Missing SATELLITE_OF in [GLOBAL]");
    return false;
  }

  m_cfg.getValue("GLOBAL", "SATELLITE_PORT", m_parent_port);

  if (!m_cfg.getValue("GLOBAL", "SATELLITE_SECRET", m_secret) ||
      m_secret.empty())
  {
    geulog::error("satellite", "Missing SATELLITE_SECRET in [GLOBAL]");
    return false;
  }

  // Use a satellite ID — hostname or configured name
  m_satellite_id = "satellite";
  m_cfg.getValue("GLOBAL", "SATELLITE_ID", m_satellite_id);

  // Optional TG filter — restrict which TGs to receive from parent
  if (m_cfg.getValue("GLOBAL", "SATELLITE_FILTER", m_filter_str) &&
      !m_filter_str.empty())
  {
    m_filter = TgFilter::parse(m_filter_str);
    geulog::info("satellite", "TG filter: ", m_filter.toString());
  }

  geulog::info("satellite", "Connecting to parent ", m_parent_host,
               ":", m_parent_port, " as '", m_satellite_id, "'");

  m_con.addStaticSRVRecord(0, 0, 0, m_parent_port, m_parent_host);
  m_con.connect();

  return true;
} /* SatelliteClient::initialize */


void SatelliteClient::onLocalTalkerStart(uint32_t tg,
                                          const std::string& callsign)
{
  if (!m_con.isConnected() || !m_hello_received) return;
  if (!m_filter.empty() && !m_filter.matches(tg)) return;
  sendMsg(MsgPeerTalkerStart(tg, callsign));
} /* SatelliteClient::onLocalTalkerStart */


void SatelliteClient::onLocalTalkerStop(uint32_t tg)
{
  if (!m_con.isConnected() || !m_hello_received) return;
  if (!m_filter.empty() && !m_filter.matches(tg)) return;
  sendMsg(MsgPeerTalkerStop(tg));
} /* SatelliteClient::onLocalTalkerStop */


void SatelliteClient::onLocalAudio(uint32_t tg,
                                    const std::vector<uint8_t>& audio)
{
  if (!m_con.isConnected() || !m_hello_received) return;
  if (!m_filter.empty() && !m_filter.matches(tg)) return;
  sendMsg(MsgPeerAudio(tg, audio));
} /* SatelliteClient::onLocalAudio */


void SatelliteClient::onLocalFlush(uint32_t tg)
{
  if (!m_con.isConnected() || !m_hello_received) return;
  if (!m_filter.empty() && !m_filter.matches(tg)) return;
  sendMsg(MsgPeerFlush(tg));
} /* SatelliteClient::onLocalFlush */


void SatelliteClient::onConnected(void)
{
  geulog::info("satellite", "Connected to parent ", m_con.remoteHost(),
               ":", m_con.remotePort());

  m_hello_received = false;
  m_hb_tx_cnt = HEARTBEAT_TX_CNT_RESET;
  m_hb_rx_cnt = HEARTBEAT_RX_CNT_RESET;

  sendMsg(MsgPeerHello(m_satellite_id, "", m_priority, m_secret,
                         MsgPeerHello::ROLE_SATELLITE));

  m_heartbeat_timer.setEnable(true);
} /* SatelliteClient::onConnected */


void SatelliteClient::onDisconnected(TcpConnection* con,
                                      TcpConnection::DisconnectReason reason)
{
  geulog::info("satellite", "Disconnected from parent: ",
               TcpConnection::disconnectReasonStr(reason));

  m_heartbeat_timer.setEnable(false);
  m_hello_received = false;
  TGHandler::instance()->clearAllTrunkTalkers();

  if (!m_parent_nodes.empty() && !m_parent_id.empty())
  {
    // Tombstone-clear Redis live:peer_node:<parent_id>:* and let MQTT
    // emit an empty list so consumers see the partner roster drop.
    m_reflector->onPeerNodeList(m_parent_id,
        std::vector<MsgPeerNodeList::NodeEntry>{});
    m_parent_nodes.clear();
  }
} /* SatelliteClient::onDisconnected */


void SatelliteClient::onFrameReceived(FramedTcpConnection* con,
                                       std::vector<uint8_t>& data)
{
  auto buf = reinterpret_cast<const char*>(data.data());
  stringstream ss;
  ss.write(buf, data.size());

  ReflectorMsg header;
  if (!header.unpack(ss))
  {
    geulog::error("satellite", "Failed to unpack message header");
    return;
  }

  if (!m_hello_received &&
      header.type() != MsgPeerHello::TYPE &&
      header.type() != MsgPeerHeartbeat::TYPE)
  {
    return;
  }

  m_hb_rx_cnt = HEARTBEAT_RX_CNT_RESET;

  switch (header.type())
  {
    case MsgPeerHeartbeat::TYPE:
      handleMsgPeerHeartbeat();
      break;
    case MsgPeerHello::TYPE:
      handleMsgPeerHello(ss);
      break;
    case MsgPeerTalkerStart::TYPE:
      handleMsgPeerTalkerStart(ss);
      break;
    case MsgPeerTalkerStop::TYPE:
      handleMsgPeerTalkerStop(ss);
      break;
    case MsgPeerAudio::TYPE:
      handleMsgPeerAudio(ss);
      break;
    case MsgPeerFlush::TYPE:
      handleMsgPeerFlush(ss);
      break;
    case MsgPeerNodeList::TYPE:
      handleMsgPeerNodeList(ss);
      break;
    case MsgPeerClientConnected::TYPE:
      handleMsgPeerClientConnected(ss);
      break;
    case MsgPeerClientDisconnected::TYPE:
      handleMsgPeerClientDisconnected(ss);
      break;
    case MsgPeerClientRx::TYPE:
      handleMsgPeerClientRx(ss);
      break;
    case MsgPeerClientStatus::TYPE:
      handleMsgPeerClientStatus(ss);
      break;
    default:
      break;
  }
} /* SatelliteClient::onFrameReceived */


void SatelliteClient::handleMsgPeerHeartbeat(void)
{
} /* SatelliteClient::handleMsgPeerHeartbeat */


void SatelliteClient::handleMsgPeerHello(std::istream& is)
{
  MsgPeerHello msg;
  if (!msg.unpack(is))
  {
    geulog::error("satellite", "Failed to unpack MsgPeerHello");
    return;
  }

  if (!msg.verify(m_secret))
  {
    geulog::error("satellite", "Parent authentication failed");
    m_con.disconnect();
    return;
  }

  m_hello_received = true;
  m_parent_id = sanitizeIdent(msg.id(), 64);
  geulog::info("satellite", "Parent authenticated (id='", msg.id(), "')");

  // Advertise our TG filter to the parent so it can skip TGs we
  // don't want. The parent applies the filter on its outbound path.
  sendFilter();

  // Trigger the reflector to push our local roster up immediately so the
  // parent can fan it out (debounced) — otherwise the first node-list
  // emission only happens on the next client login/TG-change.
  m_reflector->scheduleNodeListUpdate();
} /* SatelliteClient::handleMsgPeerHello */


void SatelliteClient::sendFilter(void)
{
  if (m_filter_str.empty()) return;
  sendMsg(MsgPeerFilter(m_filter_str));
  geulog::info("satellite", "Sent TG filter to parent: ", m_filter_str);
} /* SatelliteClient::sendFilter */


void SatelliteClient::handleMsgPeerTalkerStart(std::istream& is)
{
  MsgPeerTalkerStart msg;
  if (!msg.unpack(is)) return;

  // Register as trunk talker — fires trunkTalkerUpdated which
  // broadcasts MsgTalkerStart to local clients
  TGHandler::instance()->setTrunkTalkerForTG(msg.tg(), msg.callsign());
} /* SatelliteClient::handleMsgPeerTalkerStart */


void SatelliteClient::handleMsgPeerTalkerStop(std::istream& is)
{
  MsgPeerTalkerStop msg;
  if (!msg.unpack(is)) return;

  TGHandler::instance()->clearTrunkTalkerForTG(msg.tg());
} /* SatelliteClient::handleMsgPeerTalkerStop */


void SatelliteClient::handleMsgPeerAudio(std::istream& is)
{
  MsgPeerAudio msg;
  if (!msg.unpack(is)) return;

  if (msg.audio().empty()) return;

  MsgUdpAudio udp_msg(msg.audio());
  m_reflector->broadcastUdpMsg(udp_msg,
      ReflectorClient::TgFilter(msg.tg()));
} /* SatelliteClient::handleMsgPeerAudio */


void SatelliteClient::handleMsgPeerFlush(std::istream& is)
{
  MsgPeerFlush msg;
  if (!msg.unpack(is)) return;

  m_reflector->broadcastUdpMsg(MsgUdpFlushSamples(),
      ReflectorClient::TgFilter(msg.tg()));
} /* SatelliteClient::handleMsgPeerFlush */


void SatelliteClient::handleMsgPeerNodeList(std::istream& is)
{
  MsgPeerNodeList msg;
  if (!msg.unpack(is))
  {
    geulog::error("satellite", "Failed to unpack MsgPeerNodeList");
    return;
  }

  std::vector<MsgPeerNodeList::NodeEntry> sanitized;
  sanitized.reserve(msg.nodes().size());
  unsigned dropped = 0;
  for (const auto& n : msg.nodes())
  {
    MsgPeerNodeList::NodeEntry e;
    e.callsign = sanitizeIdent(n.callsign, 32);
    if (e.callsign.empty()) { ++dropped; continue; }
    e.tg       = n.tg;
    e.qth_name = sanitizeText(n.qth_name, 64);
    if (std::isfinite(n.lat) && std::isfinite(n.lon) &&
        n.lat >= -90.0f && n.lat <= 90.0f &&
        n.lon >= -180.0f && n.lon <= 180.0f)
    {
      e.lat = n.lat;
      e.lon = n.lon;
    }
    e.status = n.status;
    sanitizeJsonStrings(e.status);
    // Recipient-relative sat_id from the parent passes through unchanged:
    // empty = "on the parent itself", non-empty = "on a sibling sat
    // attached to the parent".
    e.sat_id = sanitizeIdent(n.sat_id, 64);
    sanitized.push_back(std::move(e));
  }
  if (dropped > 0)
  {
    geulog::warn("satellite", "Dropped ", dropped,
                 " parent-roster entrie(s) with empty/invalid callsign");
  }

  if (!m_parent_id.empty())
  {
    m_reflector->onPeerNodeList(m_parent_id, sanitized);
  }
  m_parent_nodes = std::move(sanitized);
} /* SatelliteClient::handleMsgPeerNodeList */


void SatelliteClient::sendNodeList(
    const std::vector<MsgPeerNodeList::NodeEntry>& nodes)
{
  if (!m_con.isConnected() || !m_hello_received) return;

  // Apply our outbound TG filter so we only push roster entries the
  // parent cares about (consistent with the audio-path scoping rule).
  std::vector<MsgPeerNodeList::NodeEntry> filtered;
  filtered.reserve(nodes.size());
  for (const auto& n : nodes)
  {
    if (!m_filter.empty() && !m_filter.matches(n.tg)) continue;
    filtered.push_back(n);
  }
  sendMsg(MsgPeerNodeList(filtered));
} /* SatelliteClient::sendNodeList */


void SatelliteClient::sendMsg(const ReflectorMsg& msg)
{
  ostringstream ss;
  ReflectorMsg header(msg.type());
  if (!header.pack(ss) || !msg.pack(ss))
  {
    geulog::error("satellite", "Failed to pack message type=", msg.type());
    return;
  }
  m_hb_tx_cnt = HEARTBEAT_TX_CNT_RESET;
  m_con.write(ss.str().data(), ss.str().size());
} /* SatelliteClient::sendMsg */


void SatelliteClient::sendClientConnected(const std::string& callsign,
                                           uint32_t tg,
                                           const std::string& ip)
{
  if (!m_con.isConnected() || !m_hello_received) return;
  // No TG filter on satellite->parent direction (parent doesn't filter
  // satellite-side clients today; if it ever advertises one, the same
  // filterPassesTg() pattern from SatelliteLink applies).
  sendMsg(MsgPeerClientConnected(callsign, tg, ip));
} /* SatelliteClient::sendClientConnected */


void SatelliteClient::sendClientDisconnected(const std::string& callsign)
{
  if (!m_con.isConnected() || !m_hello_received) return;
  sendMsg(MsgPeerClientDisconnected(callsign));
} /* SatelliteClient::sendClientDisconnected */


void SatelliteClient::sendClientRx(const std::string& callsign,
                                    const std::string& rx_json)
{
  if (!m_con.isConnected() || !m_hello_received) return;
  sendMsg(MsgPeerClientRx(callsign, rx_json));
} /* SatelliteClient::sendClientRx */


void SatelliteClient::sendClientStatus(const std::string& callsign,
                                        const std::string& status_json)
{
  if (!m_con.isConnected() || !m_hello_received) return;
  sendMsg(MsgPeerClientStatus(callsign, status_json));
} /* SatelliteClient::sendClientStatus */


void SatelliteClient::handleMsgPeerClientConnected(std::istream& is)
{
  MsgPeerClientConnected msg;
  if (!msg.unpack(is))
  {
    geulog::error("satellite",
        "Failed to unpack MsgPeerClientConnected from parent '",
        m_parent_id, "'");
    return;
  }
  Json::Value payload;
  payload["tg"] = static_cast<Json::UInt>(msg.tg());
  payload["ip"] = msg.ip();
  if (m_reflector->mqtt() != nullptr)
  {
    m_reflector->mqtt()->publishPeerClientEvent(
        m_parent_id, msg.callsign(), "connected", payload, false);
  }
} /* SatelliteClient::handleMsgPeerClientConnected */


void SatelliteClient::handleMsgPeerClientDisconnected(std::istream& is)
{
  MsgPeerClientDisconnected msg;
  if (!msg.unpack(is))
  {
    geulog::error("satellite",
        "Failed to unpack MsgPeerClientDisconnected from parent '",
        m_parent_id, "'");
    return;
  }
  Json::Value payload(Json::objectValue);
  if (m_reflector->mqtt() != nullptr)
  {
    m_reflector->mqtt()->publishPeerClientEvent(
        m_parent_id, msg.callsign(), "disconnected", payload, false);
  }
} /* SatelliteClient::handleMsgPeerClientDisconnected */


void SatelliteClient::handleMsgPeerClientRx(std::istream& is)
{
  MsgPeerClientRx msg;
  if (!msg.unpack(is))
  {
    geulog::error("satellite",
        "Failed to unpack MsgPeerClientRx from parent '", m_parent_id, "'");
    return;
  }
  if (msg.rxJson().size() > 65536u) return;
  Json::Value rx_json;
  std::istringstream ss(msg.rxJson());
  Json::CharReaderBuilder rb;
  std::string err;
  if (!Json::parseFromStream(rb, ss, &rx_json, &err))
  {
    return;
  }
  if (m_reflector->mqtt() != nullptr)
  {
    m_reflector->mqtt()->publishPeerClientEvent(
        m_parent_id, msg.callsign(), "rx", rx_json, true);  // retained
  }
} /* SatelliteClient::handleMsgPeerClientRx */


void SatelliteClient::handleMsgPeerClientStatus(std::istream& is)
{
  MsgPeerClientStatus msg;
  if (!msg.unpack(is))
  {
    geulog::error("satellite",
        "Failed to unpack MsgPeerClientStatus from parent '",
        m_parent_id, "'");
    return;
  }
  if (msg.statusJson().size() > 65536u) return;
  Json::Value status_json;
  std::istringstream ss(msg.statusJson());
  Json::CharReaderBuilder rb;
  std::string err;
  if (!Json::parseFromStream(rb, ss, &status_json, &err))
  {
    return;
  }
  if (m_reflector->mqtt() != nullptr)
  {
    m_reflector->mqtt()->publishPeerClientEvent(
        m_parent_id, msg.callsign(), "status", status_json, true);  // retained
  }
} /* SatelliteClient::handleMsgPeerClientStatus */


void SatelliteClient::heartbeatTick(Async::Timer* t)
{
  if (--m_hb_tx_cnt == 0)
  {
    m_hb_tx_cnt = HEARTBEAT_TX_CNT_RESET;
    sendMsg(MsgPeerHeartbeat());
  }

  if (--m_hb_rx_cnt == 0)
  {
    geulog::error("satellite", "Heartbeat timeout — disconnecting");
    m_con.disconnect();
  }
} /* SatelliteClient::heartbeatTick */
