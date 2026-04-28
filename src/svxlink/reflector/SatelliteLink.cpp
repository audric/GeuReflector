#include <cmath>
#include <iostream>
#include <sstream>

#include <Log.h>

#include "SatelliteLink.h"
#include "ReflectorMsg.h"
#include "Reflector.h"
#include "TGHandler.h"
#include "ReflectorClient.h"

using namespace std;
using namespace Async;

namespace {

// Byte-bound and strip control chars / ':' from an identifier coming over
// the wire. Mirrors the sanitizer used in TrunkLink/TwinLink — wire data
// flows into Redis key names, MQTT payloads and log output.
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


SatelliteLink::SatelliteLink(Reflector* reflector,
                             Async::FramedTcpConnection* con,
                             const std::string& secret)
  : m_reflector(reflector), m_con(con), m_secret(secret),
    m_hello_received(false),
    m_heartbeat_timer(1000, Timer::TYPE_PERIODIC),
    m_hb_tx_cnt(HEARTBEAT_TX_CNT_RESET),
    m_hb_rx_cnt(HEARTBEAT_RX_CNT_RESET)
{
  m_con->setMaxFrameSize(ReflectorMsg::MAX_POSTAUTH_FRAME_SIZE);
  m_con->frameReceived.connect(
      sigc::mem_fun(*this, &SatelliteLink::onFrameReceived));

  m_heartbeat_timer.expired.connect(
      sigc::mem_fun(*this, &SatelliteLink::heartbeatTick));
} /* SatelliteLink::SatelliteLink */


SatelliteLink::~SatelliteLink(void)
{
  m_heartbeat_timer.setEnable(false);
  for (uint32_t tg : m_sat_active_tgs)
  {
    TGHandler::instance()->clearTrunkTalkerForTG(tg);
  }
  m_sat_active_tgs.clear();
  // Drop any partner roster we held for this satellite so Reflector's
  // tombstone bookkeeping (Redis live:peer_node:<sat>:<callsign>) gets
  // a chance to clear those keys before we go away.
  if (!m_partner_nodes.empty() && !m_satellite_id.empty())
  {
    m_reflector->onPeerNodeList(m_satellite_id,
        std::vector<MsgPeerNodeList::NodeEntry>{});
    m_partner_nodes.clear();
  }
} /* SatelliteLink::~SatelliteLink */


Json::Value SatelliteLink::statusJson(void) const
{
  Json::Value obj(Json::objectValue);
  obj["id"] = m_satellite_id;
  obj["authenticated"] = m_hello_received;

  Json::Value active_tgs(Json::arrayValue);
  for (uint32_t tg : m_sat_active_tgs)
  {
    active_tgs.append(tg);
  }
  obj["active_tgs"] = active_tgs;

  if (!m_tg_filter.empty())
  {
    obj["filter"] = m_tg_filter.toString();
  }

  Json::Value nodes_arr(Json::arrayValue);
  for (const auto& n : m_partner_nodes)
  {
    Json::Value entry = n.status.isObject() ? n.status
                                            : Json::Value(Json::objectValue);
    entry["callsign"] = n.callsign;
    entry["tg"]       = static_cast<Json::UInt>(n.tg);
    if (n.lat != 0.0f || n.lon != 0.0f)
    {
      entry["lat"] = n.lat;
      entry["lon"] = n.lon;
    }
    if (!n.qth_name.empty()) entry["qth_name"] = n.qth_name;
    if (!n.sat_id.empty())   entry["sat_id"]   = n.sat_id;
    entry["isTalker"] =
        (TGHandler::instance()->trunkTalkerForTG(n.tg) == n.callsign);
    nodes_arr.append(entry);
  }
  obj["nodes"] = nodes_arr;

  return obj;
} /* SatelliteLink::statusJson */


void SatelliteLink::onParentTalkerStart(uint32_t tg,
                                         const std::string& callsign)
{
  if (!m_hello_received) return;
  if (!filterPassesTg(tg)) return;
  sendMsg(MsgPeerTalkerStart(tg, callsign));
} /* SatelliteLink::onParentTalkerStart */


void SatelliteLink::onParentTalkerStop(uint32_t tg)
{
  if (!m_hello_received) return;
  if (!filterPassesTg(tg)) return;
  sendMsg(MsgPeerTalkerStop(tg));
} /* SatelliteLink::onParentTalkerStop */


void SatelliteLink::onParentAudio(uint32_t tg,
                                   const std::vector<uint8_t>& audio)
{
  if (!m_hello_received) return;
  if (!filterPassesTg(tg)) return;
  sendMsg(MsgPeerAudio(tg, audio));
} /* SatelliteLink::onParentAudio */


void SatelliteLink::onParentFlush(uint32_t tg)
{
  if (!m_hello_received) return;
  if (!filterPassesTg(tg)) return;
  sendMsg(MsgPeerFlush(tg));
} /* SatelliteLink::onParentFlush */


void SatelliteLink::onFrameReceived(FramedTcpConnection* con,
                                     std::vector<uint8_t>& data)
{
  auto buf = reinterpret_cast<const char*>(data.data());
  stringstream ss;
  ss.write(buf, data.size());

  ReflectorMsg header;
  if (!header.unpack(ss))
  {
    geulog::error("satellite", "Failed to unpack satellite message header");
    return;
  }

  if (!m_hello_received &&
      header.type() != MsgPeerHello::TYPE &&
      header.type() != MsgPeerHeartbeat::TYPE)
  {
    geulog::warn("satellite", "Ignoring message type=", header.type(),
                 " before hello");
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
    case MsgPeerFilter::TYPE:
      handleMsgPeerFilter(ss);
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
      geulog::warn("satellite", "Unknown message type=", header.type());
      break;
  }
} /* SatelliteLink::onFrameReceived */


void SatelliteLink::handleMsgPeerHeartbeat(void)
{
} /* SatelliteLink::handleMsgPeerHeartbeat */


void SatelliteLink::handleMsgPeerHello(std::istream& is)
{
  MsgPeerHello msg;
  if (!msg.unpack(is))
  {
    geulog::error("satellite", "Failed to unpack MsgPeerHello");
    return;
  }

  if (msg.id().empty())
  {
    geulog::error("satellite", "Satellite sent empty ID");
    m_con->disconnect();
    return;
  }

  if (msg.role() != MsgPeerHello::ROLE_SATELLITE)
  {
    geulog::error("satellite", "Expected ROLE_SATELLITE from '", msg.id(),
                  "' but got role=", (int)msg.role());
    m_con->disconnect();
    return;
  }

  if (!msg.verify(m_secret))
  {
    geulog::error("satellite", "Authentication failed for satellite '",
                  msg.id(), "'");
    m_con->disconnect();
    return;
  }

  m_satellite_id = msg.id();
  m_hello_received = true;

  geulog::info("satellite", "Satellite '", m_satellite_id, "' authenticated");

  // Send hello reply so the satellite client can set m_hello_received
  // and start forwarding local events.  This also generates early
  // parent→satellite traffic, keeping the connection alive through
  // firewalls / NAT devices that drop idle TCP sessions.
  sendMsg(MsgPeerHello("PARENT", "", 0, m_secret,
                         MsgPeerHello::ROLE_PEER));

  // Bring the new satellite up to speed: schedule a debounced node-list
  // emission so it receives the parent's combined view (parent-local
  // + every sibling satellite, minus its own contribution).
  m_reflector->scheduleNodeListUpdate();

  statusChanged(this);
} /* SatelliteLink::handleMsgPeerHello */


void SatelliteLink::handleMsgPeerTalkerStart(std::istream& is)
{
  MsgPeerTalkerStart msg;
  if (!msg.unpack(is)) return;

  uint32_t tg = msg.tg();

  // Register as trunk talker — fires trunkTalkerUpdated which notifies
  // local clients. Reflector::onTrunkTalkerUpdated also forwards to
  // other satellites and trunk peers.
  m_sat_active_tgs.insert(tg);
  TGHandler::instance()->setTrunkTalkerForTGViaPeer(tg, msg.callsign(),
                                                    m_satellite_id);

  // Forward to trunk peers
  m_reflector->forwardSatelliteAudioToTrunks(tg, msg.callsign());

  statusChanged(this);
} /* SatelliteLink::handleMsgPeerTalkerStart */


void SatelliteLink::handleMsgPeerTalkerStop(std::istream& is)
{
  MsgPeerTalkerStop msg;
  if (!msg.unpack(is)) return;

  uint32_t tg = msg.tg();
  m_sat_active_tgs.erase(tg);
  TGHandler::instance()->clearTrunkTalkerForTG(tg);

  // Forward stop to trunk peers
  m_reflector->forwardSatelliteStopToTrunks(tg);

  statusChanged(this);
} /* SatelliteLink::handleMsgPeerTalkerStop */


void SatelliteLink::handleMsgPeerAudio(std::istream& is)
{
  MsgPeerAudio msg;
  if (!msg.unpack(is)) return;

  uint32_t tg = msg.tg();
  if (msg.audio().empty()) return;
  if (m_sat_active_tgs.find(tg) == m_sat_active_tgs.end()) return;

  // Broadcast to local clients on the parent
  MsgUdpAudio udp_msg(msg.audio());
  m_reflector->broadcastUdpMsg(udp_msg, ReflectorClient::TgFilter(tg));

  // Forward to trunk peers
  m_reflector->forwardSatelliteRawAudioToTrunks(tg, msg.audio());

  // Forward to other satellites (not this one)
  m_reflector->forwardAudioToSatellitesExcept(this, tg, msg.audio());
} /* SatelliteLink::handleMsgPeerAudio */


void SatelliteLink::handleMsgPeerFlush(std::istream& is)
{
  MsgPeerFlush msg;
  if (!msg.unpack(is)) return;

  uint32_t tg = msg.tg();

  m_reflector->broadcastUdpMsg(MsgUdpFlushSamples(),
      ReflectorClient::TgFilter(tg));

  m_reflector->forwardSatelliteFlushToTrunks(tg);
  m_reflector->forwardFlushToSatellitesExcept(this, tg);
} /* SatelliteLink::handleMsgPeerFlush */


void SatelliteLink::sendMsg(const ReflectorMsg& msg)
{
  ostringstream ss;
  ReflectorMsg header(msg.type());
  if (!header.pack(ss) || !msg.pack(ss))
  {
    geulog::error("satellite", "Failed to pack message type=", msg.type());
    return;
  }
  m_hb_tx_cnt = HEARTBEAT_TX_CNT_RESET;
  m_con->write(ss.str().data(), ss.str().size());
} /* SatelliteLink::sendMsg */


void SatelliteLink::heartbeatTick(Async::Timer* t)
{
  if (--m_hb_tx_cnt == 0)
  {
    m_hb_tx_cnt = HEARTBEAT_TX_CNT_RESET;
    sendMsg(MsgPeerHeartbeat());
  }

  if (--m_hb_rx_cnt == 0)
  {
    geulog::error("satellite", "Satellite '", m_satellite_id,
                  "': Heartbeat timeout — disconnecting");
    m_heartbeat_timer.setEnable(false);
    linkFailed(this);
  }
} /* SatelliteLink::heartbeatTick */


void SatelliteLink::handleMsgPeerFilter(std::istream& is)
{
  MsgPeerFilter msg;
  if (!msg.unpack(is))
  {
    geulog::error("satellite", "Failed to unpack MsgPeerFilter from '",
                  m_satellite_id, "'");
    return;
  }

  if (msg.filter().empty())
  {
    // Empty filter = clear any previously set filter (allow all)
    m_tg_filter = TgFilter();
    geulog::info("satellite", "Satellite '", m_satellite_id,
                 "': TG filter cleared (all TGs forwarded)");
  }
  else
  {
    m_tg_filter = TgFilter::parse(msg.filter());
    geulog::info("satellite", "Satellite '", m_satellite_id,
                 "': TG filter set: ", m_tg_filter.toString());
  }

  statusChanged(this);
} /* SatelliteLink::handleMsgPeerFilter */


bool SatelliteLink::filterPassesTg(uint32_t tg) const
{
  // Empty filter = no filtering = pass everything
  if (m_tg_filter.empty()) return true;
  return m_tg_filter.matches(tg);
} /* SatelliteLink::filterPassesTg */


void SatelliteLink::handleMsgPeerNodeList(std::istream& is)
{
  MsgPeerNodeList msg;
  if (!msg.unpack(is))
  {
    geulog::error("satellite", "Failed to unpack MsgPeerNodeList from '",
                  m_satellite_id, "'");
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
    // Stamp every entry with this satellite's id. Wire sat_id from a
    // direct satellite is normally empty (its own clients are local to
    // it); a non-empty value would indicate a deeper sat-of-sat
    // hierarchy, which we collapse to a single level here.
    e.sat_id = m_satellite_id;
    sanitized.push_back(std::move(e));
  }
  if (dropped > 0)
  {
    geulog::warn("satellite", "'", m_satellite_id, "' dropped ", dropped,
                 " node list entrie(s) with empty/invalid callsign");
  }

  m_reflector->onPeerNodeList(m_satellite_id, sanitized);
  m_partner_nodes = std::move(sanitized);

  // The parent's combined view changed — re-broadcast (debounced) to
  // trunks, twin, and sibling satellites so they pick up the new sat
  // contribution.
  m_reflector->scheduleNodeListUpdate();

  statusChanged(this);
} /* SatelliteLink::handleMsgPeerNodeList */


void SatelliteLink::sendNodeList(
    const std::vector<MsgPeerNodeList::NodeEntry>& nodes)
{
  if (!m_hello_received) return;

  // Apply this satellite's announced filter so we only forward TGs the
  // satellite cares about (matches the audio-path scoping rule).
  std::vector<MsgPeerNodeList::NodeEntry> filtered;
  filtered.reserve(nodes.size());
  for (const auto& n : nodes)
  {
    if (!filterPassesTg(n.tg)) continue;
    filtered.push_back(n);
  }
  sendMsg(MsgPeerNodeList(filtered));
} /* SatelliteLink::sendNodeList */


void SatelliteLink::sendClientConnected(const std::string& callsign,
                                        uint32_t tg,
                                        const std::string& ip)
{
  if (!m_hello_received) return;
  // TG filter applied at sender — drop if the client's TG is outside
  // this satellite's filter.
  if (!filterPassesTg(tg)) return;
  sendMsg(MsgPeerClientConnected(callsign, tg, ip));
} /* SatelliteLink::sendClientConnected */


void SatelliteLink::sendClientDisconnected(const std::string& callsign)
{
  if (!m_hello_received) return;
  // No TG in this message — filter is applied at the caller
  // (Reflector::fanoutClientDisconnected) using current-TG lookup.
  sendMsg(MsgPeerClientDisconnected(callsign));
} /* SatelliteLink::sendClientDisconnected */


void SatelliteLink::sendClientRx(const std::string& callsign,
                                  const std::string& rx_json)
{
  if (!m_hello_received) return;
  sendMsg(MsgPeerClientRx(callsign, rx_json));
} /* SatelliteLink::sendClientRx */


void SatelliteLink::sendClientStatus(const std::string& callsign,
                                      const std::string& status_json)
{
  if (!m_hello_received) return;
  sendMsg(MsgPeerClientStatus(callsign, status_json));
} /* SatelliteLink::sendClientStatus */


void SatelliteLink::handleMsgPeerClientConnected(std::istream& is)
{
  MsgPeerClientConnected msg;
  if (!msg.unpack(is))
  {
    geulog::error("satellite",
        "Failed to unpack MsgPeerClientConnected from '",
        m_satellite_id, "'");
    return;
  }
  Json::Value payload;
  payload["tg"] = static_cast<Json::UInt>(msg.tg());
  payload["ip"] = msg.ip();
  if (m_reflector->mqtt() != nullptr)
  {
    m_reflector->mqtt()->publishPeerClientEvent(
        m_satellite_id, msg.callsign(), "connected", payload, false);
  }
} /* SatelliteLink::handleMsgPeerClientConnected */


void SatelliteLink::handleMsgPeerClientDisconnected(std::istream& is)
{
  MsgPeerClientDisconnected msg;
  if (!msg.unpack(is))
  {
    geulog::error("satellite",
        "Failed to unpack MsgPeerClientDisconnected from '",
        m_satellite_id, "'");
    return;
  }
  Json::Value payload(Json::objectValue);
  if (m_reflector->mqtt() != nullptr)
  {
    m_reflector->mqtt()->publishPeerClientEvent(
        m_satellite_id, msg.callsign(), "disconnected", payload, false);
  }
} /* SatelliteLink::handleMsgPeerClientDisconnected */


void SatelliteLink::handleMsgPeerClientRx(std::istream& is)
{
  MsgPeerClientRx msg;
  if (!msg.unpack(is))
  {
    geulog::error("satellite",
        "Failed to unpack MsgPeerClientRx from '", m_satellite_id, "'");
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
        m_satellite_id, msg.callsign(), "rx", rx_json, true);
  }
} /* SatelliteLink::handleMsgPeerClientRx */


void SatelliteLink::handleMsgPeerClientStatus(std::istream& is)
{
  MsgPeerClientStatus msg;
  if (!msg.unpack(is))
  {
    geulog::error("satellite",
        "Failed to unpack MsgPeerClientStatus from '",
        m_satellite_id, "'");
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
        m_satellite_id, msg.callsign(), "status", status_json, true);
  }
} /* SatelliteLink::handleMsgPeerClientStatus */
