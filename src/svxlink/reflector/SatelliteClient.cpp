#include <iostream>
#include <sstream>
#include <random>

#include <AsyncTcpConnection.h>

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
    cerr << "*** ERROR: Missing SATELLITE_OF in [GLOBAL]" << endl;
    return false;
  }

  m_cfg.getValue("GLOBAL", "SATELLITE_PORT", m_parent_port);

  if (!m_cfg.getValue("GLOBAL", "SATELLITE_SECRET", m_secret) ||
      m_secret.empty())
  {
    cerr << "*** ERROR: Missing SATELLITE_SECRET in [GLOBAL]" << endl;
    return false;
  }

  // Use a satellite ID — hostname or configured name
  m_satellite_id = "satellite";
  m_cfg.getValue("GLOBAL", "SATELLITE_ID", m_satellite_id);

  cout << "SATELLITE: Connecting to parent " << m_parent_host
       << ":" << m_parent_port << " as '" << m_satellite_id << "'" << endl;

  m_con.addStaticSRVRecord(0, 0, 0, m_parent_port, m_parent_host);
  m_con.connect();

  return true;
} /* SatelliteClient::initialize */


void SatelliteClient::onLocalTalkerStart(uint32_t tg,
                                          const std::string& callsign)
{
  if (!m_con.isConnected() || !m_hello_received) return;
  sendMsg(MsgTrunkTalkerStart(tg, callsign));
} /* SatelliteClient::onLocalTalkerStart */


void SatelliteClient::onLocalTalkerStop(uint32_t tg)
{
  if (!m_con.isConnected() || !m_hello_received) return;
  sendMsg(MsgTrunkTalkerStop(tg));
} /* SatelliteClient::onLocalTalkerStop */


void SatelliteClient::onLocalAudio(uint32_t tg,
                                    const std::vector<uint8_t>& audio)
{
  if (!m_con.isConnected() || !m_hello_received) return;
  sendMsg(MsgTrunkAudio(tg, audio));
} /* SatelliteClient::onLocalAudio */


void SatelliteClient::onLocalFlush(uint32_t tg)
{
  if (!m_con.isConnected() || !m_hello_received) return;
  sendMsg(MsgTrunkFlush(tg));
} /* SatelliteClient::onLocalFlush */


void SatelliteClient::onConnected(void)
{
  cout << "SATELLITE: Connected to parent " << m_con.remoteHost()
       << ":" << m_con.remotePort() << endl;

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
  cout << "SATELLITE: Disconnected from parent: "
       << TcpConnection::disconnectReasonStr(reason) << endl;

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
    cerr << "*** ERROR[SATELLITE]: Failed to unpack message header" << endl;
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
    cerr << "*** ERROR[SATELLITE]: Failed to unpack MsgTrunkHello" << endl;
    return;
  }

  if (!msg.verify(m_secret))
  {
    cerr << "*** ERROR[SATELLITE]: Parent authentication failed" << endl;
    m_con.disconnect();
    return;
  }

  m_hello_received = true;
  cout << "SATELLITE: Parent authenticated (id='" << msg.id() << "')" << endl;
} /* SatelliteClient::handleMsgTrunkHello */


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
    cerr << "*** ERROR[SATELLITE]: Failed to pack message type="
         << msg.type() << endl;
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
    cerr << "*** ERROR[SATELLITE]: Heartbeat timeout — disconnecting" << endl;
    m_con.disconnect();
  }
} /* SatelliteClient::heartbeatTick */
