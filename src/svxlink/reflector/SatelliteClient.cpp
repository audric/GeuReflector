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
  sendMsg(MsgTrunkTalkerStart(tg, callsign));
} /* SatelliteClient::onLocalTalkerStart */


void SatelliteClient::onLocalTalkerStop(uint32_t tg)
{
  if (!m_con.isConnected() || !m_hello_received) return;
  if (!m_filter.empty() && !m_filter.matches(tg)) return;
  sendMsg(MsgTrunkTalkerStop(tg));
} /* SatelliteClient::onLocalTalkerStop */


void SatelliteClient::onLocalAudio(uint32_t tg,
                                    const std::vector<uint8_t>& audio)
{
  if (!m_con.isConnected() || !m_hello_received) return;
  if (!m_filter.empty() && !m_filter.matches(tg)) return;
  sendMsg(MsgTrunkAudio(tg, audio));
} /* SatelliteClient::onLocalAudio */


void SatelliteClient::onLocalFlush(uint32_t tg)
{
  if (!m_con.isConnected() || !m_hello_received) return;
  if (!m_filter.empty() && !m_filter.matches(tg)) return;
  sendMsg(MsgTrunkFlush(tg));
} /* SatelliteClient::onLocalFlush */


void SatelliteClient::onConnected(void)
{
  geulog::info("satellite", "Connected to parent ", m_con.remoteHost(),
               ":", m_con.remotePort());

  m_hello_received = false;
  m_hb_tx_cnt = HEARTBEAT_TX_CNT_RESET;
  m_hb_rx_cnt = HEARTBEAT_RX_CNT_RESET;

  sendMsg(MsgTrunkHello(m_satellite_id, "", m_priority, m_secret,
                         MsgTrunkHello::ROLE_SATELLITE));

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
      header.type() != MsgTrunkHello::TYPE &&
      header.type() != MsgTrunkHeartbeat::TYPE)
  {
    return;
  }

  m_hb_rx_cnt = HEARTBEAT_RX_CNT_RESET;

  switch (header.type())
  {
    case MsgTrunkHeartbeat::TYPE:
      handleMsgTrunkHeartbeat();
      break;
    case MsgTrunkHello::TYPE:
      handleMsgTrunkHello(ss);
      break;
    case MsgTrunkTalkerStart::TYPE:
      handleMsgTrunkTalkerStart(ss);
      break;
    case MsgTrunkTalkerStop::TYPE:
      handleMsgTrunkTalkerStop(ss);
      break;
    case MsgTrunkAudio::TYPE:
      handleMsgTrunkAudio(ss);
      break;
    case MsgTrunkFlush::TYPE:
      handleMsgTrunkFlush(ss);
      break;
    default:
      break;
  }
} /* SatelliteClient::onFrameReceived */


void SatelliteClient::handleMsgTrunkHeartbeat(void)
{
} /* SatelliteClient::handleMsgTrunkHeartbeat */


void SatelliteClient::handleMsgTrunkHello(std::istream& is)
{
  MsgTrunkHello msg;
  if (!msg.unpack(is))
  {
    geulog::error("satellite", "Failed to unpack MsgTrunkHello");
    return;
  }

  if (!msg.verify(m_secret))
  {
    geulog::error("satellite", "Parent authentication failed");
    m_con.disconnect();
    return;
  }

  m_hello_received = true;
  geulog::info("satellite", "Parent authenticated (id='", msg.id(), "')");

  // Advertise our TG filter to the parent so it can skip TGs we
  // don't want. The parent applies the filter on its outbound path.
  sendFilter();
} /* SatelliteClient::handleMsgTrunkHello */


void SatelliteClient::sendFilter(void)
{
  if (m_filter_str.empty()) return;
  sendMsg(MsgTrunkFilter(m_filter_str));
  geulog::info("satellite", "Sent TG filter to parent: ", m_filter_str);
} /* SatelliteClient::sendFilter */


void SatelliteClient::handleMsgTrunkTalkerStart(std::istream& is)
{
  MsgTrunkTalkerStart msg;
  if (!msg.unpack(is)) return;

  // Register as trunk talker — fires trunkTalkerUpdated which
  // broadcasts MsgTalkerStart to local clients
  TGHandler::instance()->setTrunkTalkerForTG(msg.tg(), msg.callsign());
} /* SatelliteClient::handleMsgTrunkTalkerStart */


void SatelliteClient::handleMsgTrunkTalkerStop(std::istream& is)
{
  MsgTrunkTalkerStop msg;
  if (!msg.unpack(is)) return;

  TGHandler::instance()->clearTrunkTalkerForTG(msg.tg());
} /* SatelliteClient::handleMsgTrunkTalkerStop */


void SatelliteClient::handleMsgTrunkAudio(std::istream& is)
{
  MsgTrunkAudio msg;
  if (!msg.unpack(is)) return;

  if (msg.audio().empty()) return;

  MsgUdpAudio udp_msg(msg.audio());
  m_reflector->broadcastUdpMsg(udp_msg,
      ReflectorClient::TgFilter(msg.tg()));
} /* SatelliteClient::handleMsgTrunkAudio */


void SatelliteClient::handleMsgTrunkFlush(std::istream& is)
{
  MsgTrunkFlush msg;
  if (!msg.unpack(is)) return;

  m_reflector->broadcastUdpMsg(MsgUdpFlushSamples(),
      ReflectorClient::TgFilter(msg.tg()));
} /* SatelliteClient::handleMsgTrunkFlush */


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


void SatelliteClient::heartbeatTick(Async::Timer* t)
{
  if (--m_hb_tx_cnt == 0)
  {
    m_hb_tx_cnt = HEARTBEAT_TX_CNT_RESET;
    sendMsg(MsgTrunkHeartbeat());
  }

  if (--m_hb_rx_cnt == 0)
  {
    geulog::error("satellite", "Heartbeat timeout — disconnecting");
    m_con.disconnect();
  }
} /* SatelliteClient::heartbeatTick */
