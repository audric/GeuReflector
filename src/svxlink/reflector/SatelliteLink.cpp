#include <iostream>
#include <sstream>

#include "SatelliteLink.h"
#include "ReflectorMsg.h"
#include "Reflector.h"
#include "TGHandler.h"
#include "ReflectorClient.h"

using namespace std;
using namespace Async;


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
  return obj;
} /* SatelliteLink::statusJson */


void SatelliteLink::onParentTalkerStart(uint32_t tg,
                                         const std::string& callsign)
{
  if (!m_hello_received) return;
  sendMsg(MsgTrunkTalkerStart(tg, callsign));
} /* SatelliteLink::onParentTalkerStart */


void SatelliteLink::onParentTalkerStop(uint32_t tg)
{
  if (!m_hello_received) return;
  sendMsg(MsgTrunkTalkerStop(tg));
} /* SatelliteLink::onParentTalkerStop */


void SatelliteLink::onParentAudio(uint32_t tg,
                                   const std::vector<uint8_t>& audio)
{
  if (!m_hello_received) return;
  sendMsg(MsgTrunkAudio(tg, audio));
} /* SatelliteLink::onParentAudio */


void SatelliteLink::onParentFlush(uint32_t tg)
{
  if (!m_hello_received) return;
  sendMsg(MsgTrunkFlush(tg));
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
    cerr << "*** ERROR[SAT]: Failed to unpack satellite message header"
         << endl;
    return;
  }

  if (!m_hello_received &&
      header.type() != MsgTrunkHello::TYPE &&
      header.type() != MsgTrunkHeartbeat::TYPE)
  {
    cerr << "*** WARNING[SAT]: Ignoring message type=" << header.type()
         << " before hello" << endl;
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
      cerr << "*** WARNING[SAT]: Unknown message type=" << header.type()
           << endl;
      break;
  }
} /* SatelliteLink::onFrameReceived */


void SatelliteLink::handleMsgTrunkHeartbeat(void)
{
} /* SatelliteLink::handleMsgTrunkHeartbeat */


void SatelliteLink::handleMsgTrunkHello(std::istream& is)
{
  MsgTrunkHello msg;
  if (!msg.unpack(is))
  {
    cerr << "*** ERROR[SAT]: Failed to unpack MsgTrunkHello" << endl;
    return;
  }

  if (msg.id().empty())
  {
    cerr << "*** ERROR[SAT]: Satellite sent empty ID" << endl;
    m_con->disconnect();
    return;
  }

  if (msg.role() != MsgTrunkHello::ROLE_SATELLITE)
  {
    cerr << "*** ERROR[SAT]: Expected ROLE_SATELLITE from '" << msg.id()
         << "' but got role=" << (int)msg.role() << endl;
    m_con->disconnect();
    return;
  }

  if (!msg.verify(m_secret))
  {
    cerr << "*** ERROR[SAT]: Authentication failed for satellite '"
         << msg.id() << "'" << endl;
    m_con->disconnect();
    return;
  }

  m_satellite_id = msg.id();
  m_hello_received = true;

  cout << "SAT: Satellite '" << m_satellite_id
       << "' authenticated" << endl;
} /* SatelliteLink::handleMsgTrunkHello */


void SatelliteLink::handleMsgTrunkTalkerStart(std::istream& is)
{
  MsgTrunkTalkerStart msg;
  if (!msg.unpack(is)) return;

  uint32_t tg = msg.tg();

  // Register as trunk talker — fires trunkTalkerUpdated which notifies
  // local clients. Reflector::onTrunkTalkerUpdated also forwards to
  // other satellites and trunk peers.
  m_sat_active_tgs.insert(tg);
  TGHandler::instance()->setTrunkTalkerForTG(tg, msg.callsign());

  // Forward to trunk peers
  m_reflector->forwardSatelliteAudioToTrunks(tg, msg.callsign());
} /* SatelliteLink::handleMsgTrunkTalkerStart */


void SatelliteLink::handleMsgTrunkTalkerStop(std::istream& is)
{
  MsgTrunkTalkerStop msg;
  if (!msg.unpack(is)) return;

  uint32_t tg = msg.tg();
  m_sat_active_tgs.erase(tg);
  TGHandler::instance()->clearTrunkTalkerForTG(tg);

  // Forward stop to trunk peers
  m_reflector->forwardSatelliteStopToTrunks(tg);
} /* SatelliteLink::handleMsgTrunkTalkerStop */


void SatelliteLink::handleMsgTrunkAudio(std::istream& is)
{
  MsgTrunkAudio msg;
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
} /* SatelliteLink::handleMsgTrunkAudio */


void SatelliteLink::handleMsgTrunkFlush(std::istream& is)
{
  MsgTrunkFlush msg;
  if (!msg.unpack(is)) return;

  uint32_t tg = msg.tg();

  m_reflector->broadcastUdpMsg(MsgUdpFlushSamples(),
      ReflectorClient::TgFilter(tg));

  m_reflector->forwardSatelliteFlushToTrunks(tg);
  m_reflector->forwardFlushToSatellitesExcept(this, tg);
} /* SatelliteLink::handleMsgTrunkFlush */


void SatelliteLink::sendMsg(const ReflectorMsg& msg)
{
  ostringstream ss;
  ReflectorMsg header(msg.type());
  if (!header.pack(ss) || !msg.pack(ss))
  {
    cerr << "*** ERROR[SAT]: Failed to pack message type=" << msg.type()
         << endl;
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
    sendMsg(MsgTrunkHeartbeat());
  }

  if (--m_hb_rx_cnt == 0)
  {
    cerr << "*** ERROR[SAT '" << m_satellite_id
         << "']: Heartbeat timeout — disconnecting" << endl;
    m_heartbeat_timer.setEnable(false);
    linkFailed(this);
  }
} /* SatelliteLink::heartbeatTick */
